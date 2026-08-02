#include "StateDeltaTracker.hpp"

#include <stdexcept>

namespace afsim_ns3
{
namespace
{

bool
Same(const Position& left, const Position& right)
{
    return left.xM == right.xM && left.yM == right.yM &&
           left.zM == right.zM;
}

bool
Same(const Velocity& left, const Velocity& right)
{
    return left.vxMps == right.vxMps && left.vyMps == right.vyMps &&
           left.vzMps == right.vzMps;
}

bool
Same(const DeviceConfig& left, const DeviceConfig& right)
{
    return left.deviceId == right.deviceId &&
           left.peerEntityId == right.peerEntityId &&
           left.kind == right.kind &&
           left.dataRateBps == right.dataRateBps &&
           left.delayMs == right.delayMs &&
           left.lossRate == right.lossRate &&
           left.linkState == right.linkState;
}

bool
Same(const EffectPolicy& left, const EffectPolicy& right)
{
    return left.subsystemId == right.subsystemId &&
           left.subsystemType == right.subsystemType &&
           left.peerEntityId == right.peerEntityId &&
           left.maxDelayMs == right.maxDelayMs &&
           left.maxLossRate == right.maxLossRate &&
           left.minThroughputBps == right.minThroughputBps &&
           left.violationState == right.violationState;
}

template <typename Value>
bool
SameVector(const std::vector<Value>& left, const std::vector<Value>& right)
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        if (!Same(left[index], right[index]))
        {
            return false;
        }
    }
    return true;
}

bool
Same(const EntityState& left, const EntityState& right)
{
    return left.entityId == right.entityId &&
           left.businessNodeId == right.businessNodeId &&
           left.parentEntityId == right.parentEntityId &&
           left.hasParent == right.hasParent &&
           Same(left.position, right.position) &&
           Same(left.velocity, right.velocity) &&
           left.headingDeg == right.headingDeg &&
           left.alive == right.alive &&
           SameVector(left.devices, right.devices) &&
           SameVector(left.effectPolicies, right.effectPolicies);
}

bool
Same(const FlowConfig& left, const FlowConfig& right)
{
    return left.flowId == right.flowId &&
           left.sourceEntityId == right.sourceEntityId &&
           left.targetEntityId == right.targetEntityId &&
           left.packetSizeBytes == right.packetSizeBytes &&
           left.intervalMs == right.intervalMs &&
           left.durationMs == right.durationMs &&
           left.active == right.active;
}

template <typename Value, typename IdGetter>
std::map<std::string, Value>
Index(const std::vector<Value>& values, IdGetter idGetter)
{
    std::map<std::string, Value> indexed;
    for (const auto& value : values)
    {
        const auto& id = idGetter(value);
        if (id.empty() || !indexed.emplace(id, value).second)
        {
            throw std::invalid_argument("state contains an empty or duplicate ID");
        }
    }
    return indexed;
}

} // namespace

bool
StateDeltaTracker::Initialized() const
{
    return mInitialized;
}

void
StateDeltaTracker::Reset(
    const std::vector<EntityState>& entities,
    const std::vector<FlowConfig>& flows)
{
    mEntities = Index(
        entities,
        [](const EntityState& entity) -> const std::string&
        { return entity.entityId; });
    mFlows = Index(
        flows,
        [](const FlowConfig& flow) -> const std::string&
        { return flow.flowId; });
    mInitialized = true;
}

DeltaState
StateDeltaTracker::Update(
    const std::vector<EntityState>& entities,
    const std::vector<FlowConfig>& flows)
{
    const auto currentEntities = Index(
        entities,
        [](const EntityState& entity) -> const std::string&
        { return entity.entityId; });
    const auto currentFlows = Index(
        flows,
        [](const FlowConfig& flow) -> const std::string&
        { return flow.flowId; });

    DeltaState delta;
    for (const auto& entry : currentEntities)
    {
        const auto old = mEntities.find(entry.first);
        if (old == mEntities.end() || !Same(old->second, entry.second))
        {
            delta.entityUpserts.push_back(entry.second);
        }
    }
    for (const auto& entry : mEntities)
    {
        if (currentEntities.count(entry.first) == 0)
        {
            delta.entityRemovals.push_back(entry.first);
        }
    }
    for (const auto& entry : currentFlows)
    {
        const auto old = mFlows.find(entry.first);
        if (old == mFlows.end() || !Same(old->second, entry.second))
        {
            delta.flowUpserts.push_back(entry.second);
        }
    }
    for (const auto& entry : mFlows)
    {
        if (currentFlows.count(entry.first) == 0)
        {
            delta.flowRemovals.push_back(entry.first);
        }
    }

    mEntities = currentEntities;
    mFlows = currentFlows;
    mInitialized = true;
    return delta;
}

bool
StateDeltaTracker::Empty(const DeltaState& delta)
{
    return delta.entityUpserts.empty() && delta.entityRemovals.empty() &&
           delta.flowUpserts.empty() && delta.flowRemovals.empty();
}

} // namespace afsim_ns3
