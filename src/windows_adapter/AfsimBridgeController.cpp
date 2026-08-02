#include "AfsimBridgeController.hpp"

#include <utility>

namespace afsim_ns3
{

AfsimBridgeController::AfsimBridgeController(TcpClientConfig config)
    : mClient(std::move(config))
{
    mClient.SetLineHandler(
        [this](const std::string& line) { OnJsonLine(line); });
}

AfsimBridgeController::~AfsimBridgeController()
{
    Stop();
}

void
AfsimBridgeController::Start()
{
    mClient.Start();
}

void
AfsimBridgeController::Stop()
{
    mClient.Stop();
}

void
AfsimBridgeController::SubmitInitial(
    const std::string& requestId,
    std::uint64_t timestampMs,
    const std::vector<EntityState>& entities,
    const std::vector<FlowConfig>& flows)
{
    mDeltaTracker.Reset(entities, flows);
    mClient.QueueInitial(
        BuildInitMessage(requestId, timestampMs, entities, flows));
}

StateSubmitResult
AfsimBridgeController::SubmitCurrentState(
    const std::string& requestId,
    std::uint64_t timestampMs,
    const std::vector<EntityState>& entities,
    const std::vector<FlowConfig>& flows)
{
    if (!mDeltaTracker.Initialized())
    {
        SubmitInitial(requestId, timestampMs, entities, flows);
        return StateSubmitResult::InitialQueued;
    }
    const auto delta = mDeltaTracker.Update(entities, flows);
    if (StateDeltaTracker::Empty(delta))
    {
        return StateSubmitResult::NoChange;
    }
    return SubmitDelta(
               requestId,
               timestampMs,
               delta,
               entities,
               flows)
               ? StateSubmitResult::DeltaQueued
               : StateSubmitResult::ResyncQueued;
}

bool
AfsimBridgeController::SubmitDelta(
    const std::string& requestId,
    std::uint64_t timestampMs,
    const DeltaState& delta,
    const std::vector<EntityState>& currentEntities,
    const std::vector<FlowConfig>& currentFlows)
{
    mDeltaTracker.Reset(currentEntities, currentFlows);
    const auto deltaJson =
        BuildDeltaMessage(requestId, timestampMs, delta);
    const auto resyncJson = BuildInitMessage(
        requestId + "-resync",
        timestampMs,
        currentEntities,
        currentFlows);
    return mClient.QueueDelta(deltaJson, resyncJson);
}

std::size_t
AfsimBridgeController::ApplyPendingEffects(IAfsimEffectSink& sink)
{
    std::vector<EffectDecision> pending;
    {
        std::lock_guard<std::mutex> lock(mPendingEffectsMutex);
        pending.swap(mPendingEffects);
    }
    for (const auto& decision : pending)
    {
        sink.ApplyNetworkEffect(decision);
    }
    return pending.size();
}

EffectState
AfsimBridgeController::GetState(
    const std::string& entityId,
    const std::string& subsystemId) const
{
    return mGate.GetState(entityId, subsystemId);
}

bool
AfsimBridgeController::IsConnected() const
{
    return mClient.IsConnected();
}

std::size_t
AfsimBridgeController::PendingNetworkMessages() const
{
    return mClient.PendingCount();
}

void
AfsimBridgeController::OnJsonLine(const std::string& line)
{
    const auto decisions = mGate.UpdateFromMetricsJson(line);
    if (decisions.empty())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mPendingEffectsMutex);
    mPendingEffects.insert(
        mPendingEffects.end(),
        decisions.begin(),
        decisions.end());
}

} // namespace afsim_ns3
