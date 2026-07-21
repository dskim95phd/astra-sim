/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/workload/StaticGraphTemplate.hh"

#include <deque>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace AstraSim {
namespace {

std::uint32_t read_message_size(
    std::string_view bytes, std::size_t& offset) {
    std::uint32_t value = 0;
    for (std::uint32_t byte_index = 0; byte_index < 5; ++byte_index) {
        if (offset >= bytes.size()) {
            throw std::invalid_argument(
                "Truncated Chakra message length prefix");
        }
        const auto byte = static_cast<std::uint8_t>(bytes[offset++]);
        if (byte_index == 4 && (byte & 0xf0U) != 0) {
            throw std::invalid_argument(
                "Chakra message length exceeds uint32 range");
        }
        value |= static_cast<std::uint32_t>(byte & 0x7fU)
                 << (byte_index * 7U);
        if ((byte & 0x80U) == 0) {
            return value;
        }
    }
    throw std::invalid_argument("Invalid Chakra message length prefix");
}

template <typename Message>
void read_message(
    std::string_view bytes, std::size_t& offset, Message& message) {
    const auto message_size = read_message_size(bytes, offset);
    if (message_size > bytes.size() - offset) {
        throw std::invalid_argument("Truncated Chakra protobuf message");
    }
    if (message_size >
        static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("Chakra protobuf message is too large");
    }
    if (!message.ParseFromArray(
            bytes.data() + offset, static_cast<int>(message_size))) {
        throw std::invalid_argument("Invalid Chakra protobuf message");
    }
    offset += message_size;
}

}  // namespace

StaticGraphTemplate StaticGraphTemplate::compile(
    std::string_view serialized_graph) {
    if (serialized_graph.empty()) {
        throw std::invalid_argument("Chakra graph is empty");
    }

    StaticGraphTemplate graph;
    std::size_t offset = 0;
    read_message(serialized_graph, offset, graph.metadata_);
    for (int index = graph.metadata_.attr_size() - 1; index >= 0; --index) {
        if (graph.metadata_.attr(index).name() == "input_file") {
            graph.metadata_.mutable_attr()->DeleteSubrange(index, 1);
        }
    }

    while (offset < serialized_graph.size()) {
        ChakraProtoMsg::Node chakra_node;
        read_message(serialized_graph, offset, chakra_node);
        if (chakra_node.type() == ChakraProtoMsg::INVALID_NODE) {
            throw std::invalid_argument(
                "Chakra graph contains an invalid node type");
        }
        const auto node_id = chakra_node.id();
        const auto node_index = graph.nodes_.size();
        if (!graph.indices_by_id_.emplace(node_id, node_index).second) {
            throw std::invalid_argument(
                "Chakra graph contains duplicate node id " +
                std::to_string(node_id));
        }

        StaticGraphNode node;
        node.initial_data_dependency_count =
            static_cast<std::uint32_t>(chakra_node.data_deps_size());
        node.chakra_node = std::move(chakra_node);
        graph.nodes_.emplace_back(std::move(node));
    }

    if (graph.nodes_.empty()) {
        throw std::invalid_argument("Chakra graph contains no nodes");
    }

    std::vector<std::uint32_t> dependency_counts;
    dependency_counts.reserve(graph.nodes_.size());
    for (std::size_t child_index = 0;
         child_index < graph.nodes_.size(); ++child_index) {
        const auto& node = graph.nodes_[child_index].chakra_node;
        std::unordered_set<std::uint64_t> unique_dependencies;
        for (const auto parent_id : node.data_deps()) {
            if (parent_id == node.id()) {
                throw std::invalid_argument(
                    "Chakra node depends on itself: " +
                    std::to_string(node.id()));
            }
            if (!unique_dependencies.emplace(parent_id).second) {
                throw std::invalid_argument(
                    "Chakra node contains a duplicate dependency: " +
                    std::to_string(node.id()));
            }
            const auto parent = graph.indices_by_id_.find(parent_id);
            if (parent == graph.indices_by_id_.end()) {
                throw std::invalid_argument(
                    "Chakra node refers to missing dependency " +
                    std::to_string(parent_id));
            }
            graph.nodes_[parent->second].children.emplace_back(child_index);
        }
        dependency_counts.emplace_back(
            graph.nodes_[child_index].initial_data_dependency_count);
        if (node.data_deps().empty()) {
            ++graph.root_count_;
        }
    }

    std::deque<std::size_t> ready;
    for (std::size_t index = 0; index < dependency_counts.size(); ++index) {
        if (dependency_counts[index] == 0) {
            ready.emplace_back(index);
        }
    }

    std::size_t visited = 0;
    while (!ready.empty()) {
        const auto parent_index = ready.front();
        ready.pop_front();
        ++visited;
        for (const auto child_index : graph.nodes_[parent_index].children) {
            auto& remaining = dependency_counts[child_index];
            if (--remaining == 0) {
                ready.emplace_back(child_index);
            }
        }
    }
    if (visited != graph.nodes_.size()) {
        throw std::invalid_argument("Chakra graph contains a dependency cycle");
    }

    return graph;
}

const ChakraProtoMsg::GlobalMetadata& StaticGraphTemplate::metadata()
    const noexcept {
    return metadata_;
}

const std::vector<StaticGraphNode>& StaticGraphTemplate::nodes()
    const noexcept {
    return nodes_;
}

const StaticGraphNode& StaticGraphTemplate::node(
    std::uint64_t node_id) const {
    return nodes_.at(node_index(node_id));
}

std::size_t StaticGraphTemplate::node_index(std::uint64_t node_id) const {
    const auto found = indices_by_id_.find(node_id);
    if (found == indices_by_id_.end()) {
        throw std::out_of_range("Unknown static Chakra node id");
    }
    return found->second;
}

bool StaticGraphTemplate::contains_node(std::uint64_t node_id) const noexcept {
    return indices_by_id_.count(node_id) != 0;
}

std::size_t StaticGraphTemplate::root_count() const noexcept {
    return root_count_;
}

}  // namespace AstraSim
