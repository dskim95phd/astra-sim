/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/WorkloadIpcServer.hh"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <utility>

namespace AstraSimAnalytical {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic = {'L', 'S', 'I', 'M'};
constexpr std::uint16_t kProtocolVersion = 2;
constexpr std::size_t kHeaderSize = 16;
constexpr std::uint64_t kMaxPayloadBytes = 64ULL * 1024ULL * 1024ULL;

void read_exact(int fd, std::uint8_t* destination, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const auto result = ::read(fd, destination + offset, size - offset);
        if (result == 0) {
            throw std::runtime_error("Unexpected EOF on workload IPC socket");
        }
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(
                std::string("Failed to read workload IPC socket: ") +
                std::strerror(errno));
        }
        offset += static_cast<std::size_t>(result);
    }
}

void write_all(int fd, const std::uint8_t* source, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const auto result = ::write(fd, source + offset, size - offset);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(
                std::string("Failed to write workload IPC socket: ") +
                std::strerror(errno));
        }
        offset += static_cast<std::size_t>(result);
    }
}

std::uint16_t read_u16(const std::uint8_t* source) {
    return (static_cast<std::uint16_t>(source[0]) << 8) |
           static_cast<std::uint16_t>(source[1]);
}

std::uint64_t read_u64(const std::uint8_t* source) {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8) | static_cast<std::uint64_t>(source[index]);
    }
    return value;
}

void write_u16(std::uint8_t* destination, std::uint16_t value) {
    destination[0] = static_cast<std::uint8_t>((value >> 8) & 0xff);
    destination[1] = static_cast<std::uint8_t>(value & 0xff);
}

void write_u64(std::uint8_t* destination, std::uint64_t value) {
    for (int index = 7; index >= 0; --index) {
        destination[index] = static_cast<std::uint8_t>(value & 0xff);
        value >>= 8;
    }
}

std::string targeted_command(std::uint32_t system_id,
                             const std::string& command) {
    return "@" + std::to_string(system_id) + "\t" + command;
}

llmservingsim::ipc::SystemCommand parse_system_command(
    const std::vector<std::uint8_t>& payload,
    const char* command_name) {
    llmservingsim::ipc::SystemCommand request;
    if (!request.ParseFromArray(payload.data(), payload.size()) ||
        request.request_id() == 0) {
        throw std::runtime_error(
            std::string(command_name) + " payload is not valid protobuf");
    }
    return request;
}

}  // namespace

WorkloadIpcServer::WorkloadIpcServer(std::string socket_path)
    : socket_path_(std::move(socket_path)) {}

WorkloadIpcServer::~WorkloadIpcServer() {
    close_descriptors();
    if (!socket_path_.empty()) {
        ::unlink(socket_path_.c_str());
    }
}

void WorkloadIpcServer::start() {
    if (socket_path_.empty()) {
        throw std::invalid_argument("Workload IPC socket path is empty");
    }
    if (socket_path_.size() >= sizeof(sockaddr_un::sun_path)) {
        throw std::invalid_argument("Workload IPC socket path is too long");
    }

    ::unlink(socket_path_.c_str());
    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error(
            std::string("Failed to create workload IPC socket: ") +
            std::strerror(errno));
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path_.c_str(),
                 sizeof(address.sun_path) - 1);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) < 0) {
        throw std::runtime_error(
            std::string("Failed to bind workload IPC socket: ") +
            std::strerror(errno));
    }
    if (::listen(listen_fd_, 1) < 0) {
        throw std::runtime_error(
            std::string("Failed to listen on workload IPC socket: ") +
            std::strerror(errno));
    }
    std::cout << "Workload IPC listening on " << socket_path_ << std::endl;
}

void WorkloadIpcServer::accept_client() {
    while (client_fd_ < 0) {
        client_fd_ = ::accept(listen_fd_, nullptr, nullptr);
        if (client_fd_ < 0 && errno != EINTR) {
            throw std::runtime_error(
                std::string("Failed to accept workload IPC client: ") +
                std::strerror(errno));
        }
    }
}

void WorkloadIpcServer::handshake() {
    const auto frame = receive_frame();
    if (frame.type != WorkloadMessageType::Hello || !frame.payload.empty()) {
        send_error("Expected an empty HELLO frame");
        throw std::runtime_error("Invalid workload IPC HELLO frame");
    }
    send_frame(WorkloadMessageType::Hello);
}

WorkloadFrame WorkloadIpcServer::receive_frame() {
    if (client_fd_ < 0) {
        throw std::runtime_error("No workload IPC client is connected");
    }

    std::array<std::uint8_t, kHeaderSize> header{};
    read_exact(client_fd_, header.data(), header.size());
    for (std::size_t index = 0; index < kMagic.size(); ++index) {
        if (header[index] != kMagic[index]) {
            throw std::runtime_error("Invalid workload IPC protocol magic");
        }
    }

    const auto version = read_u16(header.data() + 4);
    if (version != kProtocolVersion) {
        send_error("Unsupported workload IPC protocol version");
        throw std::runtime_error("Unsupported workload IPC protocol version");
    }
    const auto raw_type = read_u16(header.data() + 6);
    const auto payload_size = read_u64(header.data() + 8);
    if (payload_size > kMaxPayloadBytes ||
        payload_size > std::numeric_limits<std::size_t>::max()) {
        send_error("Workload IPC payload exceeds the configured limit");
        throw std::runtime_error("Workload IPC payload is too large");
    }

    WorkloadFrame frame{
        static_cast<WorkloadMessageType>(raw_type),
        std::vector<std::uint8_t>(static_cast<std::size_t>(payload_size)),
    };
    if (!frame.payload.empty()) {
        read_exact(client_fd_, frame.payload.data(), frame.payload.size());
    }
    return frame;
}

void WorkloadIpcServer::send_frame(
    WorkloadMessageType type, const std::vector<std::uint8_t>& payload) {
    if (client_fd_ < 0) {
        throw std::runtime_error("No workload IPC client is connected");
    }
    std::array<std::uint8_t, kHeaderSize> header{};
    std::copy(kMagic.begin(), kMagic.end(), header.begin());
    write_u16(header.data() + 4, kProtocolVersion);
    write_u16(header.data() + 6, static_cast<std::uint16_t>(type));
    write_u64(header.data() + 8, payload.size());
    write_all(client_fd_, header.data(), header.size());
    if (!payload.empty()) {
        write_all(client_fd_, payload.data(), payload.size());
    }
}

std::string WorkloadIpcServer::receive_file_command() {
    // The next command is requested only after the previous legacy workload
    // completed, so its transitional prepared state can now be released.
    prepared_batch_.reset();
    while (true) {
      const auto frame = receive_frame();
      switch (frame.type) {
        case WorkloadMessageType::RegisterTemplate:
            register_template(frame.payload);
            continue;
        case WorkloadMessageType::RunBatch:
            {
              llmservingsim::ipc::RunBatch request;
              if (!request.ParseFromArray(
                      frame.payload.data(), frame.payload.size()) ||
                  request.request_id() == 0) {
                  send_error("RUN_BATCH payload is not valid protobuf");
                  throw std::runtime_error("Invalid RUN_BATCH payload");
              }
              if (prepare_batch(frame.payload)) {
                  return targeted_command(
                      request.controller_system_id(), kPreparedBatchCommand);
              }
            }
            continue;
        case WorkloadMessageType::RunWave:
            {
              llmservingsim::ipc::RunWave request;
              if (!request.ParseFromArray(
                      frame.payload.data(), frame.payload.size()) ||
                  request.request_id() == 0) {
                  send_error("RUN_WAVE payload is not valid protobuf");
                  throw std::runtime_error("Invalid RUN_WAVE payload");
              }
              if (prepare_wave(frame.payload)) {
                  return targeted_command(
                      request.controller_system_id(), kPreparedBatchCommand);
              }
            }
            continue;
        case WorkloadMessageType::FileWorkload: {
            llmservingsim::ipc::FileWorkload request;
            if (!request.ParseFromArray(
                    frame.payload.data(), frame.payload.size()) ||
                request.request_id() == 0 || request.path().empty()) {
                send_error("FILE_WORKLOAD payload is not valid protobuf");
                throw std::runtime_error("Invalid FILE_WORKLOAD payload");
            }
            return targeted_command(request.system_id(), request.path());
        }
        case WorkloadMessageType::Pass: {
            try {
                const auto request = parse_system_command(
                    frame.payload, "PASS");
                return targeted_command(request.system_id(), "pass");
            } catch (const std::exception& error) {
                send_error(error.what());
                throw;
            }
        }
        case WorkloadMessageType::Sleep: {
            try {
                const auto request = parse_system_command(
                    frame.payload, "SLEEP");
                return targeted_command(request.system_id(), "done");
            } catch (const std::exception& error) {
                send_error(error.what());
                throw;
            }
        }
        case WorkloadMessageType::Exit: {
            try {
                const auto request = parse_system_command(
                    frame.payload, "EXIT");
                return targeted_command(request.system_id(), "exit");
            } catch (const std::exception& error) {
                send_error(error.what());
                throw;
            }
        }
        case WorkloadMessageType::AdvanceTime: {
            llmservingsim::ipc::AdvanceTime request;
            if (!request.ParseFromArray(
                    frame.payload.data(), frame.payload.size()) ||
                request.request_id() == 0) {
                send_error("ADVANCE_TIME payload is not valid protobuf");
                throw std::runtime_error("Invalid ADVANCE_TIME payload");
            }
            return targeted_command(
                request.system_id(),
                "advance:" + std::to_string(request.current_time_ns()));
        }
        default:
            send_error("Message is not supported by the file bridge");
            throw std::runtime_error(
                "Unsupported workload IPC file-bridge message");
      }
    }
}

PreparedBatch WorkloadIpcServer::take_prepared_batch() {
    if (!prepared_batch_.has_value()) {
        throw std::runtime_error("No prepared batch is available");
    }
    auto prepared = std::move(*prepared_batch_);
    prepared_batch_.reset();
    return prepared;
}

bool WorkloadIpcServer::prepare_batch(
    const std::vector<std::uint8_t>& payload) {
    if (prepared_batch_.has_value()) {
        send_error("A prepared batch is already waiting for execution");
        throw std::runtime_error("Duplicate prepared batch");
    }
    llmservingsim::ipc::RunBatch request;
    if (!request.ParseFromArray(payload.data(), payload.size())) {
        send_error("RUN_BATCH payload is not valid protobuf");
        throw std::runtime_error("Invalid RUN_BATCH payload");
    }

    try {
        prepared_batch_.emplace(template_registry_.prepare_batch(request));
        if (request.execute()) {
            for (const auto& system : prepared_batch_->systems) {
                pending_completions_.insert_or_assign(
                    system.system_id,
                    PendingCompletion{
                        prepared_batch_->request_id,
                        prepared_batch_->batch_id,
                    });
            }
        }
        llmservingsim::ipc::BatchAccepted response;
        response.set_request_id(prepared_batch_->request_id);
        response.set_batch_id(prepared_batch_->batch_id);
        response.set_template_id(prepared_batch_->template_id);
        response.set_system_count(prepared_batch_->systems.size());
        response.set_patched_value_count(
            prepared_batch_->patched_value_count);
        const auto bytes = response.SerializeAsString();
        send_frame(
            WorkloadMessageType::BatchAccepted,
            std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
        return request.execute();
    } catch (const std::exception& error) {
        if (prepared_batch_.has_value()) {
            for (const auto& system : prepared_batch_->systems) {
                pending_completions_.erase(system.system_id);
            }
        }
        prepared_batch_.reset();
        send_error(error.what());
        throw;
    }
}

bool WorkloadIpcServer::prepare_wave(
    const std::vector<std::uint8_t>& payload) {
    if (prepared_batch_.has_value()) {
        send_error("A prepared batch is already waiting for execution");
        throw std::runtime_error("Duplicate prepared wave");
    }
    llmservingsim::ipc::RunWave request;
    if (!request.ParseFromArray(payload.data(), payload.size())) {
        send_error("RUN_WAVE payload is not valid protobuf");
        throw std::runtime_error("Invalid RUN_WAVE payload");
    }
    if (request.runs().empty()) {
        send_error("RUN_WAVE contains no participants");
        throw std::runtime_error("Empty RUN_WAVE");
    }

    try {
        PreparedBatch merged{
            request.request_id(),
            request.wave_id(),
            0,
        };
        std::unordered_map<std::uint32_t, PendingCompletion> completions;
        for (const auto& run : request.runs()) {
            llmservingsim::ipc::RunBatch batch_request;
            batch_request.set_request_id(request.request_id());
            batch_request.set_instance_id(run.instance_id());
            batch_request.set_execute(true);
            batch_request.mutable_patch()->CopyFrom(run.patch());
            auto participant = template_registry_.prepare_batch(batch_request);
            merged.patched_value_count += participant.patched_value_count;
            for (auto& system : participant.systems) {
                if (completions.count(system.system_id) != 0) {
                    throw std::invalid_argument(
                        "RUN_WAVE participants overlap on a system");
                }
                completions.emplace(
                    system.system_id,
                    PendingCompletion{
                        request.request_id(), participant.batch_id});
                merged.systems.push_back(std::move(system));
            }
        }

        // Commit only after every participant has validated successfully.
        prepared_batch_.emplace(std::move(merged));
        for (const auto& completion : completions) {
            pending_completions_.insert_or_assign(
                completion.first, completion.second);
        }
        llmservingsim::ipc::BatchAccepted response;
        response.set_request_id(request.request_id());
        response.set_batch_id(request.wave_id());
        response.set_template_id(0);
        response.set_system_count(prepared_batch_->systems.size());
        response.set_patched_value_count(
            prepared_batch_->patched_value_count);
        const auto bytes = response.SerializeAsString();
        send_frame(
            WorkloadMessageType::BatchAccepted,
            std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
        return true;
    } catch (const std::exception& error) {
        prepared_batch_.reset();
        send_error(error.what());
        throw;
    }
}

bool WorkloadIpcServer::send_batch_done(
    std::uint32_t system_id,
    std::uint64_t cycles,
    std::uint64_t exposed_communication_cycles) {
    const auto completion = pending_completions_.find(system_id);
    if (completion == pending_completions_.end()) {
        return false;
    }
    llmservingsim::ipc::BatchDone response;
    response.set_request_id(completion->second.request_id);
    response.set_system_id(system_id);
    response.set_batch_id(completion->second.batch_id);
    response.set_cycles(cycles);
    response.set_exposed_communication_cycles(
        exposed_communication_cycles);
    const auto bytes = response.SerializeAsString();
    send_frame(
        WorkloadMessageType::BatchDone,
        std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
    pending_completions_.erase(completion);
    return true;
}

bool WorkloadIpcServer::has_pending_completion(
    std::uint32_t system_id) const {
    return pending_completions_.count(system_id) != 0;
}

bool WorkloadIpcServer::has_pending_input() const {
    if (client_fd_ < 0) {
        return false;
    }
    pollfd descriptor{};
    descriptor.fd = client_fd_;
    descriptor.events = POLLIN;
    const auto result = ::poll(&descriptor, 1, 0);
    return result > 0 && (descriptor.revents & POLLIN) != 0;
}

void WorkloadIpcServer::wait_for_input() const {
    if (client_fd_ < 0) {
        return;
    }
    pollfd descriptor{};
    descriptor.fd = client_fd_;
    descriptor.events = POLLIN;
    while (::poll(&descriptor, 1, -1) < 0) {
        if (errno != EINTR) {
            throw std::runtime_error(
                std::string("Failed waiting for workload IPC input: ") +
                std::strerror(errno));
        }
    }
}

void WorkloadIpcServer::register_template(
    const std::vector<std::uint8_t>& payload) {
    llmservingsim::ipc::RegisterTemplate request;
    if (!request.ParseFromArray(payload.data(), payload.size())) {
        send_error("REGISTER_TEMPLATE payload is not valid protobuf");
        throw std::runtime_error("Invalid REGISTER_TEMPLATE payload");
    }

    try {
        const auto response = template_registry_.register_template(request);
        const auto bytes = response.SerializeAsString();
        send_frame(
            WorkloadMessageType::TemplateReady,
            std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
    } catch (const std::exception& error) {
        send_error(error.what());
        throw;
    }
}

void WorkloadIpcServer::send_error(const std::string& message) {
    send_frame(
        WorkloadMessageType::Error,
        std::vector<std::uint8_t>(message.begin(), message.end()));
}

void WorkloadIpcServer::close_descriptors() noexcept {
    if (client_fd_ >= 0) {
        ::close(client_fd_);
        client_fd_ = -1;
    }
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

}  // namespace AstraSimAnalytical
