/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/workload/PreparedFeeder.hh"

#include <iostream>
#include <stdexcept>
#include <utility>

namespace AstraSim {
namespace {

void set_runtime_attribute(
    ChakraProtoMsg::Node& node,
    const std::string& name,
    std::uint64_t value) {
    for (auto& attribute : *node.mutable_attr()) {
        if (attribute.name() != name) {
            continue;
        }
        if (name == "tensor_size") {
            attribute.set_uint64_val(value);
        } else {
            attribute.set_int64_val(static_cast<std::int64_t>(value));
        }
        return;
    }
}

std::shared_ptr<Chakra::ETFeederNode> make_runtime_node(
    const StaticGraphNode& static_node,
    const RuntimeNodeValues& runtime) {
    auto node = std::make_shared<ChakraProtoMsg::Node>(
        static_node.chakra_node);
    node->set_duration_micros(runtime.enabled ? runtime.duration : 0);
    set_runtime_attribute(
        *node, "tensor_size", runtime.enabled ? runtime.tensor_size : 0);
    set_runtime_attribute(
        *node, "comm_size", runtime.enabled ? runtime.comm_size : 0);
    return std::make_shared<Chakra::ETFeederNode>(std::move(node));
}

}  // namespace

PreparedFeeder::PreparedFeeder(IterationState iteration)
    : iteration_(std::move(iteration)) {
    const auto& graph_nodes = iteration_.graph().nodes();
    nodes_.reserve(graph_nodes.size());
    for (std::size_t index = 0; index < graph_nodes.size(); ++index) {
        const auto node_id = graph_nodes[index].chakra_node.id();
        indices_by_id_.emplace(node_id, index);
        nodes_.emplace_back(make_runtime_node(
            graph_nodes[index], iteration_.runtime_values(index)));
    }
}

void PreparedFeeder::removeNode(std::uint64_t) {}

bool PreparedFeeder::hasNodesToIssue() {
    return !iteration_.is_complete();
}

std::shared_ptr<Chakra::ETFeederNode>
PreparedFeeder::getNextIssuableNode() {
    if (!iteration_.has_ready_nodes()) {
        return nullptr;
    }
    return nodes_.at(iteration_.pop_ready_node());
}

void PreparedFeeder::pushBackIssuableNode(std::uint64_t node_id) {
    iteration_.push_back_node(iteration_.graph().node_index(node_id));
}

std::shared_ptr<Chakra::ETFeederNode> PreparedFeeder::lookupNode(
    std::uint64_t node_id) {
    const auto found = indices_by_id_.find(node_id);
    if (found == indices_by_id_.end()) {
        throw std::out_of_range("Prepared workload node id was not found");
    }
    return nodes_.at(found->second);
}

void PreparedFeeder::freeChildrenNodes(std::uint64_t node_id) {
    iteration_.complete_node(iteration_.graph().node_index(node_id));
}

void PreparedFeeder::printGraph() {
    for (std::size_t index = 0; index < nodes_.size(); ++index) {
        if (iteration_.status(index) != IterationNodeStatus::Completed) {
            nodes_[index]->printNode();
        }
    }
}

}  // namespace AstraSim
