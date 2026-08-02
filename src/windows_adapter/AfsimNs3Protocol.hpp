#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace afsim_ns3
{

struct Position
{
    double xM{};
    double yM{};
    double zM{};
};

struct Velocity
{
    double vxMps{};
    double vyMps{};
    double vzMps{};
};

struct DeviceConfig
{
    std::string deviceId;
    std::string peerEntityId;
    std::string kind;
    std::uint64_t dataRateBps{};
    double delayMs{};
    double lossRate{};
    std::string linkState;
};

struct EffectPolicy
{
    std::string subsystemId;
    std::string subsystemType;
    std::string peerEntityId;
    double maxDelayMs{};
    double maxLossRate{};
    double minThroughputBps{};
    std::string violationState;
};

struct EntityState
{
    std::string entityId;
    std::string businessNodeId;
    std::string parentEntityId;
    bool hasParent{};
    Position position;
    Velocity velocity;
    double headingDeg{};
    bool alive{};
    std::vector<DeviceConfig> devices;
    std::vector<EffectPolicy> effectPolicies;
};

struct FlowConfig
{
    std::string flowId;
    std::string sourceEntityId;
    std::string targetEntityId;
    std::uint32_t packetSizeBytes{};
    std::uint32_t intervalMs{};
    std::uint32_t durationMs{};
    bool active{};
};

// A delta contains only changed/created entities and flows plus removed IDs.
struct DeltaState
{
    std::vector<EntityState> entityUpserts;
    std::vector<std::string> entityRemovals;
    std::vector<FlowConfig> flowUpserts;
    std::vector<std::string> flowRemovals;
};

std::string BuildInitMessage(
    const std::string& requestId,
    std::uint64_t timestampMs,
    const std::vector<EntityState>& entities,
    const std::vector<FlowConfig>& flows);

std::string BuildDeltaMessage(
    const std::string& requestId,
    std::uint64_t timestampMs,
    const DeltaState& delta);

} // namespace afsim_ns3
