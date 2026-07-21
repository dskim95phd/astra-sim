/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "astra-sim/workload/IterationState.hh"
#include "astra-sim/workload/WorkloadFeeder.hh"

namespace AstraSim {

class PreparedFeeder : public WorkloadFeeder {
  public:
    explicit PreparedFeeder(IterationState iteration);

    void removeNode(std::uint64_t node_id) override;
    bool hasNodesToIssue() override;
    std::shared_ptr<Chakra::ETFeederNode> getNextIssuableNode() override;
    void pushBackIssuableNode(std::uint64_t node_id) override;
    std::shared_ptr<Chakra::ETFeederNode> lookupNode(
        std::uint64_t node_id) override;
    void freeChildrenNodes(std::uint64_t node_id) override;
    void printGraph() override;

  private:
    IterationState iteration_;
    std::vector<std::shared_ptr<Chakra::ETFeederNode>> nodes_;
    std::unordered_map<std::uint64_t, std::size_t> indices_by_id_;
};

}  // namespace AstraSim
