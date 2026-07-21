/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/TemplateRegistry.hh"

namespace AstraSimAnalytical {

enum class WorkloadMessageType : std::uint16_t {
    Hello = 1,
    RegisterTemplate = 2,
    TemplateReady = 3,
    RunBatch = 4,
    RunWave = 5,
    BatchDone = 6,
    Pass = 7,
    Sleep = 8,
    Exit = 9,
    Error = 10,
    FileWorkload = 11,
    BatchAccepted = 12,
    AdvanceTime = 13,
};

struct WorkloadFrame {
    WorkloadMessageType type;
    std::vector<std::uint8_t> payload;
};

class WorkloadIpcServer {
  public:
    static constexpr const char* kPreparedBatchCommand = "@prepared_batch";

    explicit WorkloadIpcServer(std::string socket_path);
    ~WorkloadIpcServer();

    WorkloadIpcServer(const WorkloadIpcServer&) = delete;
    WorkloadIpcServer& operator=(const WorkloadIpcServer&) = delete;

    void start();
    void accept_client();
    void handshake();
    WorkloadFrame receive_frame();
    void send_frame(WorkloadMessageType type,
                    const std::vector<std::uint8_t>& payload = {});
    bool send_batch_done(std::uint32_t system_id,
                         std::uint64_t cycles,
                         std::uint64_t exposed_communication_cycles);
    bool has_pending_completion(std::uint32_t system_id) const;
    bool has_pending_input() const;
    void wait_for_input() const;
    std::string receive_file_command();
    PreparedBatch take_prepared_batch();

  private:
    void send_error(const std::string& message);
    void register_template(const std::vector<std::uint8_t>& payload);
    bool prepare_batch(const std::vector<std::uint8_t>& payload);
    bool prepare_wave(const std::vector<std::uint8_t>& payload);
    void close_descriptors() noexcept;

    std::string socket_path_;
    int listen_fd_ = -1;
    int client_fd_ = -1;
    TemplateRegistry template_registry_;
    std::optional<PreparedBatch> prepared_batch_;
    struct PendingCompletion {
        std::uint64_t request_id;
        std::uint64_t batch_id;
    };
    std::unordered_map<std::uint32_t, PendingCompletion>
        pending_completions_;
};

}  // namespace AstraSimAnalytical
