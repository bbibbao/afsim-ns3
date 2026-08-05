#include "MessageTransportGate.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace afsim_ns3
{
namespace
{

std::size_t ValueStart(
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

std::string JsonString(
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

std::uint64_t JsonUint64(const std::string& json, const std::string& key)
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
        const auto digit = static_cast<unsigned int>(json[position++] - '0');
        if (value >
            (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
        {
            throw std::runtime_error("JSON integer exceeds uint64 range");
        }
        value = value * 10 + digit;
    }
    return value;
}

double JsonDouble(const std::string& json, const std::string& key)
{
    const auto begin = ValueStart(json, key);
    std::size_t consumed = 0;
    const double value = std::stod(json.substr(begin), &consumed);
    if (consumed == 0 || !std::isfinite(value))
    {
        throw std::runtime_error("JSON field is not a finite number: " + key);
    }
    return value;
}

bool JsonBool(const std::string& json, const std::string& key)
{
    const auto position = ValueStart(json, key);
    if (json.compare(position, 4, "true") == 0)
    {
        return true;
    }
    if (json.compare(position, 5, "false") == 0)
    {
        return false;
    }
    throw std::runtime_error("JSON field is not boolean: " + key);
}

std::size_t MatchingEnd(
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

std::vector<std::string> ObjectArray(
    const std::string& json,
    const std::string& key)
{
    auto position = ValueStart(json, key);
    if (position >= json.size() || json[position] != '[')
    {
        throw std::runtime_error(key + " must be a JSON array");
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
            throw std::runtime_error(key + " entries must be objects");
        }
        const auto objectEnd = MatchingEnd(json, position, '{', '}');
        objects.push_back(json.substr(position, objectEnd - position + 1));
        position = objectEnd + 1;
    }
    return objects;
}

std::uint64_t HashText(std::uint64_t hash, const std::string& text)
{
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (const unsigned char character : text)
    {
        hash ^= character;
        hash *= prime;
    }
    return hash;
}

double UnitDraw(
    const std::string& sourceEntityId,
    const std::string& targetEntityId,
    std::uint64_t messageSerial,
    std::uint64_t revision)
{
    std::uint64_t hash = 1469598103934665603ULL;
    hash = HashText(hash, sourceEntityId);
    hash = HashText(hash, targetEntityId);
    for (unsigned int index = 0; index < 8; ++index)
    {
        hash ^= static_cast<unsigned char>((messageSerial >> (index * 8)) & 0xffU);
        hash *= 1099511628211ULL;
        hash ^= static_cast<unsigned char>((revision >> (index * 8)) & 0xffU);
        hash *= 1099511628211ULL;
    }
    return static_cast<double>(hash >> 11) /
           static_cast<double>(std::uint64_t{1} << 53);
}

} // namespace

bool MessageTransportGate::UpdateFromMetricsJson(const std::string& jsonLine)
{
    if (JsonString(jsonLine, "message_type") != "ns3_metrics")
    {
        return false;
    }
    const auto revision = JsonUint64(jsonLine, "revision");
    std::map<std::pair<std::string, std::string>, Profile> profiles;

    for (const auto& object : ObjectArray(jsonLine, "links"))
    {
        Profile profile;
        profile.connected = JsonString(object, "state") == "UP";
        profile.delayMs = std::max(0.0, JsonDouble(object, "delay_ms"));
        profile.lossRate = std::max(
            0.0,
            std::min(1.0, JsonDouble(object, "configured_loss_rate")));
        profiles[{JsonString(object, "source_entity_id"),
                  JsonString(object, "target_entity_id")}] = profile;
    }

    for (const auto& object : ObjectArray(jsonLine, "metrics"))
    {
        const auto key = std::make_pair(
            JsonString(object, "source_entity_id"),
            JsonString(object, "target_entity_id"));
        auto& profile = profiles[key];
        profile.connected =
            JsonBool(object, "connected") &&
            JsonString(object, "link_state") == "UP";
        profile.delayMs = std::max(
            profile.delayMs,
            std::max(0.0, JsonDouble(object, "latency_ms")));
    }

    std::lock_guard<std::mutex> lock(mMutex);
    if (revision <= mLatestRevision)
    {
        return false;
    }
    mProfiles = std::move(profiles);
    mLatestRevision = revision;
    return true;
}

MessageTransportDecision MessageTransportGate::Decide(
    const std::string& sourceEntityId,
    const std::string& targetEntityId,
    std::uint64_t messageSerial) const
{
    std::lock_guard<std::mutex> lock(mMutex);
    MessageTransportDecision decision;
    decision.sourceEntityId = sourceEntityId;
    decision.targetEntityId = targetEntityId;
    decision.revision = mLatestRevision;

    const auto found = mProfiles.find({sourceEntityId, targetEntityId});
    if (found == mProfiles.end())
    {
        return decision;
    }

    decision.delayMs = found->second.delayMs;
    decision.lossRate = found->second.lossRate;
    if (!found->second.connected)
    {
        decision.reason = "LINK_DOWN";
        return decision;
    }

    if (found->second.lossRate >= 1.0 ||
        UnitDraw(
            sourceEntityId,
            targetEntityId,
            messageSerial,
            mLatestRevision) < found->second.lossRate)
    {
        decision.disposition = MessageDisposition::Drop;
        decision.reason = "PACKET_LOSS";
        return decision;
    }

    decision.disposition = MessageDisposition::Deliver;
    decision.reason = "NONE";
    return decision;
}

std::uint64_t MessageTransportGate::LatestRevision() const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mLatestRevision;
}

const char* ToString(MessageDisposition disposition)
{
    switch (disposition)
    {
    case MessageDisposition::NoLink:
        return "NO_LINK";
    case MessageDisposition::Drop:
        return "DROP";
    case MessageDisposition::Deliver:
        return "DELIVER";
    }
    return "NO_LINK";
}

} // namespace afsim_ns3
