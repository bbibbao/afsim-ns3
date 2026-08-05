#include "AfsimBridgeController.hpp"
#include "AfsimNs3Protocol.hpp"
#include "GeoCoordinates.hpp"
#include "MessageTransportGate.hpp"
#include "NetworkEffectGate.hpp"
#include "StateDeltaTracker.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace afsim_ns3;

int
main()
{
    LocalEnuFrame frame(GeoPoint{0.0, 0.0, 0.0});
    const auto origin = frame.ToEnu(GeoPoint{0.0, 0.0, 0.0});
    assert(std::abs(origin.xM) < 1e-9);
    assert(std::abs(origin.yM) < 1e-9);
    assert(std::abs(origin.zM) < 1e-9);
    const auto east = frame.ToEnu(GeoPoint{0.0, 0.001, 0.0});
    assert(std::abs(east.xM - 111.319) < 0.01);
    const auto northVelocity = VelocityFromSpeed(100.0, 0.0, 0.0);
    assert(std::abs(northVelocity.vxMps) < 1e-9);
    assert(std::abs(northVelocity.vyMps - 100.0) < 1e-9);

    EntityState entity;
    entity.entityId = "AFSIM-\"1";
    entity.businessNodeId = "NODE-1";
    entity.alive = true;
    const std::string initial =
        BuildInitMessage("init-1", 0, {entity}, {});
    assert(initial.find("AFSIM-\\\"1") != std::string::npos);
    assert(initial.find("\"message_type\":\"afsim_init\"") !=
           std::string::npos);

    FlowConfig trackedFlow{
        "FLOW-1", "AFSIM-\"1", "AFSIM-2", 512, 20, 1000, true};
    StateDeltaTracker tracker;
    tracker.Reset({entity}, {trackedFlow});
    assert(StateDeltaTracker::Empty(tracker.Update({entity}, {trackedFlow})));
    entity.position.xM = 1.0;
    const auto moved = tracker.Update({entity}, {});
    assert(moved.entityUpserts.size() == 1);
    assert(moved.flowRemovals.size() == 1);

    NetworkEffectGate gate;
    const std::string metrics =
        "{\"protocol_version\":\"0.2\",\"message_type\":\"ns3_metrics\","
        "\"revision\":2,\"effects\":["
        "{\"entity_id\":\"AFSIM-1\",\"subsystem_id\":\"WEAPON-1\","
        "\"subsystem_type\":\"weapon\",\"peer_entity_id\":\"AFSIM-2\","
        "\"state\":\"BLOCKED\",\"reasons\":[\"DELAY_EXCEEDED\"]},"
        "{\"entity_id\":\"AFSIM-1\",\"subsystem_id\":\"RADAR-1\","
        "\"subsystem_type\":\"radar\",\"peer_entity_id\":\"AFSIM-2\","
        "\"state\":\"DEGRADED\",\"reasons\":[\"DELAY_EXCEEDED\"]}]}";
    const auto decisions = gate.UpdateFromMetricsJson(metrics);
    assert(decisions.size() == 2);
    assert(!gate.CanOperate("AFSIM-1", "WEAPON-1"));
    assert(gate.CanOperate("AFSIM-1", "RADAR-1"));
    assert(gate.LatestRevision() == 2);
    assert(gate.UpdateFromMetricsJson(metrics).empty());

    MessageTransportGate messageGate;
    const auto unknownMessage =
        messageGate.Decide("AFSIM-1", "AFSIM-2", 1);
    assert(unknownMessage.disposition == MessageDisposition::NoLink);
    assert(unknownMessage.reason == "NO_NETWORK_PROFILE");

    const std::string deliverMetrics =
        "{\"protocol_version\":\"0.2\",\"message_type\":\"ns3_metrics\","
        "\"revision\":10,\"links\":["
        "{\"source_entity_id\":\"AFSIM-1\",\"target_entity_id\":\"AFSIM-2\","
        "\"state\":\"UP\",\"delay_ms\":120.0,\"configured_loss_rate\":0.0}],"
        "\"metrics\":["
        "{\"source_entity_id\":\"AFSIM-1\",\"target_entity_id\":\"AFSIM-2\","
        "\"connected\":true,\"latency_ms\":120.25,\"loss_rate\":0.0,"
        "\"link_state\":\"UP\"}]}";
    assert(messageGate.UpdateFromMetricsJson(deliverMetrics));
    const auto delayedMessage =
        messageGate.Decide("AFSIM-1", "AFSIM-2", 2);
    assert(delayedMessage.disposition == MessageDisposition::Deliver);
    assert(std::abs(delayedMessage.delayMs - 120.25) < 1e-9);
    assert(delayedMessage.reason == "NONE");

    const std::string lossMetrics =
        "{\"protocol_version\":\"0.2\",\"message_type\":\"ns3_metrics\","
        "\"revision\":11,\"links\":["
        "{\"source_entity_id\":\"AFSIM-1\",\"target_entity_id\":\"AFSIM-2\","
        "\"state\":\"UP\",\"delay_ms\":5.0,\"configured_loss_rate\":1.0}],"
        "\"metrics\":["
        "{\"source_entity_id\":\"AFSIM-1\",\"target_entity_id\":\"AFSIM-2\","
        "\"connected\":true,\"latency_ms\":0.0,\"loss_rate\":1.0,"
        "\"link_state\":\"UP\"}]}";
    assert(messageGate.UpdateFromMetricsJson(lossMetrics));
    const auto lostMessage =
        messageGate.Decide("AFSIM-1", "AFSIM-2", 3);
    assert(lostMessage.disposition == MessageDisposition::Drop);
    assert(lostMessage.reason == "PACKET_LOSS");

    const std::string sampledLossMetrics =
        "{\"protocol_version\":\"0.2\",\"message_type\":\"ns3_metrics\","
        "\"revision\":12,\"links\":["
        "{\"source_entity_id\":\"AFSIM-1\",\"target_entity_id\":\"AFSIM-2\","
        "\"state\":\"UP\",\"delay_ms\":5.0,\"configured_loss_rate\":0.5}],"
        "\"metrics\":["
        "{\"source_entity_id\":\"AFSIM-1\",\"target_entity_id\":\"AFSIM-2\","
        "\"connected\":true,\"latency_ms\":5.0,\"loss_rate\":1.0,"
        "\"link_state\":\"UP\"}]}";
    assert(messageGate.UpdateFromMetricsJson(sampledLossMetrics));
    const auto sampledLossMessage =
        messageGate.Decide("AFSIM-1", "AFSIM-2", 4);
    assert(std::abs(sampledLossMessage.lossRate - 0.5) < 1e-9);

    const std::string downMetrics =
        "{\"protocol_version\":\"0.2\",\"message_type\":\"ns3_metrics\","
        "\"revision\":13,\"links\":["
        "{\"source_entity_id\":\"AFSIM-1\",\"target_entity_id\":\"AFSIM-2\","
        "\"state\":\"DOWN\",\"delay_ms\":0.0,\"configured_loss_rate\":0.0}],"
        "\"metrics\":["
        "{\"source_entity_id\":\"AFSIM-1\",\"target_entity_id\":\"AFSIM-2\","
        "\"connected\":false,\"latency_ms\":0.0,\"loss_rate\":1.0,"
        "\"link_state\":\"DOWN\"}]}";
    assert(messageGate.UpdateFromMetricsJson(downMetrics));
    const auto blockedMessage =
        messageGate.Decide("AFSIM-1", "AFSIM-2", 5);
    assert(blockedMessage.disposition == MessageDisposition::NoLink);
    assert(blockedMessage.reason == "LINK_DOWN");
    assert(!messageGate.UpdateFromMetricsJson(lossMetrics));

    TcpClientConfig queueConfig;
    queueConfig.host = "127.0.0.1";
    queueConfig.port = 9;
    queueConfig.maxPendingMessages = 4;
    AfsimBridgeController controller(queueConfig);
    EntityState movingEntity;
    movingEntity.entityId = "AFSIM-PERF";
    movingEntity.businessNodeId = "NODE-PERF";
    movingEntity.alive = true;
    std::vector<EntityState> movingEntities{movingEntity};
    const auto firstSubmit = controller.SubmitCurrentState(
        "bounded-init",
        0,
        movingEntities,
        {});
    assert(firstSubmit == StateSubmitResult::InitialQueued);

    std::size_t resyncCount = 0;
    const auto submitStarted = std::chrono::steady_clock::now();
    for (std::uint64_t revision = 1; revision <= 1000; ++revision)
    {
        movingEntities[0].position.xM = static_cast<double>(revision);
        const auto result = controller.SubmitCurrentState(
            "bounded-" + std::to_string(revision),
            revision,
            movingEntities,
            {});
        assert(result == StateSubmitResult::DeltaQueued ||
               result == StateSubmitResult::ResyncQueued);
        if (result == StateSubmitResult::ResyncQueued)
        {
            ++resyncCount;
        }
        assert(controller.PendingNetworkMessages() <= 4);
    }
    const auto submitElapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - submitStarted);
    assert(resyncCount > 0);
    assert(submitElapsed < std::chrono::seconds(2));
    return 0;
}
