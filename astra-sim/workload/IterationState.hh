/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>
#include <queue>
#include <vector>

#include "astra-sim/workload/StaticGraphTemplate.hh"

namespace AstraSim {

enum class IterationNodeStatus : std::uint8_t {
    Pending,
    Ready,
    Issued,
    Completed,
};

struct RuntimeNodeValues {
    std::uint64_t duration = 0;
    std::uint64_t tensor_size = 0;
    std::uint64_t comm_size = 0;
    bool enabled = true;
};

class IterationState {
  public:
    explicit IterationState(const StaticGraphTemplate& graph);

    const StaticGraphTemplate& graph() const noexcept;
    bool has_ready_nodes() const noexcept;
    std::size_t pop_ready_node();
    void push_back_node(std::size_t node_index);
    void complete_node(std::size_t node_index);

    std::uint32_t remaining_dependencies(
        std::size_t node_index) const;
    IterationNodeStatus status(std::size_t node_index) const;
    const RuntimeNodeValues& runtime_values(
        std::size_t node_index) const;
    RuntimeNodeValues& mutable_runtime_values(std::size_t node_index);

    std::size_t completed_count() const noexcept;
    bool is_complete() const noexcept;

  private:
    struct ReadyCompare {
        const StaticGraphTemplate* graph = nullptr;
        bool operator()(std::size_t lhs, std::size_t rhs) const;
    };

    const StaticGraphTemplate* graph_;
    std::vector<std::uint32_t> remaining_dependencies_;
    std::vector<IterationNodeStatus> statuses_;
    std::vector<RuntimeNodeValues> runtime_values_;
    std::priority_queue<
        std::size_t,
        std::vector<std::size_t>,
        ReadyCompare>
        ready_nodes_;
    std::size_t completed_count_ = 0;
};

}  // namespace AstraSim
