#include "AfsimBridgeController.hpp"
#include "GeoCoordinates.hpp"

#include "UtCallbackHolder.hpp"
#include "UtMemory.hpp"
#include "WsfApplication.hpp"
#include "WsfApplicationExtension.hpp"
#include "WsfComm.hpp"
#include "WsfCommMedium.hpp"
#include "WsfCommMediumContainer.hpp"
#include "WsfCommMediumGuided.hpp"
#include "WsfCommMediumTypeIdentifier.hpp"
#include "WsfCommNetworkManager.hpp"
#include "WsfCommObserver.hpp"
#include "WsfCommPhysicalLayer.hpp"
#include "WsfCommProtocolStack.hpp"
#include "WsfCommResult.hpp"
#include "WsfEvent.hpp"
#include "WsfMessage.hpp"
#include "WsfPlatform.hpp"
#include "WsfPlugin.hpp"
#include "WsfScenario.hpp"
#include "WsfSimulation.hpp"
#include "WsfSimulationExtension.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace
{

constexpr char ExtensionName[] = "afsim_ns3_bridge";
constexpr char GateDropReasonPrefix[] = "AFSIM_NS3_GATE_DROP:";
constexpr char GateTotalDelayReasonPrefix[] =
   "AFSIM_NS3_GATE_TOTAL_DELAY_SECONDS:";
constexpr double Pi = 3.14159265358979323846;

std::string EnvironmentText(const char* name, const std::string& fallback)
{
   const char* value = std::getenv(name);
   return value != nullptr && *value != '\0' ? value : fallback;
}

bool EnvironmentHasValue(const char* name)
{
   const char* value = std::getenv(name);
   return value != nullptr && *value != '\0';
}

double EnvironmentDouble(const char* name, double fallback, double minimum)
{
   try
   {
      return std::max(minimum, std::stod(EnvironmentText(name, "")));
   }
   catch (...)
   {
      return fallback;
   }
}

std::uint32_t EnvironmentUint32(const char* name, std::uint32_t fallback, std::uint32_t minimum)
{
   try
   {
      const auto parsed = std::stoull(EnvironmentText(name, ""));
      if (parsed > UINT32_MAX)
      {
         return fallback;
      }
      return std::max(minimum, static_cast<std::uint32_t>(parsed));
   }
   catch (...)
   {
      return fallback;
   }
}

std::uint64_t EnvironmentUint64(const char* name, std::uint64_t fallback, std::uint64_t minimum)
{
   try
   {
      return std::max(minimum, std::stoull(EnvironmentText(name, "")));
   }
   catch (...)
   {
      return fallback;
   }
}

bool EnvironmentFlag(const char* name, bool fallback)
{
   std::string value = EnvironmentText(name, fallback ? "1" : "0");
   std::transform(
      value.begin(),
      value.end(),
      value.begin(),
      [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
   return value == "1" || value == "true" || value == "yes" || value == "on";
}

void AppendLog(const std::string& message)
{
   const auto path = EnvironmentText(
      "AFSIM_NS3_LOG",
      "afsim-ns3-extension.log");
   std::ofstream output(path, std::ios::app);
   if (output)
   {
      output << message << '\n';
   }
}

double Degrees(double radians)
{
   double value = radians * 180.0 / Pi;
   while (value < 0.0)
   {
      value += 360.0;
   }
   while (value >= 360.0)
   {
      value -= 360.0;
   }
   return value;
}

std::string StringIdText(WsfStringId value)
{
   std::ostringstream output;
   output << value;
   return output.str();
}

std::uint32_t Milliseconds(double seconds, std::uint32_t minimum)
{
   const double milliseconds = seconds * 1000.0;
   if (!std::isfinite(milliseconds))
   {
      return minimum;
   }
   if (milliseconds >= static_cast<double>(UINT32_MAX))
   {
      return UINT32_MAX;
   }
   return std::max(
      minimum,
      static_cast<std::uint32_t>(std::max(0.0, std::round(milliseconds))));
}

struct RuntimeConfig
{
   std::string host{EnvironmentText("AFSIM_NS3_HOST", "127.0.0.1")};
   std::uint16_t port{static_cast<std::uint16_t>(
      std::min<std::uint32_t>(65535, EnvironmentUint32("AFSIM_NS3_PORT", 18080, 1)))};
   double intervalSeconds{EnvironmentDouble("AFSIM_NS3_INTERVAL_SECONDS", 1.0, 0.05)};
   std::uint64_t defaultDataRateBps{
      EnvironmentUint64("AFSIM_NS3_DATA_RATE_BPS", 1000000, 1)};
   double defaultDelayMs{EnvironmentDouble("AFSIM_NS3_DELAY_MS", 1.0, 0.0)};
   bool delayOverride{EnvironmentHasValue("AFSIM_NS3_DELAY_MS")};
   double defaultLossRate{
      std::min(1.0, EnvironmentDouble("AFSIM_NS3_LOSS_RATE", 0.0, 0.0))};
   bool lossOverride{EnvironmentHasValue("AFSIM_NS3_LOSS_RATE")};
   std::string linkKind{EnvironmentText("AFSIM_NS3_LINK_KIND", "wireless")};
   bool traceTicks{EnvironmentFlag("AFSIM_NS3_TRACE_TICKS", false)};
};

struct PlatformRecord
{
   WsfPlatform* platform{};
   std::string entityId;
   bool alive{};
};

struct CommEndpoint
{
   std::string entityId;
   wsf::comm::Comm* comm{};
};

struct PeerEndpoint
{
   wsf::comm::Comm* comm{};
   bool ready{};
};

struct LinkObservation
{
   std::uint64_t completed{};
   std::uint64_t failures{};
   double delayTotalMs{};
   std::uint64_t delaySamples{};
   std::uint64_t lastDataRateBps{};
   double lastDelayMs{};
   double lastLossRate{};
   bool hasDataRate{};
   bool hasDelay{};
   bool hasLoss{};
};

struct ObservedFlow
{
   std::string flowId;
   std::string sourceEntityId;
   std::string targetEntityId;
   std::uint32_t packetSizeBytes{64};
   std::uint32_t intervalMs{1};
   std::uint32_t durationMs{100};
   double lastSeenTime{-1.0};
   unsigned int lastSerial{};
   bool hasSerial{};
};

using LinkKey = std::pair<const wsf::comm::Comm*, const wsf::comm::Comm*>;
using MessageKey = std::pair<const wsf::comm::Comm*, unsigned int>;
using FlowKey = std::tuple<
   std::string, std::string, std::string,
   std::string, std::string, std::string>;

class BridgeSimulationExtension final :
   public WsfSimulationExtension,
   public afsim_ns3::IAfsimEffectSink
{
public:
   BridgeSimulationExtension()
      : mController(afsim_ns3::TcpClientConfig{
           mConfig.host,
           mConfig.port,
           250,
           250,
           32}),
        mRunId(
           "afsim-" + std::to_string(
              std::chrono::high_resolution_clock::now().time_since_epoch().count()))
   {
      mController.SetEffectErrorHandler(
         [](const afsim_ns3::EffectDecision& decision, const std::string& error)
         {
            AppendLog(
               "EFFECT_ERROR entity=" + decision.entityId +
               " subsystem=" + decision.subsystemId +
               " state=" + afsim_ns3::ToString(decision.state) +
               " error=" + error);
         });
   }

   void Start() override
   {
      mStopping = false;
      auto& simulation = GetSimulation();
      mCallbacks.Add(
         WsfObserver::MessageTransmitted(&simulation).Connect(
            &BridgeSimulationExtension::MessageTransmitted, this));
      mCallbacks.Add(WsfObserver::MessageDeliveryAttempt(&simulation).Connect(
         &BridgeSimulationExtension::MessageDeliveryAttempt, this));
      mCallbacks.Add(WsfObserver::MessageReceived(&simulation).Connect(
         &BridgeSimulationExtension::MessageReceived, this));
      mCallbacks.Add(WsfObserver::MessageDiscarded(&simulation).Connect(
         &BridgeSimulationExtension::MessageDiscarded, this));
      mController.Start();
      std::ostringstream message;
      message << "START host=" << mConfig.host << " port=" << mConfig.port;
      AppendLog(message.str());

      const double firstTime = GetSimulation().GetSimTime() + 1.0E-9;
      GetSimulation().AddEvent(
         ut::make_unique<WsfRecurringEvent>(
            firstTime,
            [this](WsfEvent& event)
            {
               if (mStopping)
               {
                  return WsfEvent::cDELETE;
               }
               try
               {
                  Tick(event.GetTime());
               }
               catch (const std::exception& error)
               {
                  AppendLog(std::string("ERROR tick=") + error.what());
               }
               catch (...)
               {
                  AppendLog("ERROR tick=unknown");
               }
               if (mStopping)
               {
                  return WsfEvent::cDELETE;
               }
               event.SetTime(event.GetTime() + mConfig.intervalSeconds);
               return WsfEvent::cRESCHEDULE;
            }));
   }

   void Complete(double aSimTime) override
   {
      mStopping = true;
      mCallbacks.Clear();
      mController.Stop();
      std::ostringstream message;
      message << "COMPLETE sim_time=" << std::fixed << std::setprecision(3) << aSimTime
              << " ticks=" << mTick;
      AppendLog(message.str());
   }

   void ApplyNetworkEffect(const afsim_ns3::EffectDecision& decision) override
   {
      AppendLog(
         "EFFECT_IGNORED entity=" + decision.entityId +
         " subsystem=" + decision.subsystemId +
         " state=" + afsim_ns3::ToString(decision.state));
   }

private:
   void MessageTransmitted(
      double simTime,
      wsf::comm::Comm* transmitter,
      const WsfMessage& message)
   {
      if (transmitter == nullptr)
      {
         return;
      }

      mTransmitTimes[{transmitter, message.GetSerialNumber()}] = simTime;
      auto* manager = GetSimulation().GetCommNetworkManager();
      auto* receiver = manager != nullptr ? manager->GetComm(message.GetDstAddr()) : nullptr;
      RecordFlow(simTime, transmitter, receiver, message);
      std::ostringstream trace;
      trace << "MESSAGE_TX serial=" << message.GetSerialNumber()
            << " sim_time=" << std::fixed << std::setprecision(6) << simTime;
      AppendLog(trace.str());
   }

   void MessageDeliveryAttempt(
      double simTime,
      wsf::comm::Comm* transmitter,
      wsf::comm::Comm* receiver,
      const WsfMessage& message,
      wsf::comm::Result& result)
   {
      if (transmitter == nullptr || receiver == nullptr)
      {
         return;
      }

      const MessageKey messageKey{
         transmitter,
         message.GetSerialNumber()};
      mExternallyGatedMessages.insert(messageKey);
      RecordFlow(simTime, transmitter, receiver, message);
      auto& observation = mLinkObservations[{transmitter, receiver}];
      UpdateObservedDataRate(observation, result);

      const auto* sourcePlatform = transmitter->GetPlatform();
      const auto* targetPlatform = receiver->GetPlatform();
      if (sourcePlatform == nullptr || targetPlatform == nullptr)
      {
         mExternallyDroppedMessages.insert(messageKey);
         result.mCheckedStatus |= WsfEM_Interaction::cSIGNAL_LEVEL;
         result.mFailedStatus |= WsfEM_Interaction::cSIGNAL_LEVEL;
         result.setDisconnectReason(
            wsf::comm::LinkDisconnectReasonType::UNKNOWN_ERROR,
            std::string(GateDropReasonPrefix) + "PLATFORM_NOT_FOUND");
         AppendLog(
            "MESSAGE_GATE serial=" + std::to_string(message.GetSerialNumber()) +
            " decision=NO_LINK reason=PLATFORM_NOT_FOUND");
         return;
      }

      const std::string sourceEntityId =
         std::to_string(sourcePlatform->GetIndex());
      const std::string targetEntityId =
         std::to_string(targetPlatform->GetIndex());
      const auto decision = mController.DecideMessage(
         sourceEntityId,
         targetEntityId,
         message.GetSerialNumber());

      const bool externalDrop =
         decision.disposition != afsim_ns3::MessageDisposition::Deliver;
      const double totalDelaySeconds = externalDrop
         ? 0.0
         : std::max(0.0, decision.delayMs) / 1000.0;
      result.mCheckedStatus |= WsfEM_Interaction::cSIGNAL_LEVEL;
      if (externalDrop)
      {
         mExternallyDroppedMessages.insert(messageKey);
         result.mFailedStatus |= WsfEM_Interaction::cSIGNAL_LEVEL;
         result.setDisconnectReason(
            decision.disposition == afsim_ns3::MessageDisposition::NoLink
               ? wsf::comm::LinkDisconnectReasonType::NO_ROUTER_BRIDGE
               : wsf::comm::LinkDisconnectReasonType::SIGNAL_LEVEL_INSUFFICIENT,
            std::string(GateDropReasonPrefix) + decision.reason);
      }
      else
      {
         mExternallyDroppedMessages.erase(messageKey);
         result.setDisconnectReason(
            wsf::comm::LinkDisconnectReasonType::LINK_CONNECTED,
            std::string(GateTotalDelayReasonPrefix) +
               std::to_string(totalDelaySeconds));
      }

      std::ostringstream gateLog;
      gateLog << "MESSAGE_GATE serial=" << message.GetSerialNumber()
              << " source=" << sourceEntityId
              << " target=" << targetEntityId
              << " revision=" << decision.revision
              << " decision=" << afsim_ns3::ToString(decision.disposition)
              << " reason=" << decision.reason
              << " delay_ms=" << std::fixed << std::setprecision(3) << decision.delayMs
              << " loss_rate=" << decision.lossRate;
      AppendLog(gateLog.str());
   }

   void MessageReceived(
      double simTime,
      wsf::comm::Comm* transmitter,
      wsf::comm::Comm* receiver,
      const WsfMessage& message,
      wsf::comm::Result& result)
   {
      if (transmitter == nullptr || receiver == nullptr)
      {
         return;
      }

      RecordFlow(simTime, transmitter, receiver, message);
      auto& observation = mLinkObservations[{transmitter, receiver}];
      const MessageKey messageKey{
         transmitter,
         message.GetSerialNumber()};
      const bool externallyGated =
         mExternallyGatedMessages.erase(messageKey) > 0;
      ++observation.completed;
      if (result.mFailedStatus != 0)
      {
         ++observation.failures;
      }
      if (!externallyGated)
      {
         UpdateObservedDelayAndRate(
            simTime, transmitter, message, observation, true);
      }
      else
      {
         mTransmitTimes.erase(messageKey);
      }
      UpdateObservedDataRate(observation, result);
      std::ostringstream trace;
      trace << "MESSAGE_RX serial=" << message.GetSerialNumber()
            << " sim_time=" << std::fixed << std::setprecision(6) << simTime;
      AppendLog(trace.str());
   }

   void MessageDiscarded(
      double simTime,
      wsf::comm::Comm* transmitter,
      const WsfMessage& message,
      const std::string&)
   {
      auto* manager = GetSimulation().GetCommNetworkManager();
      auto* receiver = manager != nullptr ? manager->GetComm(message.GetDstAddr()) : nullptr;
      if (transmitter == nullptr || receiver == nullptr)
      {
         return;
      }

      RecordFlow(simTime, transmitter, receiver, message);
      auto& observation = mLinkObservations[{transmitter, receiver}];
      const MessageKey messageKey{
         transmitter,
         message.GetSerialNumber()};
      const bool externallyDropped =
         mExternallyDroppedMessages.erase(messageKey) > 0;
      const bool externallyGated =
         mExternallyGatedMessages.erase(messageKey) > 0;
      if (!externallyDropped)
      {
         ++observation.completed;
         ++observation.failures;
         if (!externallyGated)
         {
            UpdateObservedDelayAndRate(
               simTime, transmitter, message, observation, false);
         }
         else
         {
            mTransmitTimes.erase(messageKey);
         }
      }
      std::ostringstream trace;
      trace << "MESSAGE_DISCARD serial=" << message.GetSerialNumber()
            << " sim_time=" << std::fixed << std::setprecision(6) << simTime
            << " external=" << (externallyDropped ? 1 : 0);
      AppendLog(trace.str());
   }

   static void UpdateObservedDataRate(
      LinkObservation& observation,
      const wsf::comm::Result& result)
   {
      if (std::isfinite(result.mDataRate) && result.mDataRate > 0.0)
      {
         observation.lastDataRateBps = static_cast<std::uint64_t>(
            std::min(result.mDataRate, static_cast<double>(UINT64_MAX)));
         observation.hasDataRate = true;
      }

   }

   void UpdateObservedDelayAndRate(
      double simTime,
      wsf::comm::Comm* transmitter,
      const WsfMessage& message,
      LinkObservation& observation,
      bool inferRate)
   {
      const auto transmitted =
         mTransmitTimes.find({transmitter, message.GetSerialNumber()});
      if (transmitted != mTransmitTimes.end())
      {
         const double delaySeconds = std::max(
            0.0,
            simTime - transmitted->second);
         const double delayMs = delaySeconds * 1000.0;
         if (std::isfinite(delayMs))
         {
            observation.delayTotalMs += delayMs;
            ++observation.delaySamples;
         }
         if (inferRate && delaySeconds > 0.0 && message.GetSizeBits() > 0)
         {
            const double effectiveRate =
               static_cast<double>(message.GetSizeBits()) / delaySeconds;
            if (std::isfinite(effectiveRate) && effectiveRate > 0.0)
            {
               observation.lastDataRateBps = static_cast<std::uint64_t>(
                  std::min(effectiveRate, static_cast<double>(UINT64_MAX)));
               observation.hasDataRate = true;
            }
         }
         mTransmitTimes.erase(transmitted);
      }
   }

   void RecordFlow(
      double simTime,
      wsf::comm::Comm* transmitter,
      wsf::comm::Comm* receiver,
      const WsfMessage& message)
   {
      if (transmitter == nullptr || receiver == nullptr ||
          transmitter->GetPlatform() == nullptr || receiver->GetPlatform() == nullptr)
      {
         return;
      }

      const std::string sourceEntityId =
         std::to_string(transmitter->GetPlatform()->GetIndex());
      const std::string targetEntityId =
         std::to_string(receiver->GetPlatform()->GetIndex());
      if (sourceEntityId == targetEntityId)
      {
         return;
      }

      const std::string messageType = StringIdText(message.GetType());
      const std::string messageSubType = StringIdText(message.GetSubType());
      const FlowKey key{
         sourceEntityId,
         transmitter->GetName(),
         targetEntityId,
         receiver->GetName(),
         messageType,
         messageSubType};
      auto& flow = mObservedFlows[key];
      if (flow.flowId.empty())
      {
         flow.flowId =
            "afsim-message:" + sourceEntityId + ':' + transmitter->GetName() + "->" +
            targetEntityId + ':' + receiver->GetName() + ':' + messageType + ':' + messageSubType;
         flow.sourceEntityId = sourceEntityId;
         flow.targetEntityId = targetEntityId;
         flow.intervalMs = Milliseconds(mConfig.intervalSeconds, 1);
         flow.durationMs = Milliseconds(mConfig.intervalSeconds, 100);
      }

      const unsigned int serial = message.GetSerialNumber();
      if (!flow.hasSerial || flow.lastSerial != serial)
      {
         if (flow.lastSeenTime >= 0.0 && simTime > flow.lastSeenTime)
         {
            flow.intervalMs = Milliseconds(simTime - flow.lastSeenTime, 1);
         }
         flow.lastSeenTime = simTime;
         flow.lastSerial = serial;
         flow.hasSerial = true;
      }
      flow.packetSizeBytes = static_cast<std::uint32_t>(
         std::max(64, message.GetSizeBytes()));
   }

   wsf::comm::medium::ModeGuided* GuidedMode(
      const wsf::comm::Comm* comm) const
   {
      if (comm == nullptr)
      {
         return nullptr;
      }
      auto* container =
         wsf::comm::medium::ContainerComponent<wsf::comm::Comm>::Find(*comm);
      auto* medium = container != nullptr ? container->GetMedium() : nullptr;
      if (medium == nullptr ||
          medium->GetMediumIdentifier() != wsf::comm::medium::cGUIDED)
      {
         return nullptr;
      }
      return dynamic_cast<wsf::comm::medium::ModeGuided*>(
         medium->GetMode(medium->GetCurrentModeIndex()));
   }

   std::uint64_t ConfiguredDataRate(wsf::comm::Comm* comm)
   {
      const auto found = mConfiguredDataRates.find(comm);
      if (found != mConfiguredDataRates.end())
      {
         return found->second;
      }

      std::uint64_t configured = mConfig.defaultDataRateBps;
      if (comm != nullptr)
      {
         auto* physical =
            comm->GetProtocolStack().GetLayer<wsf::comm::PhysicalLayer>();
         if (physical != nullptr)
         {
            // AFSIM exposes the configured transfer-rate variable through a
            // draw. Sample it only once per device and cache the result.
            const double rate = physical->GetTransferRate();
            if (std::isfinite(rate) && rate > 0.0)
            {
               configured = static_cast<std::uint64_t>(
                  std::min(rate, static_cast<double>(UINT64_MAX)));
            }
         }
      }
      configured = std::max<std::uint64_t>(1, configured);
      mConfiguredDataRates[comm] = configured;
      return configured;
   }

   double NativeGuidedDelayMs(
      const wsf::comm::Comm* transmitter,
      const wsf::comm::Comm* receiver) const
   {
      auto* mode = GuidedMode(transmitter);
      if (mode == nullptr)
      {
         return -1.0;
      }

      double delaySeconds = std::max(0.0, mode->GetDelayTime().LastDraw());
      const double propagationSpeed = mode->GetPropagationSpeed().LastDraw();
      if (propagationSpeed > 0.0 && transmitter != nullptr && receiver != nullptr &&
          transmitter->GetPlatform() != nullptr && receiver->GetPlatform() != nullptr)
      {
         double offsetWcs[3]{};
         transmitter->GetPlatform()->GetRelativeLocationWCS(
            receiver->GetPlatform(), offsetWcs);
         const double distance = std::sqrt(
            offsetWcs[0] * offsetWcs[0] +
            offsetWcs[1] * offsetWcs[1] +
            offsetWcs[2] * offsetWcs[2]);
         delaySeconds += distance / propagationSpeed;
      }
      return std::max(0.0, delaySeconds * 1000.0);
   }

   double ConfiguredDelayMs(
      const wsf::comm::Comm* transmitter,
      const wsf::comm::Comm* receiver) const
   {
      if (mConfig.delayOverride)
      {
         return mConfig.defaultDelayMs;
      }
      const double nativeDelayMs =
         NativeGuidedDelayMs(transmitter, receiver);
      return nativeDelayMs >= 0.0
                ? nativeDelayMs
                : mConfig.defaultDelayMs;
   }

   double ConfiguredLossRate(const wsf::comm::Comm*) const
   {
      // AFSIM 2.9 does not expose a configured random packet-loss probability
      // for every medium.  Use the explicit fallback until real receive/discard
      // observations are available for this peer.
      return mConfig.defaultLossRate;
   }

   std::string LinkKind(const wsf::comm::Comm* comm) const
   {
      if (comm != nullptr)
      {
         auto* container = wsf::comm::medium::ContainerComponent<wsf::comm::Comm>::Find(*comm);
         auto* medium = container != nullptr ? container->GetMedium() : nullptr;
         if (medium != nullptr)
         {
            const auto identifier = medium->GetMediumIdentifier();
            if (identifier == wsf::comm::medium::cGUIDED)
            {
               return "wired";
            }
            if (identifier == wsf::comm::medium::cUNGUIDED)
            {
               return "wireless";
            }
         }
      }
      return mConfig.linkKind == "wired" ? "wired" : "wireless";
   }

   void Tick(double simTime)
   {
      const std::size_t applied = mController.ApplyPendingEffects(*this);
      std::vector<afsim_ns3::EntityState> entities;
      std::vector<afsim_ns3::FlowConfig> flows;
      CollectState(simTime, entities, flows);

      std::ostringstream requestId;
      requestId << mRunId << '-' << ++mTick;
      const auto result = mController.SubmitCurrentState(
         requestId.str(),
         static_cast<std::uint64_t>(std::max(0.0, simTime) * 1000.0),
         entities,
         flows);

      std::size_t policies = 0;
      for (const auto& entity : entities)
      {
         policies += entity.effectPolicies.size();
      }

      if (mConfig.traceTicks || mTick <= 2 || applied > 0)
      {
         std::ostringstream message;
         message << "TICK sim_time=" << std::fixed << std::setprecision(3) << simTime
                 << " entities=" << entities.size() << " flows=" << flows.size()
                 << " policies=" << policies
                 << " applied=" << applied << " connected=" << (mController.IsConnected() ? 1 : 0)
                 << " submit=" << static_cast<int>(result);
         AppendLog(message.str());
      }
   }

   void CollectState(
      double simTime,
      std::vector<afsim_ns3::EntityState>& entities,
      std::vector<afsim_ns3::FlowConfig>& flows)
   {
      std::vector<PlatformRecord> platforms;
      std::map<std::string, std::string> nameToEntity;
      std::map<std::string, bool> entityAlive;
      std::set<std::pair<std::string, std::string>> upLinks;

      for (std::size_t index = 0; index < GetSimulation().GetPlatformCount(); ++index)
      {
         WsfPlatform* platform = GetSimulation().GetPlatformEntry(index);
         if (platform == nullptr)
         {
            continue;
         }
         const std::string entityId = std::to_string(platform->GetIndex());
         const bool alive = !platform->IsBroken() && !platform->IsDeleted();
         platforms.push_back(PlatformRecord{platform, entityId, alive});
         nameToEntity[platform->GetName()] = entityId;
         entityAlive[entityId] = alive;
      }

      std::map<std::pair<std::string, std::string>, wsf::comm::Comm*> commByAddress;
      std::map<std::string, std::vector<CommEndpoint>> networkPeers;
      for (const auto& record : platforms)
      {
         for (unsigned int index = 0;
              index < record.platform->GetComponentCount<wsf::comm::Comm>();
              ++index)
         {
            auto* comm = record.platform->GetComponentEntry<wsf::comm::Comm>(index);
            if (comm == nullptr)
            {
               continue;
            }
            commByAddress[{record.platform->GetName(), comm->GetName()}] = comm;
            if (!comm->GetNetwork().empty())
            {
               networkPeers[comm->GetNetwork()].push_back(CommEndpoint{record.entityId, comm});
            }
         }
      }

      for (const auto& record : platforms)
      {
         afsim_ns3::EntityState entity;
         entity.entityId = record.entityId;
         entity.businessNodeId = record.platform->GetName();
         entity.alive = record.alive;

         double latitude = 0.0;
         double longitude = 0.0;
         double altitude = 0.0;
         record.platform->GetLocationLLA(latitude, longitude, altitude);
         // AFSIM 2.9 returns latitude/longitude in degrees; LocalEnuFrame uses degrees too.
         afsim_ns3::GeoPoint point{
            latitude,
            longitude,
            altitude};
         if (!mEnuFrame)
         {
            mEnuFrame = std::make_unique<afsim_ns3::LocalEnuFrame>(point);
         }
         entity.position = mEnuFrame->ToEnu(point);

         double velocityNed[3]{};
         record.platform->GetVelocityNED(velocityNed);
         entity.velocity = afsim_ns3::Velocity{
            velocityNed[1],
            velocityNed[0],
            -velocityNed[2]};

         double heading = 0.0;
         double pitch = 0.0;
         double roll = 0.0;
         record.platform->GetOrientationNED(heading, pitch, roll);
         entity.headingDeg = Degrees(heading);

         for (unsigned int index = 0;
              index < record.platform->GetComponentCount<wsf::comm::Comm>();
              ++index)
         {
            auto* comm = record.platform->GetComponentEntry<wsf::comm::Comm>(index);
            if (comm == nullptr)
            {
               continue;
            }

            std::map<std::string, PeerEndpoint> peers;
            for (const auto& link : comm->GetLinkPairs())
            {
               const auto found = nameToEntity.find(link.first);
               if (found != nameToEntity.end() && found->second != record.entityId)
               {
                  const auto remote = commByAddress.find({link.first, link.second});
                  const bool remoteReady =
                     remote != commByAddress.end() && remote->second != nullptr &&
                     remote->second->IsTurnedOn() && remote->second->CanReceive();
                  auto& endpoint = peers[found->second];
                  if (endpoint.comm == nullptr && remote != commByAddress.end())
                  {
                     endpoint.comm = remote->second;
                  }
                  endpoint.ready = endpoint.ready || remoteReady;
               }
            }
            if (!comm->GetNetwork().empty())
            {
               const auto found = networkPeers.find(comm->GetNetwork());
               if (found != networkPeers.end())
               {
                  for (const auto& peer : found->second)
                  {
                     if (peer.entityId != record.entityId)
                     {
                        const bool remoteReady =
                           peer.comm != nullptr && peer.comm->IsTurnedOn() &&
                           peer.comm->CanReceive();
                        auto& endpoint = peers[peer.entityId];
                        if (endpoint.comm == nullptr)
                        {
                           endpoint.comm = peer.comm;
                        }
                        endpoint.ready = endpoint.ready || remoteReady;
                     }
                  }
               }
            }

            const auto& stats = comm->GetStats();
            const std::uint64_t configuredDataRate = ConfiguredDataRate(comm);
            const std::uint64_t aggregateDataRate =
               std::isfinite(stats.mLastDataRate) && stats.mLastDataRate > 0.0
                  ? static_cast<std::uint64_t>(
                       std::min(stats.mLastDataRate, static_cast<double>(UINT64_MAX)))
                  : configuredDataRate;
            for (const auto& peer : peers)
            {
               const auto& peerEntityId = peer.first;
               auto* observation = peer.second.comm != nullptr
                                      ? &mLinkObservations[{comm, peer.second.comm}]
                                      : nullptr;
               std::uint64_t dataRate = aggregateDataRate;
               double delayMs = ConfiguredDelayMs(comm, peer.second.comm);
               double lossRate = ConfiguredLossRate(comm);
               if (observation != nullptr)
               {
                  if (observation->completed > 0)
                  {
                     observation->lastLossRate = std::min(
                        1.0,
                        static_cast<double>(observation->failures) /
                           static_cast<double>(observation->completed));
                     observation->hasLoss = true;
                     observation->completed = 0;
                     observation->failures = 0;
                  }
                  if (observation->delaySamples > 0)
                  {
                     observation->lastDelayMs =
                        observation->delayTotalMs /
                        static_cast<double>(observation->delaySamples);
                     observation->hasDelay = true;
                     observation->delayTotalMs = 0.0;
                     observation->delaySamples = 0;
                  }
                  if (observation->hasDataRate)
                  {
                     dataRate = observation->lastDataRateBps;
                  }
                  if (observation->hasDelay && !mConfig.delayOverride)
                  {
                     delayMs = observation->lastDelayMs;
                  }
                  if (observation->hasLoss && !mConfig.lossOverride)
                  {
                     lossRate = observation->lastLossRate;
                  }
               }

               afsim_ns3::DeviceConfig device;
               device.deviceId = comm->GetName() + "-to-" + peerEntityId;
               device.peerEntityId = peerEntityId;
               device.kind = LinkKind(comm);
               device.dataRateBps = std::max<std::uint64_t>(1, dataRate);
               device.delayMs = delayMs;
               device.lossRate = lossRate;
               device.linkState = record.alive && entityAlive[peerEntityId] &&
                                        comm->IsTurnedOn() && comm->CanSend() && peer.second.ready
                                     ? "UP"
                                     : "DOWN";
               if (device.linkState == "UP")
               {
                  upLinks.insert({record.entityId, peerEntityId});
               }
               entity.devices.push_back(std::move(device));
            }
         }

         entities.push_back(std::move(entity));
      }

      for (const auto& entry : mObservedFlows)
      {
         const auto& observed = entry.second;
         const auto source = entityAlive.find(observed.sourceEntityId);
         const auto target = entityAlive.find(observed.targetEntityId);

         afsim_ns3::FlowConfig flow;
         flow.flowId = observed.flowId;
         flow.sourceEntityId = observed.sourceEntityId;
         flow.targetEntityId = observed.targetEntityId;
         flow.packetSizeBytes = observed.packetSizeBytes;
         flow.intervalMs = observed.intervalMs;
         flow.durationMs = observed.durationMs;
         // Once AFSIM has emitted this message stream, keep it as a real
         // business-flow configuration. Fast delta coalescing must not erase
         // it merely because no packet was emitted in the current sample.
         flow.active = source != entityAlive.end() && source->second &&
                       target != entityAlive.end() && target->second &&
                       upLinks.count({observed.sourceEntityId, observed.targetEntityId}) != 0;
         flows.push_back(std::move(flow));
      }

      const double transmitCutoff =
         simTime - std::max(5.0, mConfig.intervalSeconds * 5.0);
      for (auto iterator = mTransmitTimes.begin(); iterator != mTransmitTimes.end();)
      {
         if (iterator->second < transmitCutoff)
         {
            iterator = mTransmitTimes.erase(iterator);
         }
         else
         {
            ++iterator;
         }
      }
   }

   RuntimeConfig mConfig;
   afsim_ns3::AfsimBridgeController mController;
   std::unique_ptr<afsim_ns3::LocalEnuFrame> mEnuFrame;
   UtCallbackHolder mCallbacks;
   std::map<const wsf::comm::Comm*, std::uint64_t> mConfiguredDataRates;
   std::map<LinkKey, LinkObservation> mLinkObservations;
   std::map<MessageKey, double> mTransmitTimes;
   std::set<MessageKey> mExternallyGatedMessages;
   std::set<MessageKey> mExternallyDroppedMessages;
   std::map<FlowKey, ObservedFlow> mObservedFlows;
   std::string mRunId;
   std::uint64_t mTick{};
   bool mStopping{};
};

class ScenarioExtension final : public WsfScenarioExtension
{
public:
   void SimulationCreated(WsfSimulation& simulation) override
   {
      simulation.RegisterExtension(
         GetExtensionName(),
         ut::make_unique<BridgeSimulationExtension>());
   }
};

} // namespace

void UT_PLUGIN_EXPORT Register_wsf_afsim_ns3(WsfApplication& application)
{
   if (!application.ExtensionIsRegistered(ExtensionName))
   {
      application.RegisterFeature("afsim_ns3", ExtensionName);
      application.RegisterExtension(
         ExtensionName,
         ut::make_unique<WsfDefaultApplicationExtension<ScenarioExtension>>());
   }
}

extern "C"
{
UT_PLUGIN_EXPORT void WsfPluginVersion(UtPluginVersion& version)
{
   version = UtPluginVersion(
      WSF_PLUGIN_API_MAJOR_VERSION,
      WSF_PLUGIN_API_MINOR_VERSION,
      WSF_PLUGIN_API_COMPILER_STRING);
}

UT_PLUGIN_EXPORT void WsfPluginSetup(WsfApplication& application)
{
   AppendLog("SETUP");
   Register_wsf_afsim_ns3(application);
}
}
