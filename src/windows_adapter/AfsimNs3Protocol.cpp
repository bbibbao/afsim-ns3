#include "AfsimNs3Protocol.hpp"

#include <iomanip>
#include <sstream>

namespace afsim_ns3
{
namespace
{

std::string
Quote(const std::string& value)
{
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20)
            {
                output << "\\u" << std::hex << std::setw(4)
                       << std::setfill('0')
                       << static_cast<unsigned int>(character)
                       << std::dec << std::setfill(' ');
            }
            else
            {
                output << static_cast<char>(character);
            }
        }
    }
    output << '"';
    return output.str();
}

const char*
Boolean(bool value)
{
    return value ? "true" : "false";
}

void
WriteDevice(std::ostream& output, const DeviceConfig& device)
{
    output << "{\"device_id\":" << Quote(device.deviceId)
           << ",\"peer_entity_id\":" << Quote(device.peerEntityId)
           << ",\"kind\":" << Quote(device.kind)
           << ",\"data_rate_bps\":" << device.dataRateBps
           << ",\"delay_ms\":" << device.delayMs
           << ",\"loss_rate\":" << device.lossRate
           << ",\"link_state\":" << Quote(device.linkState) << '}';
}

void
WritePolicy(std::ostream& output, const EffectPolicy& policy)
{
    output << "{\"subsystem_id\":" << Quote(policy.subsystemId)
           << ",\"subsystem_type\":" << Quote(policy.subsystemType)
           << ",\"peer_entity_id\":" << Quote(policy.peerEntityId)
           << ",\"max_delay_ms\":" << policy.maxDelayMs
           << ",\"max_loss_rate\":" << policy.maxLossRate
           << ",\"min_throughput_bps\":" << policy.minThroughputBps
           << ",\"violation_state\":" << Quote(policy.violationState)
           << '}';
}

template <typename Value, typename Writer>
void
WriteArray(
    std::ostream& output,
    const std::vector<Value>& values,
    Writer writer)
{
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0)
        {
            output << ',';
        }
        writer(output, values[index]);
    }
    output << ']';
}

void
WriteEntity(std::ostream& output, const EntityState& entity)
{
    output << "{\"entity_id\":" << Quote(entity.entityId)
           << ",\"business_node_id\":" << Quote(entity.businessNodeId)
           << ",\"parent_entity_id\":";
    if (entity.hasParent)
    {
        output << Quote(entity.parentEntityId);
    }
    else
    {
        output << "null";
    }
    output << ",\"position\":{\"x_m\":" << entity.position.xM
           << ",\"y_m\":" << entity.position.yM
           << ",\"z_m\":" << entity.position.zM << '}'
           << ",\"velocity\":{\"vx_mps\":" << entity.velocity.vxMps
           << ",\"vy_mps\":" << entity.velocity.vyMps
           << ",\"vz_mps\":" << entity.velocity.vzMps << '}'
           << ",\"heading_deg\":" << entity.headingDeg
           << ",\"alive\":" << Boolean(entity.alive)
           << ",\"devices\":";
    WriteArray(output, entity.devices, WriteDevice);
    output << ",\"effect_policies\":";
    WriteArray(output, entity.effectPolicies, WritePolicy);
    output << '}';
}

void
WriteFlow(std::ostream& output, const FlowConfig& flow)
{
    output << "{\"flow_id\":" << Quote(flow.flowId)
           << ",\"source_entity_id\":" << Quote(flow.sourceEntityId)
           << ",\"target_entity_id\":" << Quote(flow.targetEntityId)
           << ",\"packet_size_bytes\":" << flow.packetSizeBytes
           << ",\"interval_ms\":" << flow.intervalMs
           << ",\"duration_ms\":" << flow.durationMs
           << ",\"active\":" << Boolean(flow.active) << '}';
}

void
WriteStrings(std::ostream& output, const std::vector<std::string>& values)
{
    WriteArray(
        output,
        values,
        [](std::ostream& target, const std::string& value)
        { target << Quote(value); });
}

void
WriteEnvelope(
    std::ostream& output,
    const char* messageType,
    const std::string& requestId,
    std::uint64_t timestampMs)
{
    output << "{\"protocol_version\":\"0.2\",\"message_type\":"
           << Quote(messageType) << ",\"request_id\":" << Quote(requestId)
           << ",\"timestamp_ms\":" << timestampMs;
}

} // namespace

std::string
BuildInitMessage(
    const std::string& requestId,
    std::uint64_t timestampMs,
    const std::vector<EntityState>& entities,
    const std::vector<FlowConfig>& flows)
{
    std::ostringstream output;
    output << std::setprecision(15);
    WriteEnvelope(output, "afsim_init", requestId, timestampMs);
    output << ",\"entities\":";
    WriteArray(output, entities, WriteEntity);
    output << ",\"flows\":";
    WriteArray(output, flows, WriteFlow);
    output << '}';
    return output.str();
}

std::string
BuildDeltaMessage(
    const std::string& requestId,
    std::uint64_t timestampMs,
    const DeltaState& delta)
{
    std::ostringstream output;
    output << std::setprecision(15);
    WriteEnvelope(output, "afsim_delta", requestId, timestampMs);
    output << ",\"entity_upserts\":";
    WriteArray(output, delta.entityUpserts, WriteEntity);
    output << ",\"entity_removals\":";
    WriteStrings(output, delta.entityRemovals);
    output << ",\"flow_upserts\":";
    WriteArray(output, delta.flowUpserts, WriteFlow);
    output << ",\"flow_removals\":";
    WriteStrings(output, delta.flowRemovals);
    output << '}';
    return output.str();
}

} // namespace afsim_ns3
