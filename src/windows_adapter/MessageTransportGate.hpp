#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace afsim_ns3
{

enum class MessageDisposition
{
    NoLink,
    Drop,
    Deliver
};

struct MessageTransportDecision
{
    std::string sourceEntityId;
    std::string targetEntityId;
    MessageDisposition disposition{MessageDisposition::NoLink};
    double delayMs{};
    double lossRate{1.0};
    std::uint64_t revision{};
    std::string reason{"NO_NETWORK_PROFILE"};
};

// Keeps the latest ns-3 result and makes a fast deterministic decision for
// each AFSIM message. It never performs socket or ns-3 work on the simulation
// thread.
class MessageTransportGate
{
  public:
    bool UpdateFromMetricsJson(const std::string& jsonLine);

    MessageTransportDecision Decide(
        const std::string& sourceEntityId,
        const std::string& targetEntityId,
        std::uint64_t messageSerial) const;

    std::uint64_t LatestRevision() const;

  private:
    struct Profile
    {
        bool connected{};
        double delayMs{};
        double lossRate{1.0};
    };

    mutable std::mutex mMutex;
    std::map<std::pair<std::string, std::string>, Profile> mProfiles;
    std::uint64_t mLatestRevision{};
};

const char* ToString(MessageDisposition disposition);

} // namespace afsim_ns3
