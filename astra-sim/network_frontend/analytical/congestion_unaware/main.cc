/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/common/Logging.hh"
#include "astra-sim/workload/PreparedFeeder.hh"
#include "common/CmdLineParser.hh"
#include "common/WorkloadIpcServer.hh"
#include "congestion_unaware/CongestionUnawareNetworkApi.hh"
#include <astra-network-analytical/common/EventQueue.h>
#include <astra-network-analytical/common/NetworkParser.h>
#include <astra-network-analytical/congestion_unaware/Helper.h>
#include <memory_backend/analytical/AnalyticalMemory.hh>
#include <json/json.hpp>
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#include <iostream>
#include <sstream>

using namespace AstraSim;
using namespace Analytical;
using namespace AstraSimAnalytical;
using namespace AstraSimAnalyticalCongestionUnaware;
using namespace NetworkAnalytical;
using namespace NetworkAnalyticalCongestionUnaware;
using namespace std;
using json = nlohmann::json;


static std::string save_json_to_tmp(const json& j, const std::string& name) {
  const char* dir = "tmp__mem";
  if (::mkdir(dir, 0755) == -1) {
    if (errno != EEXIST) {
      std::perror("mkdir tmp_mem");
      std::exit(1);
    }
  }
  std::string path = std::string(dir) + "/" + name + ".json";
  std::ofstream ofs(path);
  if (!ofs) {
    std::cerr << "Unable to write tmp file: " << path << "\n";
    std::exit(1);
  }
  ofs << j.dump(2);
  return path;
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    auto cmd_line_parser = CmdLineParser(argv[0]);
    cmd_line_parser.parse(argc, argv);

    // Get command line arguments
    const auto workload_configuration =
        cmd_line_parser.get<std::string>("workload-configuration");
    const auto comm_group_configuration =
        cmd_line_parser.get<std::string>("comm-group-configuration");
    const auto system_configuration =
        cmd_line_parser.get<std::string>("system-configuration");
    const auto memory_configuration =
        cmd_line_parser.get<std::string>("memory-configuration");
    const auto network_configuration =
        cmd_line_parser.get<std::string>("network-configuration");
    const auto logging_configuration =
        cmd_line_parser.get<std::string>("logging-configuration");
    const auto num_queues_per_dim =
        cmd_line_parser.get<int>("num-queues-per-dim");
    const auto comm_scale = cmd_line_parser.get<double>("comm-scale");
    const auto injection_scale = cmd_line_parser.get<double>("injection-scale");
    const auto rendezvous_protocol =
        cmd_line_parser.get<bool>("rendezvous-protocol");
    auto start_npu_ids =
        cmd_line_parser.get<std::vector<int>>("start-npu-ids");
    auto end_npu_ids =
        cmd_line_parser.get<std::vector<int>>("end-npu-ids");
    const auto workload_ipc_socket =
        cmd_line_parser.get<std::string>("workload-ipc-socket");

    // clear vector if default value is used
    if (start_npu_ids.size() == 1 && start_npu_ids[0] == -1) {
      start_npu_ids.clear();
    }
    if (end_npu_ids.size() == 1 && end_npu_ids[0] == -1) {
      end_npu_ids.clear();
    }

    AstraSim::LoggerFactory::init(logging_configuration);

    // Instantiate event queue
    const auto event_queue = std::make_shared<EventQueue>();

    // Generate topology
    const auto network_parser = NetworkParser(network_configuration);
    const auto topology = construct_topology(network_parser);

    // Get topology information
    const auto npus_count = topology->get_npus_count();
    const auto npus_count_per_dim = topology->get_npus_count_per_dim();
    const auto dims_count = topology->get_dims_count();

    // Set up Network API
    CongestionUnawareNetworkApi::set_event_queue(event_queue);
    CongestionUnawareNetworkApi::set_topology(topology);

    // Create ASTRA-sim related resources
    auto network_apis =
        std::vector<std::unique_ptr<CongestionUnawareNetworkApi>>();
    
    json mem_json;
    std::ifstream rm_ifs(memory_configuration);
    rm_ifs >> mem_json;

    std::vector<std::unique_ptr<AnalyticalMemory>> memory_levels;

    // Check if the configuration is for a single memory type
    const bool is_single =
      mem_json.is_object() &&
      mem_json.contains("memory-type") &&
      mem_json.contains("mem-latency") &&
      mem_json.contains("mem-bw");
    
    if (is_single) {
      std::cout << "Single Memory Configuration Detected" << std::endl;
      memory_levels.push_back(std::make_unique<AnalyticalMemory>(memory_configuration));
    } else {
      // local memory
      if (mem_json.contains("local_mem") && mem_json["local_mem"].is_object()) {
        json j = mem_json["local_mem"];
        j["memory-location"] = "LOCAL_MEMORY";
        auto path = save_json_to_tmp(j, "local_mem");
        memory_levels.push_back(std::make_unique<AnalyticalMemory>(path));
        std::remove(path.c_str()); 
      }

      // remote memory
      if (mem_json.contains("remote_mem") && mem_json["remote_mem"].is_object()) {
        json j = mem_json["remote_mem"];
        j["memory-location"] = "REMOTE_MEMORY";
        auto path = save_json_to_tmp(j, "remote_mem");
        memory_levels.push_back(std::make_unique<AnalyticalMemory>(path));
        std::remove(path.c_str()); 
      }

      // cxl memory
      if (mem_json.contains("cxl_mem") && mem_json["cxl_mem"].is_object()) {
        json j = mem_json["cxl_mem"];
        j["memory-location"] = "CXL_MEMORY";
        auto path = save_json_to_tmp(j, "cxl_mem");
        memory_levels.push_back(std::make_unique<AnalyticalMemory>(path));
        std::remove(path.c_str()); 
      }

      ::rmdir("tmp_mem");
    }

    auto memory_apis = std::vector<AstraMemoryAPI*>();
    for (auto& mem_api : memory_levels) {
      memory_apis.push_back(mem_api.get());
    }

    auto systems = std::vector<Sys*>();

    auto queues_per_dim = std::vector<int>();
    for (auto i = 0; i < dims_count; i++) {
        queues_per_dim.push_back(num_queues_per_dim);
    }

    for (int i = 0; i < npus_count; i++) {
        // create network and system
        auto network_api = std::make_unique<CongestionUnawareNetworkApi>(i);
        auto* const system =
            new Sys(i, workload_configuration, comm_group_configuration,
                    system_configuration, memory_apis, network_api.get(),
                    npus_count_per_dim, queues_per_dim, injection_scale,
                    comm_scale, rendezvous_protocol);

        // push back network and system
        network_apis.push_back(std::move(network_api));
        systems.push_back(system);
    }

    // Map instance NPU IDs for proper workload management
    // Precompute the systems handled by each controller NPU
    std::vector<std::vector<Sys*>> managed_systems(start_npu_ids.size());

    for (std::size_t idx = 0; idx < start_npu_ids.size(); ++idx) {
      int npu_id = start_npu_ids[idx];

      // Determine the upper bound for this controller:
      // - If there's a next controller, stop before it
      // - Otherwise, go until npus_count
      int upper_bound_id;
      if (idx + 1 < start_npu_ids.size()) {
        upper_bound_id = start_npu_ids[idx + 1];
      } else {
        upper_bound_id = npus_count;  // last controller handles until the end
      }

      // Collect systems in the range (npu_id+1 .. upper_bound_id-1)
      for (int sid = npu_id + 1; sid < upper_bound_id; ++sid) {
        if (sid < 0 || sid >= npus_count) {
            AstraSim::LoggerFactory::get_logger("workload")
                ->critical("Skipping invalid system id {} while building managed_systems", sid);
        }
        if (std::find(end_npu_ids.begin(), end_npu_ids.end(), sid) != end_npu_ids.end()) {
          continue;
        }
        managed_systems[idx].push_back(systems[sid]);
      }
    }

    std::unique_ptr<WorkloadIpcServer> workload_ipc_server;
    if (!workload_ipc_socket.empty()) {
      workload_ipc_server =
          std::make_unique<WorkloadIpcServer>(workload_ipc_socket);
      workload_ipc_server->start();
      workload_ipc_server->accept_client();
      workload_ipc_server->handshake();
    }
    auto read_workload_command = [&workload_ipc_server]() {
      if (workload_ipc_server) {
        return workload_ipc_server->receive_file_command();
      }
      std::string command;
      std::getline(std::cin, command);
      return command;
    };
    auto decode_workload_command = [](std::string command) {
      if (command.empty() || command.front() != '@') {
        throw std::invalid_argument(
            "Legacy workload command has no target system id");
      }
      const auto separator = command.find('\t');
      if (separator == std::string::npos || separator <= 1) {
        throw std::invalid_argument(
            "Legacy workload command has invalid target framing");
      }
      const auto system_id = std::stoi(command.substr(1, separator - 1));
      return std::make_pair(system_id, command.substr(separator + 1));
    };
    auto has_pending_workload_command = [&workload_ipc_server]() {
      if (workload_ipc_server) {
        return workload_ipc_server->has_pending_input();
      }
      pollfd descriptor{};
      descriptor.fd = STDIN_FILENO;
      descriptor.events = POLLIN;
      const auto result = ::poll(&descriptor, 1, 0);
      return result > 0 && (descriptor.revents & POLLIN) != 0;
    };
    auto wait_for_workload_command = [&workload_ipc_server]() {
      if (workload_ipc_server) {
        workload_ipc_server->wait_for_input();
        return;
      }
      pollfd descriptor{};
      descriptor.fd = STDIN_FILENO;
      descriptor.events = POLLIN;
      while (::poll(&descriptor, 1, -1) < 0 && errno == EINTR) {
      }
    };
    auto install_prepared_batch = [&workload_ipc_server, &systems,
                                   npus_count]() {
      auto prepared = workload_ipc_server->take_prepared_batch();
      std::vector<AstraSim::Workload*> workloads;
      workloads.reserve(prepared.systems.size());
      for (auto& prepared_system : prepared.systems) {
        if (prepared_system.system_id >= static_cast<std::uint32_t>(npus_count)) {
          throw std::out_of_range("Prepared batch system id is out of range");
        }
        auto* workload = systems[prepared_system.system_id]->workload;
        workload->install_prepared_workload(
            std::make_unique<AstraSim::PreparedFeeder>(
                std::move(prepared_system.iteration)));
        workloads.emplace_back(workload);
      }
      for (auto* workload : workloads) {
        workload->fire();
      }
    };
    auto apply_workload_command = [
        &systems, &start_npu_ids, &managed_systems,
        &install_prepared_batch, &workload_ipc_server, &event_queue,
        npus_count](
        int system_id, const std::string& command) {
      if (system_id < 0 || system_id >= npus_count) {
        throw std::out_of_range("Workload command system id is out of range");
      }
      if (command == "pass") {
        return false;
      }
      if (command.rfind("advance:", 0) == 0) {
        event_queue->advance_to(std::stoull(command.substr(8)));
        return false;
      }
      if (command == "exit") {
        return true;
      }
      if (command == "done") {
        systems[system_id]->workload->is_sleep = true;
        return false;
      }
      if (command == WorkloadIpcServer::kPreparedBatchCommand) {
        install_prepared_batch();
        return false;
      }
      if (!workload_ipc_server && command.rfind("wave:", 0) == 0) {
        const auto separator = command.find('\t');
        if (separator == std::string::npos || separator <= 5) {
          throw std::invalid_argument("Legacy RUN_WAVE framing is invalid");
        }
        const auto path = command.substr(separator + 1);
        std::stringstream participants(command.substr(5, separator - 5));
        std::string encoded_system_id;
        while (std::getline(participants, encoded_system_id, ',')) {
          const auto participant = std::stoi(encoded_system_id);
          if (participant < 0 || participant >= npus_count) {
            throw std::out_of_range(
                "Legacy RUN_WAVE participant is out of range");
          }
          systems[participant]->workload->add_workload(path, {});
        }
        return false;
      }

      std::vector<Sys*> managed;
      const auto controller = std::find(
          start_npu_ids.begin(), start_npu_ids.end(), system_id);
      if (controller != start_npu_ids.end()) {
        const auto index = static_cast<std::size_t>(
            std::distance(start_npu_ids.begin(), controller));
        if (workload_ipc_server) {
          managed = managed_systems[index];
        } else {
          const auto upper_bound =
              index + 1 < start_npu_ids.size()
                  ? start_npu_ids[index + 1]
                  : npus_count;
          for (int managed_id = system_id + 1;
               managed_id < upper_bound; ++managed_id) {
            managed.push_back(systems[managed_id]);
          }
        }
      }
      systems[system_id]->workload->add_workload(command, managed);
      return false;
    };

    // Initiate simulation
    for (int i = 0; i < npus_count; i++) {
        systems[i]->workload->fire();
        // For debugging
        // systems[i]->workload->et_feeder->printGraph();
    }

    // run simulation
    // while (!event_queue->finished()) {
    //     event_queue->proceed();
    // }

    bool exit = false;
    std::vector<std::int64_t> last_command_iteration(npus_count, -1);
    while (!exit) {
      if(!event_queue->finished()){
        event_queue->proceed();
      }
      else {
        const bool all_workloads_idle = std::all_of(
            systems.begin(), systems.end(), [](const Sys* system) {
              return system->workload->is_finished ||
                     system->workload->is_sleep;
            });
        const auto report_pending_for = [&](int system_id) {
          return (!systems[system_id]->workload->is_sleep &&
                  systems[system_id]->workload->is_finished &&
                  last_command_iteration[system_id] !=
                      systems[system_id]->workload->iteration) ||
                 (workload_ipc_server &&
                  workload_ipc_server->has_pending_completion(system_id));
        };
        const bool completion_report_pending = std::any_of(
            start_npu_ids.begin(), start_npu_ids.end(), report_pending_for) ||
            std::any_of(
                end_npu_ids.begin(), end_npu_ids.end(), report_pending_for);
        if (!completion_report_pending && has_pending_workload_command()) {
          auto command = decode_workload_command(read_workload_command());
          exit = apply_workload_command(command.first, command.second);
        } else if (workload_ipc_server &&
            !completion_report_pending &&
            !has_pending_workload_command()) {
          // IPC drives idle-time advancement explicitly. Never let host-side
          // graph preparation latency leak into simulated time while a
          // collective or another externally driven workload is waiting.
          wait_for_workload_command();
        } else if (all_workloads_idle &&
            !completion_report_pending &&
            !has_pending_workload_command()) {
          wait_for_workload_command();
        } else {
          event_queue->add_current_time();
        }
      }

      // Freeze every completion visible at this simulated timestamp before
      // applying any host command. Applying a fast direct-mode command while
      // scanning the remaining systems made BATCH_DONE ordering depend on
      // host/socket timing, which changed PD routing at equal timestamps.
      std::vector<int> completion_frontier;
      const auto collect_completion = [&](int npu_id) {
        if (std::find(completion_frontier.begin(), completion_frontier.end(),
                      npu_id) != completion_frontier.end()) {
          return;
        }
        const auto* workload = systems[npu_id]->workload;
        if (workload->is_sleep || !workload->is_finished) {
          return;
        }
        if (last_command_iteration[npu_id] == workload->iteration &&
            (!workload_ipc_server ||
             !workload_ipc_server->has_pending_completion(npu_id))) {
          return;
        }
        completion_frontier.push_back(npu_id);
      };
      for (int npu_id : end_npu_ids) {
        cout << "Checking End NPU " << npu_id << " ..." << endl;
        collect_completion(npu_id);
      }
      for (int npu_id : start_npu_ids) {
        cout << "Checking Managed Systems for Controller NPU " << npu_id
             << " ..." << endl;
        collect_completion(npu_id);
      }

      for (int npu_id : completion_frontier) {
        systems[npu_id]->workload->report();
        if (workload_ipc_server) {
          const auto cycles = Sys::boostedTick();
          workload_ipc_server->send_batch_done(
              npu_id, cycles,
              cycles - systems[npu_id]->workload->hw_resource->tics_gpu_ops);
        }
        AstraSim::LoggerFactory::get_logger("workload")->info("Waiting");
      }
      for (int npu_id : completion_frontier) {
        auto command = decode_workload_command(read_workload_command());
        last_command_iteration[npu_id] =
            systems[npu_id]->workload->iteration;
        exit = apply_workload_command(command.first, command.second);
        if (exit) {
          break;
        }
      }
    }

    // check non exited system
    cout << "Checking Non-Exited Systems ..." << endl;
    bool done = true;
    for (int npu_id = 0; npu_id < npus_count; npu_id++) {

      if (systems[npu_id]->workload->is_finished == false){
        cout << "sys[" << npu_id << "] " << endl;
        systems[npu_id]->workload->et_feeder->printGraph();
        done = false;
      }
    }
    if (done){
      cout << "---------------------------" << endl;
      cout << "All Request Has Been Exited" << endl;
      cout << "---------------------------" << endl;
    }
    else{
      cout << "---------------------------" << endl;
      cout << "ERROR: Some Requests Remain" << endl;
      cout << "---------------------------" << endl;
    }

    // terminate simulation
    AstraSim::LoggerFactory::shutdown();
    return 0;
}
