#include "AfsimBridgeController.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace afsim_ns3;

class RecordingSink final : public IAfsimEffectSink
{
  public:
    void ApplyNetworkEffect(const EffectDecision& decision) override
    {
        attempts.emplace_back(decision.revision, decision.subsystemId);
        if (decision.revision == failingRevision &&
            decision.subsystemId == failingSubsystem)
        {
            throw std::runtime_error("injected effect failure");
        }
        states[{decision.revision, decision.subsystemId}] = decision.state;
    }

    std::map<std::pair<std::uint64_t, std::string>, EffectState> states;
    std::vector<std::pair<std::uint64_t, std::string>> attempts;
    std::uint64_t failingRevision{};
    std::string failingSubsystem;
};

EntityState
SourceEntity(double delayMs)
{
    EntityState source;
    source.entityId = "AFSIM-1";
    source.businessNodeId = "NODE-1";
    source.alive = true;
    source.devices.push_back(
        DeviceConfig{
            "LINK-1",
            "AFSIM-2",
            "wired",
            10000000,
            delayMs,
            0.0,
            "UP",
        });
    source.effectPolicies.push_back(
        EffectPolicy{
            "WEAPON-1",
            "weapon",
            "AFSIM-2",
            50.0,
            0.2,
            100000.0,
            "BLOCKED",
        });
    source.effectPolicies.push_back(
        EffectPolicy{
            "RADAR-1",
            "radar",
            "AFSIM-2",
            50.0,
            0.2,
            100000.0,
            "DEGRADED",
        });
    return source;
}

EntityState
TargetEntity()
{
    EntityState target;
    target.entityId = "AFSIM-2";
    target.businessNodeId = "NODE-2";
    target.position.xM = 1000.0;
    target.headingDeg = 180.0;
    target.alive = true;
    return target;
}

bool
WaitForRevision(
    AfsimBridgeController& controller,
    RecordingSink& sink,
    std::uint64_t revision)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < deadline)
    {
        controller.ApplyPendingEffects(sink);
        if (sink.states.count({revision, "WEAPON-1"}) != 0 &&
            sink.states.count({revision, "RADAR-1"}) != 0)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

struct RecordedEffectError
{
    EffectDecision decision;
    std::string message;
};

bool
WaitForIsolatedRevision(
    AfsimBridgeController& controller,
    RecordingSink& sink,
    const std::vector<RecordedEffectError>& errors,
    std::uint64_t revision)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(12);
    while (std::chrono::steady_clock::now() < deadline)
    {
        controller.ApplyPendingEffects(sink);
        const bool laterEffectApplied =
            sink.states.count({revision, "WEAPON-1"}) != 0;
        const bool failedEffectRecorded =
            !errors.empty() && errors.back().decision.revision == revision &&
            errors.back().decision.subsystemId == "RADAR-1";
        if (laterEffectApplied && failedEffectRecorded)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

int
main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: windows_adapter_probe HOST PORT\n";
        return 2;
    }
    TcpClientConfig config;
    config.host = argv[1];
    config.port =
        static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 10));
    config.reconnectDelayMs = 100;

    AfsimBridgeController controller(config);
    RecordingSink sink;
    std::vector<RecordedEffectError> errors;
    controller.SetEffectErrorHandler(
        [&errors](const EffectDecision& decision, const std::string& message)
        {
            errors.push_back(RecordedEffectError{decision, message});
        });
    FlowConfig flow{
        "FLOW-1",
        "AFSIM-1",
        "AFSIM-2",
        1024,
        10,
        1000,
        true,
    };
    auto source = SourceEntity(10.0);
    const auto target = TargetEntity();
    std::vector<EntityState> entities{source, target};
    std::vector<FlowConfig> flows{flow};

    controller.Start();
    if (controller.SubmitCurrentState(
            "cpp-init-001", 0, entities, flows) !=
        StateSubmitResult::InitialQueued)
    {
        controller.Stop();
        std::cerr << "initial state was not queued\n";
        return 3;
    }
    if (!WaitForRevision(controller, sink, 1))
    {
        controller.Stop();
        std::cerr << "initial ns-3 metrics timed out\n";
        return 4;
    }
    if (sink.states[{1, "WEAPON-1"}] != EffectState::Available ||
        sink.states[{1, "RADAR-1"}] != EffectState::Available)
    {
        controller.Stop();
        std::cerr << "initial effects were not available\n";
        return 5;
    }

    if (controller.SubmitCurrentState(
            "cpp-no-change", 500, entities, flows) !=
        StateSubmitResult::NoChange)
    {
        controller.Stop();
        std::cerr << "unchanged frame generated network traffic\n";
        return 6;
    }

    source = SourceEntity(120.0);
    sink.failingRevision = 2;
    sink.failingSubsystem = "RADAR-1";
    entities[0] = source;
    if (controller.SubmitCurrentState(
            "cpp-delta-001", 1000, entities, flows) !=
        StateSubmitResult::DeltaQueued)
    {
        controller.Stop();
        std::cerr << "changed frame was not queued as delta\n";
        return 7;
    }
    if (!WaitForIsolatedRevision(controller, sink, errors, 2))
    {
        controller.Stop();
        std::cerr << "isolated effect metrics timed out\n";
        return 8;
    }
    controller.Stop();

    if (sink.states[{2, "WEAPON-1"}] != EffectState::Blocked ||
        sink.states.count({2, "RADAR-1"}) != 0)
    {
        std::cerr << "a later effect was not applied after an isolated failure\n";
        return 9;
    }
    if (errors.size() != 1 || errors[0].decision.revision != 2 ||
        errors[0].decision.subsystemId != "RADAR-1" ||
        errors[0].message != "injected effect failure")
    {
        std::cerr << "effect failure was not recorded\n";
        return 10;
    }
    const auto radarAttempt =
        std::make_pair<std::uint64_t, std::string>(2, "RADAR-1");
    const auto weaponAttempt =
        std::make_pair<std::uint64_t, std::string>(2, "WEAPON-1");
    if (sink.attempts.size() < 4 ||
        sink.attempts[sink.attempts.size() - 2] != radarAttempt ||
        sink.attempts.back() != weaponAttempt)
    {
        std::cerr << "effect processing did not continue in order\n";
        return 11;
    }

    std::cout << "revision=2 radar_error=recorded weapon=BLOCKED continued=1\n";
    return 0;
}
