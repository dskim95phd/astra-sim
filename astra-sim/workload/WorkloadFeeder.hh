/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include <cstdint>
#include <memory>

#include "extern/graph_frontend/chakra/src/feeder/et_feeder_node.h"

namespace AstraSim {

class WorkloadFeeder {
  public:
    virtual ~WorkloadFeeder() = default;

    virtual void removeNode(std::uint64_t node_id) = 0;
    virtual bool hasNodesToIssue() = 0;
    virtual std::shared_ptr<Chakra::ETFeederNode> getNextIssuableNode() = 0;
    virtual void pushBackIssuableNode(std::uint64_t node_id) = 0;
    virtual std::shared_ptr<Chakra::ETFeederNode> lookupNode(
        std::uint64_t node_id) = 0;
    virtual void freeChildrenNodes(std::uint64_t node_id) = 0;
    virtual void printGraph() = 0;
};

}  // namespace AstraSim
