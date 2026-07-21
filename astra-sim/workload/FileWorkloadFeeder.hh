/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#pragma once

#include <memory>
#include <string>

#include "astra-sim/workload/WorkloadFeeder.hh"
#include "extern/graph_frontend/chakra/src/feeder/et_feeder.h"

namespace AstraSim {

class FileWorkloadFeeder : public WorkloadFeeder {
  public:
    explicit FileWorkloadFeeder(const std::string& filename);

    void removeNode(std::uint64_t node_id) override;
    bool hasNodesToIssue() override;
    std::shared_ptr<Chakra::ETFeederNode> getNextIssuableNode() override;
    void pushBackIssuableNode(std::uint64_t node_id) override;
    std::shared_ptr<Chakra::ETFeederNode> lookupNode(
        std::uint64_t node_id) override;
    void freeChildrenNodes(std::uint64_t node_id) override;
    void printGraph() override;

  private:
    Chakra::ETFeeder feeder_;
};

}  // namespace AstraSim
