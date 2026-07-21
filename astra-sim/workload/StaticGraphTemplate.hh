/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "et_def.pb.h"

namespace AstraSim {

struct StaticGraphNode {
    ChakraProtoMsg::Node chakra_node;
    std::vector<std::size_t> children;
    std::uint32_t initial_data_dependency_count = 0;
};

class StaticGraphTemplate {
  public:
    static StaticGraphTemplate compile(std::string_view serialized_graph);

    const ChakraProtoMsg::GlobalMetadata& metadata() const noexcept;
    const std::vector<StaticGraphNode>& nodes() const noexcept;
    const StaticGraphNode& node(std::uint64_t node_id) const;
    std::size_t node_index(std::uint64_t node_id) const;
    bool contains_node(std::uint64_t node_id) const noexcept;
    std::size_t root_count() const noexcept;

  private:
    ChakraProtoMsg::GlobalMetadata metadata_;
    std::vector<StaticGraphNode> nodes_;
    std::unordered_map<std::uint64_t, std::size_t> indices_by_id_;
    std::size_t root_count_ = 0;
};

}  // namespace AstraSim
