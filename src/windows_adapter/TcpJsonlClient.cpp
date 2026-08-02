#include "TcpJsonlClient.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
using SocketHandle = SOCKET;
constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle InvalidSocket = -1;
#endif

namespace afsim_ns3
{
namespace
{

void
CloseSocket(SocketHandle socket)
{
    if (socket == InvalidSocket)
    {
        return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

bool
SetNonBlocking(SocketHandle socket)
{
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool
WouldBlock()
{
#ifdef _WIN32
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN ||
           errno == EINPROGRESS;
#endif
}

bool
WaitSocket(
    SocketHandle socket,
    bool writable,
    std::uint32_t timeoutMs)
{
    fd_set descriptors;
    FD_ZERO(&descriptors);
    FD_SET(socket, &descriptors);
    timeval timeout{};
    timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(timeoutMs / 1000);
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(
        (timeoutMs % 1000) * 1000);
    const int result =
        select(
#ifdef _WIN32
            0,
#else
            socket + 1,
#endif
            writable ? nullptr : &descriptors,
            writable ? &descriptors : nullptr,
            nullptr,
            &timeout);
    return result > 0;
}

bool
ConnectionSucceeded(SocketHandle socket)
{
    int socketError = 0;
#ifdef _WIN32
    int length = sizeof(socketError);
#else
    socklen_t length = sizeof(socketError);
#endif
    return getsockopt(
               socket,
               SOL_SOCKET,
               SO_ERROR,
               reinterpret_cast<char*>(&socketError),
               &length) == 0 &&
           socketError == 0;
}

SocketHandle
Connect(const TcpClientConfig& config)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const std::string port = std::to_string(config.port);
    if (getaddrinfo(
            config.host.c_str(),
            port.c_str(),
            &hints,
            &addresses) != 0)
    {
        return InvalidSocket;
    }

    SocketHandle connected = InvalidSocket;
    for (auto* address = addresses;
         address != nullptr && connected == InvalidSocket;
         address = address->ai_next)
    {
        const auto socketHandle = socket(
            address->ai_family,
            address->ai_socktype,
            address->ai_protocol);
        if (socketHandle == InvalidSocket || !SetNonBlocking(socketHandle))
        {
            CloseSocket(socketHandle);
            continue;
        }

        const int result = connect(
            socketHandle,
            address->ai_addr,
            static_cast<int>(address->ai_addrlen));
        if (result == 0 ||
            (WouldBlock() &&
             WaitSocket(
                 socketHandle,
                 true,
                 config.connectTimeoutMs) &&
             ConnectionSucceeded(socketHandle)))
        {
            connected = socketHandle;
        }
        else
        {
            CloseSocket(socketHandle);
        }
    }
    freeaddrinfo(addresses);
    return connected;
}

bool
SendAll(SocketHandle socket, const std::string& message)
{
    std::size_t sent = 0;
    while (sent < message.size())
    {
        if (!WaitSocket(socket, true, 1000))
        {
            return false;
        }
        const auto remaining = message.size() - sent;
        const int written = send(
            socket,
            message.data() + sent,
            static_cast<int>(remaining),
            0);
        if (written <= 0)
        {
            if (WouldBlock())
            {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

} // namespace

class TcpJsonlClient::Impl
{
  public:
    explicit Impl(TcpClientConfig config)
        : mConfig(std::move(config))
    {
        if (mConfig.host.empty() || mConfig.port == 0 ||
            mConfig.maxPendingMessages == 0)
        {
            throw std::invalid_argument("invalid TCP client configuration");
        }
    }

    ~Impl()
    {
        Stop();
    }

    void SetLineHandler(LineHandler handler)
    {
        std::lock_guard<std::mutex> lock(mHandlerMutex);
        mHandler = std::move(handler);
    }

    void Start()
    {
        bool expected = false;
        if (!mRunning.compare_exchange_strong(expected, true))
        {
            return;
        }
        mThread = std::thread([this]() { Run(); });
    }

    void Stop()
    {
        if (!mRunning.exchange(false))
        {
            return;
        }
        mQueueChanged.notify_all();
        if (mThread.joinable())
        {
            mThread.join();
        }
        mConnected = false;
    }

    void QueueInitial(std::string json)
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        mResync = Normalize(std::move(json));
        mPending.clear();
        mPending.push_back(mResync);
        mQueueChanged.notify_all();
    }

    bool QueueDelta(std::string json, std::string currentFullSnapshot)
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        mResync = Normalize(std::move(currentFullSnapshot));
        if (mPending.size() >= mConfig.maxPendingMessages)
        {
            mPending.clear();
            mPending.push_back(mResync);
            mQueueChanged.notify_all();
            return false;
        }
        mPending.push_back(Normalize(std::move(json)));
        mQueueChanged.notify_all();
        return true;
    }

    std::size_t PendingCount() const
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        return mPending.size();
    }

    bool IsConnected() const
    {
        return mConnected.load();
    }

    void Run()
    {
#ifdef _WIN32
        WSADATA winsock{};
        if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0)
        {
            mRunning = false;
            return;
        }
#endif
        while (mRunning)
        {
            const auto socket = Connect(mConfig);
            if (socket == InvalidSocket)
            {
                WaitForReconnect();
                continue;
            }
            mConnected = true;
            ProcessConnection(socket);
            mConnected = false;
            CloseSocket(socket);
            if (mRunning)
            {
                WaitForReconnect();
            }
        }
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void ProcessConnection(SocketHandle socket)
    {
        std::string current;
        {
            std::lock_guard<std::mutex> lock(mQueueMutex);
            current = mResync;
            mPending.clear();
        }
        std::string received;
        received.reserve(8192);
        while (mRunning)
        {
            if (current.empty())
            {
                std::lock_guard<std::mutex> lock(mQueueMutex);
                if (!mPending.empty())
                {
                    current = std::move(mPending.front());
                    mPending.pop_front();
                }
            }
            if (!current.empty())
            {
                if (!SendAll(socket, current))
                {
                    std::lock_guard<std::mutex> lock(mQueueMutex);
                    mPending.push_front(std::move(current));
                    return;
                }
                current.clear();
            }

            if (WaitSocket(socket, false, 50))
            {
                char buffer[8192];
                const int count = recv(socket, buffer, sizeof(buffer), 0);
                if (count == 0)
                {
                    return;
                }
                if (count < 0)
                {
                    if (WouldBlock())
                    {
                        continue;
                    }
                    return;
                }
                received.append(buffer, static_cast<std::size_t>(count));
                if (received.size() > 10 * 1024 * 1024)
                {
                    return;
                }
                DispatchLines(received);
            }
        }
    }

    void DispatchLines(std::string& buffer)
    {
        std::size_t newline = 0;
        while ((newline = buffer.find('\n')) != std::string::npos)
        {
            std::string line = buffer.substr(0, newline);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            buffer.erase(0, newline + 1);
            LineHandler handler;
            {
                std::lock_guard<std::mutex> lock(mHandlerMutex);
                handler = mHandler;
            }
            if (handler && !line.empty())
            {
                try
                {
                    handler(line);
                }
                catch (...)
                {
                    // A consumer exception must not stop the network worker.
                }
            }
        }
    }

    void WaitForReconnect()
    {
        std::unique_lock<std::mutex> lock(mQueueMutex);
        mQueueChanged.wait_for(
            lock,
            std::chrono::milliseconds(mConfig.reconnectDelayMs),
            [this]() { return !mRunning.load(); });
    }

    static std::string Normalize(std::string json)
    {
        while (!json.empty() &&
               (json.back() == '\r' || json.back() == '\n'))
        {
            json.pop_back();
        }
        json.push_back('\n');
        return json;
    }

    TcpClientConfig mConfig;
    std::atomic<bool> mRunning{false};
    std::atomic<bool> mConnected{false};
    std::thread mThread;
    mutable std::mutex mQueueMutex;
    std::condition_variable mQueueChanged;
    std::deque<std::string> mPending;
    std::string mResync;
    mutable std::mutex mHandlerMutex;
    LineHandler mHandler;
};

TcpJsonlClient::TcpJsonlClient(TcpClientConfig config)
    : mImpl(new Impl(std::move(config)))
{
}

TcpJsonlClient::~TcpJsonlClient() = default;

void
TcpJsonlClient::SetLineHandler(LineHandler handler)
{
    mImpl->SetLineHandler(std::move(handler));
}

void
TcpJsonlClient::Start()
{
    mImpl->Start();
}

void
TcpJsonlClient::Stop()
{
    mImpl->Stop();
}

void
TcpJsonlClient::QueueInitial(std::string json)
{
    mImpl->QueueInitial(std::move(json));
}

bool
TcpJsonlClient::QueueDelta(
    std::string json,
    std::string currentFullSnapshot)
{
    return mImpl->QueueDelta(
        std::move(json),
        std::move(currentFullSnapshot));
}

bool
TcpJsonlClient::IsConnected() const
{
    return mImpl->IsConnected();
}

std::size_t
TcpJsonlClient::PendingCount() const
{
    return mImpl->PendingCount();
}

} // namespace afsim_ns3
