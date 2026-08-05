#include "AfsimBridgeController.hpp"

#include <exception>
#include <utility>

namespace afsim_ns3
{
namespace
{

auto
EffectKey(const EffectDecision& decision)
{
    return std::make_tuple(
        decision.entityId,
        decision.subsystemType,
        decision.subsystemId,
        decision.peerEntityId);
}

} // namespace

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
AfsimBridgeController::SetEffectErrorHandler(EffectErrorHandler handler)
{
    mEffectErrorHandler = std::move(handler);
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
    decltype(mPendingEffects) pending;
    {
        std::lock_guard<std::mutex> lock(mPendingEffectsMutex);
        pending.swap(mPendingEffects);
    }
    std::size_t applied = 0;
    for (const auto& entry : pending)
    {
        try
        {
            sink.ApplyNetworkEffect(entry.second);
            ++applied;
        }
        catch (const std::exception& error)
        {
            ReportEffectError(entry.second, error.what());
        }
        catch (...)
        {
            ReportEffectError(entry.second, "unknown exception");
        }
    }
    return applied;
}

EffectState
AfsimBridgeController::GetState(
    const std::string& entityId,
    const std::string& subsystemId) const
{
    return mGate.GetState(entityId, subsystemId);
}

MessageTransportDecision
AfsimBridgeController::DecideMessage(
    const std::string& sourceEntityId,
    const std::string& targetEntityId,
    std::uint64_t messageSerial) const
{
    return mMessageGate.Decide(
        sourceEntityId,
        targetEntityId,
        messageSerial);
}

std::uint64_t
AfsimBridgeController::LatestMetricsRevision() const
{
    return mMessageGate.LatestRevision();
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
    mMessageGate.UpdateFromMetricsJson(line);
    const auto decisions = mGate.UpdateFromMetricsJson(line);
    if (decisions.empty())
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mPendingEffectsMutex);
    for (const auto& decision : decisions)
    {
        // Keep only the newest decision for each affected link and subsystem.
        mPendingEffects[EffectKey(decision)] = decision;
    }
}

void
AfsimBridgeController::ReportEffectError(
    const EffectDecision& decision,
    const std::string& message) const noexcept
{
    if (!mEffectErrorHandler)
    {
        return;
    }
    try
    {
        mEffectErrorHandler(decision, message);
    }
    catch (...)
    {
        // A diagnostic hook must never prevent the remaining effects.
    }
}

} // namespace afsim_ns3
