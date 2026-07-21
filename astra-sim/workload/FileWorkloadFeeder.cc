/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/workload/FileWorkloadFeeder.hh"

namespace AstraSim {

FileWorkloadFeeder::FileWorkloadFeeder(const std::string& filename)
    : feeder_(filename) {}

void FileWorkloadFeeder::removeNode(std::uint64_t node_id) {
    feeder_.removeNode(node_id);
}

bool FileWorkloadFeeder::hasNodesToIssue() {
    return feeder_.hasNodesToIssue();
}

std::shared_ptr<Chakra::ETFeederNode>
FileWorkloadFeeder::getNextIssuableNode() {
    return feeder_.getNextIssuableNode();
}

void FileWorkloadFeeder::pushBackIssuableNode(std::uint64_t node_id) {
    feeder_.pushBackIssuableNode(node_id);
}

std::shared_ptr<Chakra::ETFeederNode> FileWorkloadFeeder::lookupNode(
    std::uint64_t node_id) {
    return feeder_.lookupNode(node_id);
}

void FileWorkloadFeeder::freeChildrenNodes(std::uint64_t node_id) {
    feeder_.freeChildrenNodes(node_id);
}

void FileWorkloadFeeder::printGraph() {
    feeder_.printGraph();
}

}  // namespace AstraSim
