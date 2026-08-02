#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace afsim_ns3
{

struct TcpClientConfig
{
    std::string host;
    std::uint16_t port{18080};
    std::uint32_t connectTimeoutMs{5000};
    std::uint32_t reconnectDelayMs{1000};
    std::size_t maxPendingMessages{128};
};

class TcpJsonlClient
{
  public:
    using LineHandler = std::function<void(const std::string&)>;

    explicit TcpJsonlClient(TcpClientConfig config);
    ~TcpJsonlClient();

    TcpJsonlClient(const TcpJsonlClient&) = delete;
    TcpJsonlClient& operator=(const TcpJsonlClient&) = delete;

    void SetLineHandler(LineHandler handler);
    void Start();
    void Stop();

    // A new full snapshot supersedes every unsent delta.
    void QueueInitial(std::string json);

    // The current full snapshot is cached for lossless reconnect recovery.
    // Returns false if overflow forced a queued full resynchronization.
    bool QueueDelta(std::string json, std::string currentFullSnapshot);

    bool IsConnected() const;
    std::size_t PendingCount() const;

  private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace afsim_ns3
