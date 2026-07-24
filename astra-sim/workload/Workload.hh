/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __WORKLOAD_HH__
#define __WORKLOAD_HH__

#include <memory>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <queue>
#include <utility>

#include "astra-sim/system/Callable.hh"
#include "astra-sim/system/CommunicatorGroup.hh"
#include "astra-sim/workload/HardwareResource.hh"
#include "astra-sim/workload/WorkloadFeeder.hh"

namespace AstraSim {

class Sys;
class DataSet;

struct PreparedWorkloadCompletion {
    uint32_t iteration;
    uint64_t cycles;
    uint64_t exposed_communication_cycles;
};

class Workload : public Callable {
  public:
    Workload(Sys* sys,
             std::string et_filename,
             std::string comm_group_filename);
    ~Workload();

    // communicator groups
    void initialize_comm_group(std::string comm_group_filename);

    // event-based simulation
    void issue_dep_free_nodes();
    void issue(std::shared_ptr<Chakra::ETFeederNode> node);
    void issue_replay(std::shared_ptr<Chakra::ETFeederNode> node);
    // void issue_remote_mem(std::shared_ptr<Chakra::ETFeederNode> node); integrated into issue_mem
    void issue_mem(std::shared_ptr<Chakra::ETFeederNode> node);
    void issue_comp(std::shared_ptr<Chakra::ETFeederNode> node);
    void issue_comm(std::shared_ptr<Chakra::ETFeederNode> node);
    void skip_invalid(std::shared_ptr<Chakra::ETFeederNode> node);
    void call(EventType event, CallData* data);
    void fire();
    void add_workload(const std::string& new_filename, const std::vector<Sys*>& systems);
    bool install_prepared_workload(
        std::unique_ptr<WorkloadFeeder> feeder,
        bool report_completion);
    bool has_prepared_completion() const;
    PreparedWorkloadCompletion take_prepared_completion();
    void sleep_workload(const std::vector<Sys*>& systems);

    // stats
    void report();
    void report(const PreparedWorkloadCompletion& completion);

    WorkloadFeeder* et_feeder;
    CommunicatorGroup* comm_group;
    HardwareResource* hw_resource;
    Sys* sys;
    std::unordered_map<int, uint64_t> collective_comm_node_id_map;
    std::unordered_map<int, DataSet*> collective_comm_wrapper_map;
    bool is_finished;
    uint32_t iteration;
    std::string filename;

    bool is_sleep;
    std::queue<std::string> pending_workloads;
    std::queue<std::pair<std::unique_ptr<WorkloadFeeder>, bool>>
        pending_prepared_workloads;
    std::queue<PreparedWorkloadCompletion> prepared_completions;
    bool active_workload_is_prepared;
    bool report_active_prepared_completion;
};

}  // namespace AstraSim

#endif /* __WORKLOAD_HH__ */
