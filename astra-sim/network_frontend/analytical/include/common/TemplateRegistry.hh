/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "astra-sim/workload/IterationState.hh"
#include "astra-sim/workload/StaticGraphTemplate.hh"
#include "llmservingsim_workload.pb.h"

namespace AstraSimAnalytical {

struct SlotTarget {
    std::size_t node_index;
    std::string attribute;
};

struct RegisteredTemplate {
    llmservingsim::ipc::RegisterTemplate descriptor;
    std::unordered_map<std::uint32_t, AstraSim::StaticGraphTemplate> graphs;
    std::unordered_map<std::uint64_t, std::vector<SlotTarget>> slot_targets;
    std::unordered_set<std::uint64_t> required_slots;
};

struct PreparedSystem {
    PreparedSystem(
        std::uint32_t system_id,
        const AstraSim::StaticGraphTemplate& graph);

    std::uint32_t system_id;
    AstraSim::IterationState iteration;
};

struct PreparedBatch {
    std::uint64_t request_id;
    std::uint64_t batch_id;
    std::uint64_t template_id;
    std::vector<PreparedSystem> systems;
    std::size_t patched_value_count = 0;
};

class TemplateRegistry {
  public:
    llmservingsim::ipc::TemplateReady register_template(
        const llmservingsim::ipc::RegisterTemplate& request);
    PreparedBatch prepare_batch(
        const llmservingsim::ipc::RunBatch& request) const;

    const RegisteredTemplate& lookup(
        std::uint64_t template_id) const;
    std::size_t size() const noexcept;

  private:
    void validate(const llmservingsim::ipc::RegisterTemplate& request) const;

    std::uint64_t next_template_id_ = 1;
    std::unordered_map<std::uint64_t, RegisteredTemplate> templates_;
    std::unordered_map<std::string, std::uint64_t> ids_by_key_;
};

}  // namespace AstraSimAnalytical
