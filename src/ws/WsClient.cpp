#include "WsClient.h"

// The socket layer is Win32 in production (EuroScope is Windows-only). The
// thin POSIX branch below exists ONLY so tests/ws_client_test.cpp can run
// this exact code against a live in-process server on the CI/dev machine.
#ifdef _WIN32
// winsock2.h must come before windows.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

#include "WsFrame.h"

namespace Ws
{
    namespace
    {
        const uintptr_t kInvalidSocket = static_cast<uintptr_t>(INVALID_SOCKET);

#ifdef _WIN32
        // WSAStartup/WSACleanup are reference-counted by Windows, so a
        // pair per client is safe and keeps this self-contained.
        struct WsaGuard
        {
            bool ok = false;
            WsaGuard()
            {
                WSADATA data;
                ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
            }
            ~WsaGuard()
            {
                if (ok)
                    WSACleanup();
            }
        };

        uint32_t TickSeed()
        {
            return static_cast<uint32_t>(GetTickCount());
        }
#else
        struct WsaGuard
        {
            bool ok = true;
        };

        uint32_t TickSeed()
        {
            return static_cast<uint32_t>(
                std::chrono::steady_clock::now().time_since_epoch().count());
        }
#endif
    }

    WsClient::WsClient(const Url& url)
        : m_url(url),
          m_socket(kInvalidSocket),
          m_rngState(TickSeed() ^
                     static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this)) ^
                     0x9E3779B9u),
          m_thread(&WsClient::Run, this)
    {
    }

    WsClient::~WsClient()
    {
        Stop();
        if (m_thread.joinable())
            m_thread.join();
    }

    void WsClient::Stop()
    {
        m_stop = true;
        // shutdown() wakes a thread blocked in recv/send on ALL platforms
        // (on POSIX, close() alone does not), then close the socket.
        std::lock_guard<std::mutex> lock(m_socketMutex);
        if (m_socket != kInvalidSocket)
        {
#ifdef _WIN32
            shutdown(static_cast<SOCKET>(m_socket), SD_BOTH);
#else
            shutdown(static_cast<SOCKET>(m_socket), SHUT_RDWR);
#endif
            closesocket(static_cast<SOCKET>(m_socket));
            m_socket = kInvalidSocket;
        }
    }

    std::string WsClient::LastError() const
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        return m_lastError;
    }

    bool WsClient::Send(const std::string& text)
    {
        std::lock_guard<std::mutex> lock(m_outMutex);
        if (m_outbound.size() >= kMaxQueued)
            return false;
        m_outbound.push_back(text);
        return true;
    }

    std::vector<std::string> WsClient::TakeReceived()
    {
        std::lock_guard<std::mutex> lock(m_inMutex);
        std::vector<std::string> out(m_inbound.begin(), m_inbound.end());
        m_inbound.clear();
        return out;
    }

    void WsClient::Fail(const std::string& error)
    {
        {
            std::lock_guard<std::mutex> lock(m_errorMutex);
            if (m_lastError.empty())
                m_lastError = error;
        }
        m_state = ClientState::Closed;
    }

    uint32_t WsClient::NextRandom()
    {
        // xorshift32 - mask keys need to be unpredictable enough to avoid
        // proxy cache poisoning, not cryptographically strong (no TLS
        // here anyway).
        uint32_t x = m_rngState;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        m_rngState = x;
        return x;
    }

    bool WsClient::SendAll(const uint8_t* data, size_t len)
    {
        size_t sent = 0;
        while (sent < len && !m_stop)
        {
            const int n = send(static_cast<SOCKET>(m_socket),
                               reinterpret_cast<const char*>(data + sent),
                               static_cast<int>(len - sent), 0);
            if (n <= 0)
                return false;
            sent += static_cast<size_t>(n);
        }
        return sent == len;
    }

    bool WsClient::SendFrame(Opcode opcode, const uint8_t* payload, size_t len)
    {
        const uint32_t r = NextRandom();
        const uint8_t mask[4] = {
            static_cast<uint8_t>(r & 0xFF),
            static_cast<uint8_t>((r >> 8) & 0xFF),
            static_cast<uint8_t>((r >> 16) & 0xFF),
            static_cast<uint8_t>((r >> 24) & 0xFF),
        };
        const std::vector<uint8_t> wire = EncodeFrame(opcode, payload, len, mask);
        return SendAll(wire.data(), wire.size());
    }

    void WsClient::Run()
    {
        WsaGuard wsa;
        if (!wsa.ok)
        {
            Fail("WSAStartup failed");
            return;
        }

        // --- resolve + connect -----------------------------------------

        addrinfo hints = {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* results = nullptr;
        if (getaddrinfo(m_url.host.c_str(), m_url.port.c_str(), &hints,
                        &results) != 0 ||
            results == nullptr)
        {
            Fail("could not resolve " + m_url.host);
            return;
        }

        SOCKET sock = INVALID_SOCKET;
        for (addrinfo* ai = results; ai != nullptr && !m_stop; ai = ai->ai_next)
        {
            sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (sock == INVALID_SOCKET)
                continue;
            if (connect(sock, ai->ai_addr,
                        static_cast<int>(ai->ai_addrlen)) == 0)
                break;
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
        freeaddrinfo(results);

        if (sock == INVALID_SOCKET)
        {
            Fail("could not connect to " + m_url.host + ":" + m_url.port);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_socketMutex);
            if (m_stop)
            {
                closesocket(sock);
                return;
            }
            m_socket = static_cast<uintptr_t>(sock);
        }

        const int noDelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

        // --- handshake ---------------------------------------------------

        uint8_t random16[16];
        for (int i = 0; i < 16; i += 4)
        {
            const uint32_t r = NextRandom();
            random16[i] = static_cast<uint8_t>(r & 0xFF);
            random16[i + 1] = static_cast<uint8_t>((r >> 8) & 0xFF);
            random16[i + 2] = static_cast<uint8_t>((r >> 16) & 0xFF);
            random16[i + 3] = static_cast<uint8_t>((r >> 24) & 0xFF);
        }
        const std::string key = MakeKey(random16);
        const std::string request = BuildRequest(m_url, key);
        if (!SendAll(reinterpret_cast<const uint8_t*>(request.data()),
                     request.size()))
        {
            Fail("failed to send handshake");
            return;
        }

        std::string response;
        std::vector<uint8_t> buffer;  // frame bytes after the handshake
        int handshakeTicks = 0;
        const int kHandshakeTimeoutTicks = 300;  // ~15 s at 50 ms per tick
        for (;;)
        {
            if (m_stop)
                return;

            // Bounded wait: a server that accepted TCP but never answers
            // must not wedge us in Connecting forever.
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(sock, &readSet);
            timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 50 * 1000;
            const int ready = select(static_cast<int>(sock) + 1, &readSet,
                                     nullptr, nullptr, &timeout);
            if (ready < 0)
            {
                Fail("select failed during handshake");
                return;
            }
            if (ready == 0)
            {
                if (++handshakeTicks >= kHandshakeTimeoutTicks)
                {
                    Fail("handshake timeout (no response from server)");
                    return;
                }
                continue;
            }

            char chunk[4096];
            const int n = recv(sock, chunk, sizeof(chunk), 0);
            if (n <= 0)
            {
                Fail("connection closed during handshake");
                return;
            }
            response.append(chunk, static_cast<size_t>(n));

            size_t consumed = 0;
            std::string error;
            const HandshakeResult hr = ParseResponse(response, key, consumed, error);
            if (hr == HandshakeResult::NeedMore)
                continue;
            if (hr == HandshakeResult::Error)
            {
                Fail("handshake rejected: " + error);
                return;
            }
            buffer.assign(response.begin() + static_cast<long>(consumed),
                          response.end());
            break;
        }

        m_state = ClientState::Connected;

        // --- frame pump ---------------------------------------------------

        MessageAssembler assembler;
        int ticksSincePing = 0;
        const int kPingEveryTicks = 600;  // ~30 s at 50 ms per tick

        // Decodes every complete frame currently in `buffer`. Returns
        // false when the connection must end (Fail() already called).
        const auto processBuffer = [&]() -> bool {
            size_t offset = 0;
            bool keepGoing = true;
            while (keepGoing)
            {
                Frame frame;
                size_t consumed = 0;
                std::string error;
                const DecodeResult dr = TryDecodeFrame(
                    buffer.data() + offset, buffer.size() - offset, frame,
                    consumed, error);
                if (dr == DecodeResult::NeedMore)
                    break;
                if (dr == DecodeResult::Error)
                {
                    Fail("protocol error: " + error);
                    keepGoing = false;
                    break;
                }
                offset += consumed;

                switch (assembler.Feed(frame))
                {
                    case MessageAssembler::Event::Message:
                        {
                            std::lock_guard<std::mutex> lock(m_inMutex);
                            if (m_inbound.size() < kMaxQueued)
                                m_inbound.push_back(std::move(assembler.message));
                        }
                        break;
                    case MessageAssembler::Event::Ping:
                        if (!SendFrame(Opcode::Pong, assembler.control.data(),
                                       assembler.control.size()))
                        {
                            Fail("pong failed");
                            keepGoing = false;
                        }
                        break;
                    case MessageAssembler::Event::Close:
                        // acknowledge and finish
                        SendFrame(Opcode::Close, assembler.control.data(),
                                  assembler.control.size() >= 2 ? 2 : 0);
                        Fail("closed by peer");
                        keepGoing = false;
                        break;
                    case MessageAssembler::Event::Error:
                        Fail("protocol error: " + assembler.error);
                        keepGoing = false;
                        break;
                    case MessageAssembler::Event::Pong:
                    case MessageAssembler::Event::None:
                        break;
                }
            }
            if (offset > 0)
                buffer.erase(buffer.begin(),
                             buffer.begin() + static_cast<long>(offset));
            return keepGoing;
        };

        // The server's first frames may have arrived coalesced with the
        // handshake response - process that leftover BEFORE waiting for
        // more bytes, or a server that speaks first (e.g. Pusher's
        // connection_established) would deadlock us.
        if (!processBuffer())
            return;

        while (!m_stop)
        {
            // 1. flush outbound queue
            for (;;)
            {
                std::string next;
                {
                    std::lock_guard<std::mutex> lock(m_outMutex);
                    if (m_outbound.empty())
                        break;
                    next = std::move(m_outbound.front());
                    m_outbound.pop_front();
                }
                if (!SendFrame(Opcode::Text,
                               reinterpret_cast<const uint8_t*>(next.data()),
                               next.size()))
                {
                    Fail("send failed");
                    return;
                }
            }

            // 2. wait up to 50 ms for readability
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(sock, &readSet);
            timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 50 * 1000;
            // First parameter: ignored on Windows, maxfd+1 on POSIX.
            const int ready = select(static_cast<int>(sock) + 1, &readSet,
                                     nullptr, nullptr, &timeout);
            if (ready < 0)
            {
                Fail("select failed");
                return;
            }

            // 3. keepalive
            if (++ticksSincePing >= kPingEveryTicks)
            {
                ticksSincePing = 0;
                if (!SendFrame(Opcode::Ping, nullptr, 0))
                {
                    Fail("ping failed");
                    return;
                }
            }

            if (ready == 0)
                continue;

            // 4. read + decode
            char chunk[65536];
            const int n = recv(sock, chunk, sizeof(chunk), 0);
            if (n <= 0)
            {
                Fail("connection closed by peer");
                return;
            }
            buffer.insert(buffer.end(), chunk, chunk + n);

            if (!processBuffer())
                return;
        }

        // graceful local close
        SendFrame(Opcode::Close, nullptr, 0);
        m_state = ClientState::Closed;
    }
}
