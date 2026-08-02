#pragma once

#include "AfsimNs3Protocol.hpp"
#include "NetworkEffectGate.hpp"
#include "StateDeltaTracker.hpp"
#include "TcpJsonlClient.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace afsim_ns3
{

class IAfsimEffectSink
{
  public:
    virtual ~IAfsimEffectSink() = default;

    // Implement this with the AFSIM 2.9cn APIs for the named weapon/radar.
    // It is invoked only by ApplyPendingEffects on the simulation thread.
    virtual void ApplyNetworkEffect(const EffectDecision& decision) = 0;
};

enum class StateSubmitResult
{
    InitialQueued,
    DeltaQueued,
    ResyncQueued,
    NoChange
};

class AfsimBridgeController
{
  public:
    explicit AfsimBridgeController(TcpClientConfig config);
    ~AfsimBridgeController();

    AfsimBridgeController(const AfsimBridgeController&) = delete;
    AfsimBridgeController& operator=(const AfsimBridgeController&) = delete;

    void Start();
    void Stop();

    void SubmitInitial(
        const std::string& requestId,
        std::uint64_t timestampMs,
        const std::vector<EntityState>& entities,
        const std::vector<FlowConfig>& flows);

    // Normal event-step entry: the first call sends full state, later calls
    // compare against the previous frame and send only changes.
    StateSubmitResult SubmitCurrentState(
        const std::string& requestId,
        std::uint64_t timestampMs,
        const std::vector<EntityState>& entities,
        const std::vector<FlowConfig>& flows);

    // currentEntities/currentFlows are serialized only as reconnect recovery;
    // the normal LAN message contains only delta.
    bool SubmitDelta(
        const std::string& requestId,
        std::uint64_t timestampMs,
        const DeltaState& delta,
        const std::vector<EntityState>& currentEntities,
        const std::vector<FlowConfig>& currentFlows);

    // Call once per AFSIM event step. No socket or ns-3 work occurs here.
    std::size_t ApplyPendingEffects(IAfsimEffectSink& sink);

    EffectState GetState(
        const std::string& entityId,
        const std::string& subsystemId) const;

    bool IsConnected() const;
    std::size_t PendingNetworkMessages() const;

  private:
    void OnJsonLine(const std::string& line);

    TcpJsonlClient mClient;
    StateDeltaTracker mDeltaTracker;
    NetworkEffectGate mGate;
    std::mutex mPendingEffectsMutex;
    std::vector<EffectDecision> mPendingEffects;
};

} // namespace afsim_ns3
