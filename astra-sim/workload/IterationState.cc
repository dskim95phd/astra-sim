/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/workload/IterationState.hh"

#include <stdexcept>
#include <string>

namespace AstraSim {
namespace {

std::uint64_t unsigned_attribute(
    const ChakraProtoMsg::Node& node, const std::string& name) {
    for (const auto& attribute : node.attr()) {
        if (attribute.name() != name) {
            continue;
        }
        if (attribute.has_uint64_val()) {
            return attribute.uint64_val();
        }
        if (attribute.has_int64_val()) {
            if (attribute.int64_val() < 0) {
                throw std::invalid_argument(
                    "Chakra node contains a negative " + name);
            }
            return static_cast<std::uint64_t>(attribute.int64_val());
        }
        throw std::invalid_argument(
            "Chakra node attribute has an invalid type: " + name);
    }
    return 0;
}

}  // namespace

IterationState::IterationState(const StaticGraphTemplate& graph)
    : graph_(&graph), ready_nodes_(ReadyCompare{&graph}) {
    const auto& nodes = graph.nodes();
    remaining_dependencies_.reserve(nodes.size());
    statuses_.reserve(nodes.size());
    runtime_values_.reserve(nodes.size());

    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto& static_node = nodes[index];
        const auto dependency_count =
            static_node.initial_data_dependency_count;
        remaining_dependencies_.emplace_back(dependency_count);
        statuses_.emplace_back(
            dependency_count == 0 ? IterationNodeStatus::Ready
                                  : IterationNodeStatus::Pending);

        RuntimeNodeValues values;
        values.duration = static_node.chakra_node.duration_micros();
        values.tensor_size = unsigned_attribute(
            static_node.chakra_node, "tensor_size");
        values.comm_size = unsigned_attribute(
            static_node.chakra_node, "comm_size");
        runtime_values_.emplace_back(values);

        if (dependency_count == 0) {
            ready_nodes_.emplace(index);
        }
    }
}

const StaticGraphTemplate& IterationState::graph() const noexcept {
    return *graph_;
}

bool IterationState::has_ready_nodes() const noexcept {
    return !ready_nodes_.empty();
}

std::size_t IterationState::pop_ready_node() {
    if (ready_nodes_.empty()) {
        throw std::logic_error("Iteration has no ready Chakra node");
    }
    const auto node_index = ready_nodes_.top();
    ready_nodes_.pop();
    if (statuses_.at(node_index) != IterationNodeStatus::Ready) {
        throw std::logic_error("Ready queue contains a non-ready Chakra node");
    }
    statuses_[node_index] = IterationNodeStatus::Issued;
    return node_index;
}

void IterationState::push_back_node(std::size_t node_index) {
    if (statuses_.at(node_index) != IterationNodeStatus::Issued) {
        throw std::logic_error("Only an issued Chakra node can be pushed back");
    }
    statuses_[node_index] = IterationNodeStatus::Ready;
    ready_nodes_.emplace(node_index);
}

void IterationState::complete_node(std::size_t node_index) {
    if (statuses_.at(node_index) != IterationNodeStatus::Issued) {
        throw std::logic_error("Only an issued Chakra node can complete");
    }
    statuses_[node_index] = IterationNodeStatus::Completed;
    ++completed_count_;

    for (const auto child_index : graph_->nodes().at(node_index).children) {
        if (statuses_.at(child_index) != IterationNodeStatus::Pending) {
            throw std::logic_error(
                "Chakra child became ready before all parents completed");
        }
        auto& remaining = remaining_dependencies_.at(child_index);
        if (remaining == 0) {
            throw std::logic_error(
                "Chakra child dependency count underflow");
        }
        --remaining;
        if (remaining == 0) {
            statuses_[child_index] = IterationNodeStatus::Ready;
            ready_nodes_.emplace(child_index);
        }
    }
}

std::uint32_t IterationState::remaining_dependencies(
    std::size_t node_index) const {
    return remaining_dependencies_.at(node_index);
}

IterationNodeStatus IterationState::status(std::size_t node_index) const {
    return statuses_.at(node_index);
}

const RuntimeNodeValues& IterationState::runtime_values(
    std::size_t node_index) const {
    return runtime_values_.at(node_index);
}

RuntimeNodeValues& IterationState::mutable_runtime_values(
    std::size_t node_index) {
    if (statuses_.at(node_index) != IterationNodeStatus::Pending &&
        statuses_.at(node_index) != IterationNodeStatus::Ready) {
        throw std::logic_error(
            "Cannot patch a Chakra node after it has been issued");
    }
    return runtime_values_.at(node_index);
}

std::size_t IterationState::completed_count() const noexcept {
    return completed_count_;
}

bool IterationState::is_complete() const noexcept {
    return completed_count_ == graph_->nodes().size();
}

bool IterationState::ReadyCompare::operator()(
    std::size_t lhs, std::size_t rhs) const {
    return graph->nodes().at(lhs).chakra_node.id() >
           graph->nodes().at(rhs).chakra_node.id();
}

}  // namespace AstraSim
