#include "NetworkEffectGate.hpp"

#include <cctype>
#include <limits>
#include <stdexcept>

namespace afsim_ns3
{
namespace
{

std::size_t
ValueStart(
    const std::string& json,
    const std::string& key,
    std::size_t begin = 0)
{
    const std::string needle = '"' + key + '"';
    const auto keyPosition = json.find(needle, begin);
    if (keyPosition == std::string::npos)
    {
        throw std::runtime_error("missing JSON field: " + key);
    }
    const auto colon = json.find(':', keyPosition + needle.size());
    if (colon == std::string::npos)
    {
        throw std::runtime_error("invalid JSON field: " + key);
    }
    auto position = colon + 1;
    while (position < json.size() &&
           std::isspace(static_cast<unsigned char>(json[position])))
    {
        ++position;
    }
    return position;
}

std::string
JsonString(
    const std::string& json,
    const std::string& key,
    std::size_t begin = 0)
{
    auto position = ValueStart(json, key, begin);
    if (position >= json.size() || json[position] != '"')
    {
        throw std::runtime_error("JSON field is not a string: " + key);
    }
    ++position;
    std::string result;
    while (position < json.size())
    {
        const char character = json[position++];
        if (character == '"')
        {
            return result;
        }
        if (character != '\\')
        {
            result.push_back(character);
            continue;
        }
        if (position >= json.size())
        {
            break;
        }
        const char escaped = json[position++];
        switch (escaped)
        {
        case '"':
        case '\\':
        case '/':
            result.push_back(escaped);
            break;
        case 'b':
            result.push_back('\b');
            break;
        case 'f':
            result.push_back('\f');
            break;
        case 'n':
            result.push_back('\n');
            break;
        case 'r':
            result.push_back('\r');
            break;
        case 't':
            result.push_back('\t');
            break;
        default:
            throw std::runtime_error("unsupported JSON string escape");
        }
    }
    throw std::runtime_error("unterminated JSON string");
}

std::uint64_t
JsonUint64(const std::string& json, const std::string& key)
{
    auto position = ValueStart(json, key);
    if (position >= json.size() ||
        !std::isdigit(static_cast<unsigned char>(json[position])))
    {
        throw std::runtime_error("JSON field is not an unsigned integer: " + key);
    }
    std::uint64_t value = 0;
    while (position < json.size() &&
           std::isdigit(static_cast<unsigned char>(json[position])))
    {
        const unsigned int digit =
            static_cast<unsigned int>(json[position++] - '0');
        if (value >
            (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
        {
            throw std::runtime_error("JSON integer exceeds uint64 range");
        }
        value = value * 10 + digit;
    }
    return value;
}

std::size_t
MatchingEnd(
    const std::string& json,
    std::size_t begin,
    char opening,
    char closing)
{
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (auto position = begin; position < json.size(); ++position)
    {
        const char character = json[position];
        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (character == '\\')
            {
                escaped = true;
            }
            else if (character == '"')
            {
                inString = false;
            }
            continue;
        }
        if (character == '"')
        {
            inString = true;
        }
        else if (character == opening)
        {
            ++depth;
        }
        else if (character == closing && --depth == 0)
        {
            return position;
        }
    }
    throw std::runtime_error("unterminated JSON container");
}

EffectState
ParseState(const std::string& value)
{
    if (value == "AVAILABLE")
    {
        return EffectState::Available;
    }
    if (value == "DEGRADED")
    {
        return EffectState::Degraded;
    }
    if (value == "BLOCKED")
    {
        return EffectState::Blocked;
    }
    throw std::runtime_error("unknown network effect state");
}

std::vector<std::string>
EffectObjects(const std::string& json)
{
    auto position = ValueStart(json, "effects");
    if (position >= json.size() || json[position] != '[')
    {
        throw std::runtime_error("effects must be a JSON array");
    }
    const auto arrayEnd = MatchingEnd(json, position, '[', ']');
    std::vector<std::string> objects;
    ++position;
    while (position < arrayEnd)
    {
        while (position < arrayEnd &&
               (std::isspace(static_cast<unsigned char>(json[position])) ||
                json[position] == ','))
        {
            ++position;
        }
        if (position == arrayEnd)
        {
            break;
        }
        if (json[position] != '{')
        {
            throw std::runtime_error("effects entries must be objects");
        }
        const auto objectEnd = MatchingEnd(json, position, '{', '}');
        objects.push_back(json.substr(position, objectEnd - position + 1));
        position = objectEnd + 1;
    }
    return objects;
}

} // namespace

std::vector<EffectDecision>
NetworkEffectGate::UpdateFromMetricsJson(const std::string& jsonLine)
{
    if (JsonString(jsonLine, "message_type") != "ns3_metrics")
    {
        return {};
    }
    const auto revision = JsonUint64(jsonLine, "revision");
    std::vector<EffectDecision> decisions;
    for (const auto& object : EffectObjects(jsonLine))
    {
        decisions.push_back(
            EffectDecision{
                JsonString(object, "entity_id"),
                JsonString(object, "subsystem_id"),
                JsonString(object, "subsystem_type"),
                JsonString(object, "peer_entity_id"),
                ParseState(JsonString(object, "state")),
                revision,
            });
    }

    std::lock_guard<std::mutex> lock(mMutex);
    if (revision <= mLatestRevision)
    {
        return {};
    }
    for (const auto& decision : decisions)
    {
        mStates[{decision.entityId, decision.subsystemId}] = decision.state;
    }
    mLatestRevision = revision;
    return decisions;
}

EffectState
NetworkEffectGate::GetState(
    const std::string& entityId,
    const std::string& subsystemId) const
{
    std::lock_guard<std::mutex> lock(mMutex);
    const auto found = mStates.find({entityId, subsystemId});
    return found == mStates.end() ? EffectState::Blocked : found->second;
}

bool
NetworkEffectGate::CanOperate(
    const std::string& entityId,
    const std::string& subsystemId) const
{
    return GetState(entityId, subsystemId) != EffectState::Blocked;
}

std::uint64_t
NetworkEffectGate::LatestRevision() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mLatestRevision;
}

const char*
ToString(EffectState state)
{
    switch (state)
    {
    case EffectState::Available:
        return "AVAILABLE";
    case EffectState::Degraded:
        return "DEGRADED";
    case EffectState::Blocked:
        return "BLOCKED";
    }
    return "BLOCKED";
}

} // namespace afsim_ns3
