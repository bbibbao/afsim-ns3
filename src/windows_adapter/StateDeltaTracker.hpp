#pragma once

#include "AfsimNs3Protocol.hpp"

#include <map>
#include <string>
#include <vector>

namespace afsim_ns3
{

class StateDeltaTracker
{
  public:
    bool Initialized() const;

    void Reset(
        const std::vector<EntityState>& entities,
        const std::vector<FlowConfig>& flows);

    // Updates the stored baseline and returns only changes since the last call.
    DeltaState Update(
        const std::vector<EntityState>& entities,
        const std::vector<FlowConfig>& flows);

    static bool Empty(const DeltaState& delta);

  private:
    std::map<std::string, EntityState> mEntities;
    std::map<std::string, FlowConfig> mFlows;
    bool mInitialized{};
};

} // namespace afsim_ns3
