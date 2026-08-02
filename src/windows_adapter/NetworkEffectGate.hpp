#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace afsim_ns3
{

enum class EffectState
{
    Available,
    Degraded,
    Blocked
};

struct EffectDecision
{
    std::string entityId;
    std::string subsystemId;
    std::string subsystemType;
    std::string peerEntityId;
    EffectState state{EffectState::Blocked};
    std::uint64_t revision{};
};

class NetworkEffectGate
{
  public:
    // Returns only decisions from a newer ns-3 metrics revision.
    std::vector<EffectDecision> UpdateFromMetricsJson(
        const std::string& jsonLine);

    EffectState GetState(
        const std::string& entityId,
        const std::string& subsystemId) const;

    bool CanOperate(
        const std::string& entityId,
        const std::string& subsystemId) const;

    std::uint64_t LatestRevision() const;

  private:
    mutable std::mutex mMutex;
    std::map<std::pair<std::string, std::string>, EffectState> mStates;
    std::uint64_t mLatestRevision{};
};

const char* ToString(EffectState state);

} // namespace afsim_ns3
