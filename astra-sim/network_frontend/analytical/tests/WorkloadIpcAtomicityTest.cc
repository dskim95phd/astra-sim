/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/WorkloadIpcServer.hh"

#include "et_def.pb.h"
#include "llmservingsim_workload.pb.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using AstraSimAnalytical::WorkloadFrame;
using AstraSimAnalytical::WorkloadIpcServer;
using AstraSimAnalytical::WorkloadMessageType;

constexpr std::array<std::uint8_t, 4> kMagic = {'L', 'S', 'I', 'M'};
constexpr std::uint16_t kProtocolVersion = 2;
constexpr std::size_t kHeaderSize = 16;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void write_all(int fd, const std::uint8_t* source, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const auto result = ::write(fd, source + offset, size - offset);
        if (result <= 0) {
            throw std::runtime_error("Failed to write test socket");
        }
        offset += static_cast<std::size_t>(result);
    }
}

void read_exact(int fd, std::uint8_t* destination, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const auto result = ::read(fd, destination + offset, size - offset);
        if (result <= 0) {
            throw std::runtime_error("Failed to read test socket");
        }
        offset += static_cast<std::size_t>(result);
    }
}

void write_u16(std::uint8_t* destination, std::uint16_t value) {
    destination[0] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    destination[1] = static_cast<std::uint8_t>(value & 0xffU);
}

void write_u64(std::uint8_t* destination, std::uint64_t value) {
    for (int index = 7; index >= 0; --index) {
        destination[index] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8U;
    }
}

std::uint16_t read_u16(const std::uint8_t* source) {
    return (static_cast<std::uint16_t>(source[0]) << 8U) |
           static_cast<std::uint16_t>(source[1]);
}

std::uint64_t read_u64(const std::uint8_t* source) {
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8U) | source[index];
    }
    return value;
}

void send_frame(int fd, WorkloadMessageType type, const std::string& payload) {
    std::array<std::uint8_t, kHeaderSize> header{};
    std::copy(kMagic.begin(), kMagic.end(), header.begin());
    write_u16(header.data() + 4, kProtocolVersion);
    write_u16(header.data() + 6, static_cast<std::uint16_t>(type));
    write_u64(header.data() + 8, payload.size());
    write_all(fd, header.data(), header.size());
    if (!payload.empty()) {
        write_all(
            fd, reinterpret_cast<const std::uint8_t*>(payload.data()),
            payload.size());
    }
}

WorkloadFrame receive_frame(int fd) {
    std::array<std::uint8_t, kHeaderSize> header{};
    read_exact(fd, header.data(), header.size());
    require(std::equal(kMagic.begin(), kMagic.end(), header.begin()),
            "Invalid response magic");
    require(read_u16(header.data() + 4) == kProtocolVersion,
            "Invalid response version");
    const auto payload_size = read_u64(header.data() + 8);
    std::vector<std::uint8_t> payload(payload_size);
    if (!payload.empty()) {
        read_exact(fd, payload.data(), payload.size());
    }
    return {
        static_cast<WorkloadMessageType>(read_u16(header.data() + 6)),
        std::move(payload),
    };
}

void append_varint(std::string& output, std::uint32_t value) {
    while (value >= 0x80U) {
        output.push_back(static_cast<char>((value & 0x7fU) | 0x80U));
        value >>= 7U;
    }
    output.push_back(static_cast<char>(value));
}

template <typename Message>
void append_message(std::string& output, const Message& message) {
    const auto bytes = message.SerializeAsString();
    append_varint(output, static_cast<std::uint32_t>(bytes.size()));
    output.append(bytes);
}

std::string make_graph() {
    ChakraProtoMsg::GlobalMetadata metadata;
    ChakraProtoMsg::Node node;
    node.set_id(1);
    node.set_name("atomicity_test");
    node.set_type(ChakraProtoMsg::COMP_NODE);
    node.set_duration_micros(1);
    std::string graph;
    append_message(graph, metadata);
    append_message(graph, node);
    return graph;
}

int connect_client(const std::string& socket_path) {
    const auto fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("Failed to create test client socket");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(
        address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) < 0) {
        ::close(fd);
        throw std::runtime_error("Failed to connect test client socket");
    }
    return fd;
}

}  // namespace

int main() {
    const auto socket_path =
        "/tmp/llmservingsim-wave-atomicity-" +
        std::to_string(static_cast<long long>(::getpid())) + ".sock";
    WorkloadIpcServer server(socket_path);
    server.start();

    std::exception_ptr server_error;
    std::thread server_thread([&]() {
        try {
            server.accept_client();
            server.handshake();
            bool rejected = false;
            try {
                server.receive_file_command();
            } catch (const std::exception&) {
                rejected = true;
            }
            require(rejected, "Malformed RUN_WAVE was not rejected");
            require(!server.has_pending_completion(0),
                    "Rejected RUN_WAVE left a participant pending");
            require(!server.send_batch_done(0, 10, 0),
                    "Rejected RUN_WAVE emitted a partial completion");
            bool has_prepared_batch = true;
            try {
                server.take_prepared_batch();
            } catch (const std::exception&) {
                has_prepared_batch = false;
            }
            require(!has_prepared_batch,
                    "Rejected RUN_WAVE left prepared state behind");

            require(server.receive_file_command() ==
                        std::string("@0\t") +
                            WorkloadIpcServer::kPreparedBatchCommand,
                    "Valid RUN_BATCH did not execute after rejection");
            const auto prepared = server.take_prepared_batch();
            require(prepared.systems.size() == 1 &&
                        prepared.systems.front().system_id == 0,
                    "Valid RUN_BATCH prepared the wrong systems");
            require(server.receive_file_command() ==
                        std::string("@0\t") +
                            WorkloadIpcServer::kPreparedBatchCommand,
                    "Queued RUN_BATCH did not execute");
            const auto queued = server.take_prepared_batch();
            require(queued.batch_id == 13,
                    "Queued RUN_BATCH prepared the wrong batch");
            require(server.send_batch_done(0, 123, 7),
                    "First RUN_BATCH completion was not correlated");
            require(server.send_batch_done(0, 456, 9),
                    "Queued RUN_BATCH completion was not correlated");
        } catch (...) {
            server_error = std::current_exception();
        }
    });

    int client_fd = -1;
    try {
        client_fd = connect_client(socket_path);
        send_frame(client_fd, WorkloadMessageType::Hello, {});
        require(receive_frame(client_fd).type == WorkloadMessageType::Hello,
                "HELLO handshake failed");

        llmservingsim::ipc::RegisterTemplate registration;
        registration.set_request_id(1);
        registration.set_template_key("atomicity-test");
        auto* graph = registration.add_graphs();
        graph->set_system_id(0);
        graph->set_chakra_graph(make_graph());
        send_frame(
            client_fd, WorkloadMessageType::RegisterTemplate,
            registration.SerializeAsString());
        const auto ready_frame = receive_frame(client_fd);
        require(ready_frame.type == WorkloadMessageType::TemplateReady,
                "Template registration failed");
        llmservingsim::ipc::TemplateReady ready;
        require(ready.ParseFromArray(
                    ready_frame.payload.data(), ready_frame.payload.size()),
                "Invalid TEMPLATE_READY response");

        llmservingsim::ipc::RunWave wave;
        wave.set_request_id(2);
        wave.set_wave_id(41);
        wave.set_controller_system_id(0);
        auto* valid = wave.add_runs();
        valid->set_instance_id(0);
        valid->mutable_patch()->set_batch_id(10);
        valid->mutable_patch()->set_template_id(ready.template_id());
        valid->mutable_patch()->add_systems()->set_system_id(0);
        auto* invalid = wave.add_runs();
        invalid->set_instance_id(1);
        invalid->mutable_patch()->set_batch_id(11);
        invalid->mutable_patch()->set_template_id(ready.template_id() + 999);
        invalid->mutable_patch()->add_systems()->set_system_id(1);
        send_frame(
            client_fd, WorkloadMessageType::RunWave,
            wave.SerializeAsString());
        const auto error_frame = receive_frame(client_fd);
        require(error_frame.type == WorkloadMessageType::Error,
                "Malformed RUN_WAVE did not return ERROR");

        llmservingsim::ipc::RunBatch batch;
        batch.set_request_id(3);
        batch.set_instance_id(0);
        batch.set_execute(true);
        batch.set_controller_system_id(0);
        batch.mutable_patch()->set_batch_id(12);
        batch.mutable_patch()->set_template_id(ready.template_id());
        batch.mutable_patch()->add_systems()->set_system_id(0);
        send_frame(
            client_fd, WorkloadMessageType::RunBatch,
            batch.SerializeAsString());
        require(receive_frame(client_fd).type ==
                    WorkloadMessageType::BatchAccepted,
                "Valid RUN_BATCH was not accepted after rejection");

        batch.set_request_id(4);
        batch.mutable_patch()->set_batch_id(13);
        send_frame(
            client_fd, WorkloadMessageType::RunBatch,
            batch.SerializeAsString());
        require(receive_frame(client_fd).type ==
                    WorkloadMessageType::BatchAccepted,
                "Queued RUN_BATCH was not accepted");

        const auto first_done_frame = receive_frame(client_fd);
        require(first_done_frame.type == WorkloadMessageType::BatchDone,
                "First RUN_BATCH did not emit BATCH_DONE");
        llmservingsim::ipc::BatchDone first_done;
        require(first_done.ParseFromArray(
                    first_done_frame.payload.data(),
                    first_done_frame.payload.size()) &&
                    first_done.batch_id() == 12,
                "First RUN_BATCH completion lost FIFO identity");
        const auto queued_done_frame = receive_frame(client_fd);
        require(queued_done_frame.type == WorkloadMessageType::BatchDone,
                "Queued RUN_BATCH did not emit BATCH_DONE");
        llmservingsim::ipc::BatchDone queued_done;
        require(queued_done.ParseFromArray(
                    queued_done_frame.payload.data(),
                    queued_done_frame.payload.size()) &&
                    queued_done.batch_id() == 13,
                "Queued RUN_BATCH completion lost FIFO identity");
    } catch (...) {
        if (client_fd >= 0) {
            ::close(client_fd);
        }
        server_thread.join();
        throw;
    }
    ::close(client_fd);
    server_thread.join();
    if (server_error) {
        std::rethrow_exception(server_error);
    }
    std::cout << "RUN_WAVE atomic rejection passed" << std::endl;
    return 0;
}
