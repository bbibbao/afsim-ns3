#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ns3;

namespace
{

struct NodeInput
{
    uint32_t index{};
    double x{};
    double y{};
    double z{};
    bool alive{};
};

struct LinkInput
{
    uint32_t index{};
    uint32_t source{};
    uint32_t target{};
    uint64_t dataRateBps{};
    double delayMs{};
    double lossRate{};
    bool up{};
};

struct FlowInput
{
    uint32_t index{};
    uint32_t source{};
    uint32_t target{};
    uint32_t packetSizeBytes{};
    uint32_t intervalMs{};
    uint32_t durationMs{};
    bool enabled{};
};

struct Scenario
{
    std::string id;
    uint32_t expectedNodes{};
    uint32_t expectedLinks{};
    uint32_t expectedFlows{};
    std::vector<NodeInput> nodes;
    std::vector<LinkInput> links;
    std::vector<FlowInput> flows;
};

std::vector<std::string>
SplitTabs(const std::string& line)
{
    std::vector<std::string> parts;
    std::stringstream stream(line);
    std::string part;
    while (std::getline(stream, part, '\t'))
    {
        parts.push_back(part);
    }
    return parts;
}

uint32_t
ToUint32(const std::string& value)
{
    const auto parsed = std::stoull(value);
    if (parsed > UINT32_MAX)
    {
        throw std::runtime_error("integer exceeds uint32 range");
    }
    return static_cast<uint32_t>(parsed);
}

uint64_t
ToUint64(const std::string& value)
{
    return std::stoull(value);
}

double
ToDouble(const std::string& value)
{
    return std::stod(value);
}

bool
ToBool(const std::string& value)
{
    if (value == "1")
    {
        return true;
    }
    if (value == "0")
    {
        return false;
    }
    throw std::runtime_error("boolean must be 0 or 1");
}

Ipv4Address
FirstNodeAddress(Ptr<Node> node)
{
    const auto ipv4 = node->GetObject<Ipv4>();
    for (uint32_t interfaceIndex = 1;
         interfaceIndex < ipv4->GetNInterfaces();
         ++interfaceIndex)
    {
        for (uint32_t addressIndex = 0;
             addressIndex < ipv4->GetNAddresses(interfaceIndex);
             ++addressIndex)
        {
            const auto address =
                ipv4->GetAddress(interfaceIndex, addressIndex).GetLocal();
            if (address != Ipv4Address::GetZero() &&
                address != Ipv4Address::GetLoopback())
            {
                return address;
            }
        }
    }
    return Ipv4Address::GetZero();
}

void
ValidateScenario(const Scenario& scenario)
{
    if (scenario.nodes.size() != scenario.expectedNodes ||
        scenario.links.size() != scenario.expectedLinks ||
        scenario.flows.size() != scenario.expectedFlows)
    {
        throw std::runtime_error("scenario record count mismatch");
    }
    for (uint32_t index = 0; index < scenario.nodes.size(); ++index)
    {
        if (scenario.nodes[index].index != index)
        {
            throw std::runtime_error("node indexes must be contiguous");
        }
    }
    for (const auto& link : scenario.links)
    {
        if (link.source >= scenario.nodes.size() ||
            link.target >= scenario.nodes.size())
        {
            throw std::runtime_error("link references unknown node");
        }
        if (link.dataRateBps == 0 || link.delayMs < 0.0 ||
            link.lossRate < 0.0 || link.lossRate > 1.0)
        {
            throw std::runtime_error("invalid link parameters");
        }
    }
    for (const auto& flow : scenario.flows)
    {
        if (flow.source >= scenario.nodes.size() ||
            flow.target >= scenario.nodes.size())
        {
            throw std::runtime_error("flow references unknown node");
        }
        if (flow.packetSizeBytes < 64 || flow.intervalMs == 0 ||
            flow.durationMs < 100)
        {
            throw std::runtime_error("invalid flow parameters");
        }
    }
}

bool
HasActivePath(
    const Scenario& scenario,
    uint32_t source,
    uint32_t target)
{
    if (source >= scenario.nodes.size() || target >= scenario.nodes.size() ||
        !scenario.nodes[source].alive || !scenario.nodes[target].alive)
    {
        return false;
    }

    std::vector<std::vector<uint32_t>> adjacency(scenario.nodes.size());
    for (const auto& link : scenario.links)
    {
        if (!link.up || !scenario.nodes[link.source].alive ||
            !scenario.nodes[link.target].alive)
        {
            continue;
        }
        adjacency[link.source].push_back(link.target);
        adjacency[link.target].push_back(link.source);
    }

    std::vector<bool> visited(scenario.nodes.size(), false);
    std::vector<uint32_t> pending{source};
    visited[source] = true;
    while (!pending.empty())
    {
        const auto current = pending.back();
        pending.pop_back();
        if (current == target)
        {
            return true;
        }
        for (const auto next : adjacency[current])
        {
            if (!visited[next])
            {
                visited[next] = true;
                pending.push_back(next);
            }
        }
    }
    return false;
}

void
RunScenario(const Scenario& scenario)
{
    ValidateScenario(scenario);

    NodeContainer nodes;
    nodes.Create(scenario.nodes.size());

    auto positions = CreateObject<ListPositionAllocator>();
    for (const auto& input : scenario.nodes)
    {
        positions->Add(Vector(input.x, input.y, input.z));
    }
    MobilityHelper mobility;
    mobility.SetPositionAllocator(positions);
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.Install(nodes);

    InternetStackHelper internet;
    internet.Install(nodes);

    for (const auto& link : scenario.links)
    {
        if (!link.up || !scenario.nodes[link.source].alive ||
            !scenario.nodes[link.target].alive)
        {
            continue;
        }

        PointToPointHelper helper;
        helper.SetDeviceAttribute(
            "DataRate",
            DataRateValue(DataRate(link.dataRateBps)));
        helper.SetChannelAttribute(
            "Delay",
            TimeValue(MilliSeconds(link.delayMs)));

        NodeContainer pair(nodes.Get(link.source), nodes.Get(link.target));
        auto devices = helper.Install(pair);

        for (uint32_t deviceIndex = 0; deviceIndex < devices.GetN();
             ++deviceIndex)
        {
            auto errorModel = CreateObject<RateErrorModel>();
            errorModel->SetUnit(RateErrorModel::ERROR_UNIT_PACKET);
            errorModel->SetRate(link.lossRate);
            devices.Get(deviceIndex)->SetAttribute(
                "ReceiveErrorModel",
                PointerValue(errorModel));
        }

        const uint32_t secondOctet = 1 + (link.index / 256);
        const uint32_t thirdOctet = link.index % 256;
        std::ostringstream subnet;
        subnet << "10." << secondOctet << "." << thirdOctet << ".0";
        Ipv4AddressHelper addresses;
        addresses.SetBase(subnet.str().c_str(), "255.255.255.252");
        addresses.Assign(devices);
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    FlowMonitorHelper monitorHelper;
    auto monitor = monitorHelper.InstallAll();
    std::map<uint16_t, uint32_t> portToFlowIndex;
    double stopSeconds = 0.5;

    for (const auto& flow : scenario.flows)
    {
        if (!flow.enabled || !scenario.nodes[flow.source].alive ||
            !scenario.nodes[flow.target].alive)
        {
            continue;
        }
        const auto targetAddress = FirstNodeAddress(nodes.Get(flow.target));
        if (targetAddress == Ipv4Address::GetZero())
        {
            continue;
        }

        const uint16_t port =
            static_cast<uint16_t>(9000 + (flow.index % 50000));
        portToFlowIndex[port] = flow.index;

        UdpServerHelper server(port);
        auto serverApps = server.Install(nodes.Get(flow.target));
        serverApps.Start(Seconds(0.0));
        serverApps.Stop(MilliSeconds(100 + flow.durationMs + 200));

        UdpClientHelper client(targetAddress, port);
        client.SetAttribute(
            "MaxPackets",
            UintegerValue(
                std::max<uint32_t>(
                    1,
                    flow.durationMs / flow.intervalMs)));
        client.SetAttribute(
            "Interval",
            TimeValue(MilliSeconds(flow.intervalMs)));
        client.SetAttribute(
            "PacketSize",
            UintegerValue(flow.packetSizeBytes));
        auto clientApps = client.Install(nodes.Get(flow.source));
        clientApps.Start(MilliSeconds(100));
        clientApps.Stop(MilliSeconds(100 + flow.durationMs));

        stopSeconds = std::max(
            stopSeconds,
            (100.0 + static_cast<double>(flow.durationMs) + 200.0) /
                1000.0);
    }

    Simulator::Stop(Seconds(stopSeconds));
    Simulator::Run();
    monitor->CheckForLostPackets();

    std::map<uint32_t, FlowMonitor::FlowStats> measured;
    auto classifier =
        DynamicCast<Ipv4FlowClassifier>(monitorHelper.GetClassifier());
    for (const auto& entry : monitor->GetFlowStats())
    {
        const auto tuple = classifier->FindFlow(entry.first);
        const auto found = portToFlowIndex.find(tuple.destinationPort);
        if (found != portToFlowIndex.end() && tuple.protocol == 17)
        {
            measured[found->second] = entry.second;
        }
    }

    std::cout << std::fixed << std::setprecision(6);
    for (const auto& flow : scenario.flows)
    {
        const bool connected =
            flow.enabled &&
            HasActivePath(scenario, flow.source, flow.target);
        double latencyMs = 0.0;
        double lossRate = 1.0;
        double throughputBps = 0.0;
        uint64_t txPackets = 0;
        uint64_t rxPackets = 0;

        const auto found = measured.find(flow.index);
        if (found != measured.end())
        {
            const auto& stats = found->second;
            txPackets = stats.txPackets;
            rxPackets = stats.rxPackets;
            if (rxPackets > 0)
            {
                latencyMs =
                    stats.delaySum.GetSeconds() * 1000.0 /
                    static_cast<double>(rxPackets);
            }
            if (txPackets > 0)
            {
                lossRate =
                    static_cast<double>(txPackets - rxPackets) /
                    static_cast<double>(txPackets);
            }
            const double activeSeconds =
                (stats.timeLastRxPacket - stats.timeFirstTxPacket)
                    .GetSeconds();
            if (rxPackets > 0 && activeSeconds > 0.0)
            {
                throughputBps =
                    static_cast<double>(stats.rxBytes) * 8.0 /
                    activeSeconds;
            }
        }

        std::cout << "RESULT\t" << flow.index << "\t"
                  << (connected ? 1 : 0) << "\t" << latencyMs << "\t"
                  << lossRate << "\t" << throughputBps << "\t"
                  << txPackets << "\t" << rxPackets << "\t"
                  << (connected ? "UP" : "DOWN") << "\n";
    }
    std::cout << "DONE\t" << scenario.id << "\n";
    std::cout.flush();
    Simulator::Destroy();
}

} // namespace

int
main()
{
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    Scenario scenario;
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line.empty())
        {
            continue;
        }
        if (line == "QUIT")
        {
            return 0;
        }

        try
        {
            const auto parts = SplitTabs(line);
            if (parts.empty())
            {
                continue;
            }
            if (parts[0] == "SCENARIO")
            {
                if (parts.size() != 5)
                {
                    throw std::runtime_error("invalid SCENARIO line");
                }
                scenario = Scenario{};
                scenario.id = parts[1];
                scenario.expectedNodes = ToUint32(parts[2]);
                scenario.expectedLinks = ToUint32(parts[3]);
                scenario.expectedFlows = ToUint32(parts[4]);
            }
            else if (parts[0] == "NODE")
            {
                if (parts.size() != 6)
                {
                    throw std::runtime_error("invalid NODE line");
                }
                scenario.nodes.push_back(
                    NodeInput{
                        ToUint32(parts[1]),
                        ToDouble(parts[2]),
                        ToDouble(parts[3]),
                        ToDouble(parts[4]),
                        ToBool(parts[5]),
                    });
            }
            else if (parts[0] == "LINK")
            {
                if (parts.size() != 8)
                {
                    throw std::runtime_error("invalid LINK line");
                }
                scenario.links.push_back(
                    LinkInput{
                        ToUint32(parts[1]),
                        ToUint32(parts[2]),
                        ToUint32(parts[3]),
                        ToUint64(parts[4]),
                        ToDouble(parts[5]),
                        ToDouble(parts[6]),
                        ToBool(parts[7]),
                    });
            }
            else if (parts[0] == "FLOW")
            {
                if (parts.size() != 8)
                {
                    throw std::runtime_error("invalid FLOW line");
                }
                scenario.flows.push_back(
                    FlowInput{
                        ToUint32(parts[1]),
                        ToUint32(parts[2]),
                        ToUint32(parts[3]),
                        ToUint32(parts[4]),
                        ToUint32(parts[5]),
                        ToUint32(parts[6]),
                        ToBool(parts[7]),
                    });
            }
            else if (parts[0] == "RUN")
            {
                RunScenario(scenario);
            }
            else
            {
                throw std::runtime_error("unknown runner command");
            }
        }
        catch (const std::exception& error)
        {
            std::cout << "ERROR\t" << error.what() << "\n";
            std::cout.flush();
            Simulator::Destroy();
            scenario = Scenario{};
        }
    }
    return 0;
}
