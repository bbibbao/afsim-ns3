#include "AfsimBridgeController.hpp"
#include "AfsimNs3Protocol.hpp"
#include "GeoCoordinates.hpp"
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
