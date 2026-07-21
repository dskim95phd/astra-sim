/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "common/TemplateRegistry.hh"

#include "astra-sim/workload/IterationState.hh"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace AstraSimAnalytical {
namespace {

bool is_patchable_attribute(const std::string& attribute) {
    static const std::unordered_set<std::string> patchable_attributes = {
        "duration",
        "tensor_size",
        "comm_size",
        "enabled",
    };
    return patchable_attributes.count(attribute) != 0;
}

std::uint64_t slot_key(std::uint32_t system_id, std::uint32_t slot_id) {
    return (static_cast<std::uint64_t>(system_id) << 32U) |
           static_cast<std::uint64_t>(slot_id);
}

void apply_slot_value(
    AstraSim::RuntimeNodeValues& runtime,
    const SlotTarget& target,
    std::uint64_t value) {
    if (target.attribute == "duration") {
        runtime.duration = value;
    } else if (target.attribute == "tensor_size") {
        runtime.tensor_size = value;
    } else if (target.attribute == "comm_size") {
        runtime.comm_size = value;
    } else if (target.attribute == "enabled") {
        runtime.enabled = value != 0;
    } else {
        throw std::invalid_argument("Unsupported runtime slot attribute");
    }
}

std::vector<std::uint64_t> drain_iteration(
    const AstraSim::StaticGraphTemplate& graph, bool exercise_pushback) {
    AstraSim::IterationState iteration(graph);
    std::vector<std::uint64_t> execution_order;
    execution_order.reserve(graph.nodes().size());
    bool pushed_back = false;
    while (iteration.has_ready_nodes()) {
        auto node_index = iteration.pop_ready_node();
        if (exercise_pushback && !pushed_back) {
            iteration.push_back_node(node_index);
            const auto retried_node_index = iteration.pop_ready_node();
            if (retried_node_index != node_index) {
                throw std::invalid_argument(
                    "Iteration pushback changed ready-node priority");
            }
            node_index = retried_node_index;
            pushed_back = true;
        }
        execution_order.emplace_back(
            graph.nodes().at(node_index).chakra_node.id());
        iteration.complete_node(node_index);
    }
    if (!iteration.is_complete() ||
        iteration.completed_count() != graph.nodes().size()) {
        throw std::invalid_argument(
            "Compiled Chakra graph cannot reach a complete iteration state");
    }
    return execution_order;
}

void validate_repeatable_iteration_state(
    const AstraSim::StaticGraphTemplate& graph) {
    const auto first_order = drain_iteration(graph, false);
    const auto second_order = drain_iteration(graph, true);
    if (first_order != second_order) {
        throw std::invalid_argument(
            "Compiled Chakra graph produced unstable execution state");
    }
}

}  // namespace

PreparedSystem::PreparedSystem(
    std::uint32_t system_id,
    const AstraSim::StaticGraphTemplate& graph)
    : system_id(system_id), iteration(graph) {}

void TemplateRegistry::validate(
    const llmservingsim::ipc::RegisterTemplate& request) const {
    if (request.template_key().empty()) {
        throw std::invalid_argument("Template key is empty");
    }
    if (request.graphs().empty()) {
        throw std::invalid_argument("Template contains no Chakra graphs");
    }

    std::unordered_set<std::uint32_t> system_ids;
    for (const auto& graph : request.graphs()) {
        if (graph.chakra_graph().empty()) {
            throw std::invalid_argument(
                "Template contains an empty Chakra graph");
        }
        if (!system_ids.emplace(graph.system_id()).second) {
            throw std::invalid_argument(
                "Template contains a duplicate system id");
        }
    }

    for (const auto& binding : request.bindings()) {
        if (system_ids.count(binding.system_id()) == 0) {
            throw std::invalid_argument(
                "Slot binding refers to a system outside the template");
        }
        if (binding.attribute().empty()) {
            throw std::invalid_argument("Slot binding attribute is empty");
        }
        if (!is_patchable_attribute(binding.attribute())) {
            throw std::invalid_argument(
                "Slot binding uses an unsupported attribute");
        }
    }
}

llmservingsim::ipc::TemplateReady TemplateRegistry::register_template(
    const llmservingsim::ipc::RegisterTemplate& request) {
    validate(request);
    auto stored_request = request;
    stored_request.clear_request_id();

    std::uint64_t template_id;
    const auto existing = ids_by_key_.find(request.template_key());
    if (existing != ids_by_key_.end()) {
        template_id = existing->second;
        if (templates_.at(template_id).descriptor.SerializeAsString() !=
            stored_request.SerializeAsString()) {
            throw std::invalid_argument(
                "Template key is already registered with different data");
        }
    } else {
        RegisteredTemplate registered_template;
        registered_template.descriptor = stored_request;
        for (const auto& graph : stored_request.graphs()) {
            auto compiled_graph = AstraSim::StaticGraphTemplate::compile(
                graph.chakra_graph());
            validate_repeatable_iteration_state(compiled_graph);
            registered_template.graphs.emplace(
                graph.system_id(), std::move(compiled_graph));
        }
        for (const auto& binding : stored_request.bindings()) {
            const auto graph =
                registered_template.graphs.find(binding.system_id());
            if (graph == registered_template.graphs.end() ||
                !graph->second.contains_node(binding.node_id())) {
                throw std::invalid_argument(
                    "Slot binding refers to a node outside the template");
            }
            const auto key = slot_key(
                binding.system_id(), binding.slot_id());
            auto& targets = registered_template.slot_targets[key];
            const auto node_index =
                graph->second.node_index(binding.node_id());
            const auto duplicate = std::find_if(
                targets.begin(), targets.end(),
                [&binding, node_index](const SlotTarget& target) {
                    return target.node_index == node_index &&
                           target.attribute == binding.attribute();
                });
            if (duplicate != targets.end()) {
                throw std::invalid_argument(
                    "Template contains a duplicate slot binding");
            }
            targets.push_back(SlotTarget{
                node_index,
                binding.attribute(),
            });
            if (binding.required()) {
                registered_template.required_slots.emplace(key);
            }
        }

        template_id = next_template_id_++;
        templates_.emplace(
            template_id, std::move(registered_template));
        ids_by_key_.emplace(request.template_key(), template_id);
    }

    llmservingsim::ipc::TemplateReady response;
    response.set_request_id(request.request_id());
    response.set_template_id(template_id);
    response.set_template_key(request.template_key());
    const auto& registered_template = templates_.at(template_id);
    std::vector<std::uint32_t> system_ids;
    system_ids.reserve(registered_template.graphs.size());
    for (const auto& system_graph : registered_template.graphs) {
        system_ids.emplace_back(system_graph.first);
    }
    std::sort(system_ids.begin(), system_ids.end());
    for (const auto system_id : system_ids) {
        const auto& graph = registered_template.graphs.at(system_id);
        auto* summary = response.add_graphs();
        summary->set_system_id(system_id);
        summary->set_node_count(graph.nodes().size());
        summary->set_root_count(graph.root_count());
        summary->set_iteration_state_validated(true);
    }
    return response;
}

PreparedBatch TemplateRegistry::prepare_batch(
    const llmservingsim::ipc::RunBatch& request) const {
    const auto& patch = request.patch();
    if (patch.template_id() == 0) {
        throw std::invalid_argument("Batch patch has no template id");
    }
    const auto& registered_template = lookup(patch.template_id());

    PreparedBatch prepared{
        request.request_id(),
        patch.batch_id(),
        patch.template_id(),
    };
    std::vector<std::uint32_t> system_ids;
    system_ids.reserve(registered_template.graphs.size());
    for (const auto& system_graph : registered_template.graphs) {
        system_ids.emplace_back(system_graph.first);
    }
    std::sort(system_ids.begin(), system_ids.end());

    std::unordered_map<std::uint32_t, std::size_t> system_indices;
    for (const auto system_id : system_ids) {
        system_indices.emplace(system_id, prepared.systems.size());
        prepared.systems.emplace_back(
            system_id, registered_template.graphs.at(system_id));
    }

    std::unordered_set<std::uint32_t> patched_systems;
    std::unordered_set<std::uint64_t> supplied_slots;
    for (const auto& system_patch : patch.systems()) {
        const auto system = system_indices.find(system_patch.system_id());
        if (system == system_indices.end()) {
            throw std::invalid_argument(
                "Batch patch refers to a system outside the template");
        }
        if (!patched_systems.emplace(system_patch.system_id()).second) {
            throw std::invalid_argument(
                "Batch patch contains a duplicate system");
        }
        auto& iteration = prepared.systems.at(system->second).iteration;
        const auto apply_value = [&](std::uint32_t slot_id,
                                     std::uint64_t value) {
            const auto key = slot_key(system_patch.system_id(), slot_id);
            if (!supplied_slots.emplace(key).second) {
                throw std::invalid_argument(
                    "Batch patch contains a duplicate slot value");
            }
            const auto targets = registered_template.slot_targets.find(key);
            if (targets == registered_template.slot_targets.end()) {
                throw std::invalid_argument(
                    "Batch patch refers to an unknown slot");
            }
            for (const auto& target : targets->second) {
                apply_slot_value(
                    iteration.mutable_runtime_values(target.node_index),
                    target, value);
            }
            ++prepared.patched_value_count;
        };
        for (const auto& slot_value : system_patch.values()) {
            apply_value(slot_value.slot_id(), slot_value.value());
        }
        if (system_patch.packed_values_size() % 2 != 0) {
            throw std::invalid_argument(
                "Batch patch packed values do not contain slot/value pairs");
        }
        for (int index = 0; index < system_patch.packed_values_size();
             index += 2) {
            const auto slot_id = system_patch.packed_values(index);
            if (slot_id > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument(
                    "Batch patch packed slot id exceeds uint32 range");
            }
            apply_value(
                static_cast<std::uint32_t>(slot_id),
                system_patch.packed_values(index + 1));
        }
    }

    if (patched_systems.size() != registered_template.graphs.size()) {
        throw std::invalid_argument(
            "Batch patch does not contain every template system");
    }
    for (const auto required_slot : registered_template.required_slots) {
        if (supplied_slots.count(required_slot) == 0) {
            throw std::invalid_argument(
                "Batch patch is missing a required slot value");
        }
    }
    return prepared;
}

const RegisteredTemplate& TemplateRegistry::lookup(
    std::uint64_t template_id) const {
    const auto found = templates_.find(template_id);
    if (found == templates_.end()) {
        throw std::out_of_range("Unknown template id");
    }
    return found->second;
}

std::size_t TemplateRegistry::size() const noexcept {
    return templates_.size();
}

}  // namespace AstraSimAnalytical
