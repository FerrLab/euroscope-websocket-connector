// End-to-end test for the Pusher gateway mode: the REAL Gateway +
// PusherSession + WsClient stack (POSIX build, see src/ws/WsClient.cpp)
// against a scripted Pusher-protocol server over live TCP.
//
// Covered: /app/{key} connection path, pusher:connection_established,
// pusher:subscribe with a verified HMAC auth token, subscription_succeeded
// gating (IsConnected/justConnected only after subscribe), client-event
// wrapping of outbound contract messages, inbound client-event commands,
// and the fatal pusher:error path (bad key -> long backoff, no reconnect
// storm).
//
// UNIX-only, like ws_client_test.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "Gateway.h"
#include "pusher/Crypto.h"
#include "ws/WsFrame.h"
#include "ws/WsHandshake.h"

using nlohmann::json;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                        \
    do                                                                     \
    {                                                                      \
        ++g_checks;                                                        \
        if (!(cond))                                                       \
        {                                                                  \
            ++g_failures;                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  "    \
                      << #cond << "\n";                                    \
        }                                                                  \
    } while (0)

namespace
{
    using Clock = std::chrono::steady_clock;

    const char* kAppKey = "test-app-key";
    const char* kSecret = "test-app-secret";
    const char* kChannel = "private-euroscope";

    std::vector<uint8_t> ServerFrame(Ws::Opcode opcode, const std::string& payload)
    {
        std::vector<uint8_t> out;
        out.push_back(static_cast<uint8_t>(0x80 | static_cast<uint8_t>(opcode)));
        const size_t len = payload.size();
        if (len < 126)
            out.push_back(static_cast<uint8_t>(len));
        else
        {
            out.push_back(126);
            out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>(len & 0xFF));
        }
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    // Scripted Pusher server for exactly one client connection.
    class PusherServer
    {
    public:
        std::atomic<int> port{ 0 };
        std::atomic<bool> gotValidAuth{ false };
        std::atomic<bool> gotBadPath{ false };
        std::atomic<bool> gotWrappedEvent{ false };
        std::string wrappedContract;  // data of the first client event (guarded by done flag)
        std::atomic<bool> rejectWithFatalError{ false };

        PusherServer() : m_thread(&PusherServer::Run, this) {}

        ~PusherServer()
        {
            if (m_listenFd >= 0)
                close(m_listenFd);
            if (m_thread.joinable())
                m_thread.join();
        }

    private:
        int m_listenFd = -1;
        std::thread m_thread;

        void Send(int fd, const std::string& text)
        {
            const std::vector<uint8_t> f = ServerFrame(Ws::Opcode::Text, text);
            (void)!write(fd, f.data(), f.size());
        }

        void Run()
        {
            m_listenFd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
                listen(m_listenFd, 2) != 0)
                return;
            socklen_t alen = sizeof(addr);
            getsockname(m_listenFd, reinterpret_cast<sockaddr*>(&addr), &alen);
            port = ntohs(addr.sin_port);

            const int fd = accept(m_listenFd, nullptr, nullptr);
            if (fd < 0)
                return;

            // WS handshake; also validate the Pusher connection path.
            std::string request;
            char buf[8192];
            while (request.find("\r\n\r\n") == std::string::npos)
            {
                const ssize_t n = read(fd, buf, sizeof(buf));
                if (n <= 0)
                {
                    close(fd);
                    return;
                }
                request.append(buf, static_cast<size_t>(n));
            }
            const std::string expectedPath = "GET /app/" + std::string(kAppKey) + "?";
            if (request.compare(0, expectedPath.size(), expectedPath) != 0)
                gotBadPath = true;

            const std::string marker = "Sec-WebSocket-Key: ";
            const size_t kpos = request.find(marker);
            const size_t kend = request.find("\r\n", kpos);
            const std::string key =
                request.substr(kpos + marker.size(), kend - kpos - marker.size());
            const std::string response =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + Ws::ExpectedAccept(key) + "\r\n\r\n";
            (void)!write(fd, response.data(), response.size());

            if (rejectWithFatalError)
            {
                Send(fd, R"({"event":"pusher:error","data":{"code":4001,"message":"App key does not exist"}})");
                close(fd);
                return;
            }

            const std::string socketId = "81607.9500";
            Send(fd, R"({"event":"pusher:connection_established","data":"{\"socket_id\":\")" +
                         socketId + R"(\",\"activity_timeout\":120}"})");

            // Frame loop
            std::vector<uint8_t> stream;
            Ws::MessageAssembler assembler;
            for (;;)
            {
                const ssize_t n = read(fd, buf, sizeof(buf));
                if (n <= 0)
                    break;
                stream.insert(stream.end(), buf, buf + n);

                size_t offset = 0;
                for (;;)
                {
                    Ws::Frame frame;
                    size_t consumed = 0;
                    std::string error;
                    if (Ws::TryDecodeFrame(stream.data() + offset,
                                           stream.size() - offset, frame, consumed,
                                           error) != Ws::DecodeResult::Ok)
                        break;
                    offset += consumed;

                    const auto ev = assembler.Feed(frame);
                    if (ev == Ws::MessageAssembler::Event::Ping)
                    {
                        const std::vector<uint8_t> pong = ServerFrame(
                            Ws::Opcode::Pong,
                            std::string(assembler.control.begin(), assembler.control.end()));
                        (void)!write(fd, pong.data(), pong.size());
                        continue;
                    }
                    if (ev == Ws::MessageAssembler::Event::Close)
                    {
                        close(fd);
                        return;
                    }
                    if (ev != Ws::MessageAssembler::Event::Message)
                        continue;

                    const json msg = json::parse(assembler.message, nullptr, false);
                    if (msg.is_discarded())
                        continue;
                    const std::string event = msg.value("event", "");

                    if (event == "pusher:subscribe")
                    {
                        const std::string channel = msg.at("data").value("channel", "");
                        const std::string auth = msg.at("data").value("auth", "");
                        const std::string expected =
                            std::string(kAppKey) + ":" +
                            Crypto::HmacSha256Hex(kSecret, socketId + ":" + channel);
                        if (channel == kChannel && auth == expected)
                        {
                            gotValidAuth = true;
                            Send(fd,
                                 R"({"event":"pusher_internal:subscription_succeeded","channel":")" +
                                     channel + R"(","data":"{}"})");
                            // Immediately push a command at the plugin, as a
                            // frontend would.
                            Send(fd,
                                 R"({"event":"client-euroscope","channel":")" + channel +
                                     R"(","data":"{\"type\":\"command\",\"id\":9,\"action\":\"ping\"}"})");
                        }
                        else
                        {
                            Send(fd, R"({"event":"pusher:error","data":{"code":4009,"message":"auth failed"}})");
                        }
                    }
                    else if (event == "client-euroscope")
                    {
                        if (!gotWrappedEvent)
                        {
                            wrappedContract = msg.at("data").is_string()
                                                  ? msg.at("data").get<std::string>()
                                                  : msg.at("data").dump();
                            gotWrappedEvent = true;
                        }
                    }
                }
                if (offset > 0)
                    stream.erase(stream.begin(), stream.begin() + static_cast<long>(offset));
            }
            close(fd);
        }
    };

    bool PumpUntil(Gateway& gateway, std::vector<std::string>& inbox,
                   bool& sawJustConnected, const std::function<bool()>& done,
                   int timeoutMs)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (Clock::now() < deadline)
        {
            bool justConnected = false;
            for (std::string& m : gateway.Tick(justConnected))
                inbox.push_back(std::move(m));
            if (justConnected)
                sawJustConnected = true;
            if (done())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return done();
    }
}

int main()
{
    // --- happy path: connect, token auth, subscribe, exchange ------------
    {
        PusherServer server;
        while (server.port.load() == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

        Gateway gateway;
        gateway.SetMode(GatewayMode::Pusher);
        gateway.SetPusherKey(kAppKey);
        gateway.SetPusherSecret(kSecret);
        gateway.SetPusherChannel(kChannel);
        gateway.SetClientVersion("0.4.0-test");
        CHECK(gateway.SetUrl("ws://127.0.0.1:" + std::to_string(server.port.load())).empty());
        CHECK(gateway.Enable().empty());

        std::vector<std::string> inbox;
        bool sawJustConnected = false;

        // becomes usable only after subscription succeeded, and the queued
        // command from the server arrives through Tick
        CHECK(PumpUntil(gateway, inbox, sawJustConnected,
                        [&] { return gateway.IsConnected() && !inbox.empty(); }, 5000));
        CHECK(sawJustConnected);
        CHECK(!server.gotBadPath.load());
        CHECK(server.gotValidAuth.load());
        CHECK(inbox.size() == 1u);
        if (!inbox.empty())
        {
            const json cmd = json::parse(inbox[0]);
            CHECK(cmd.at("action").get<std::string>() == "ping");
        }

        // outbound contract message arrives wrapped as a client event
        const std::string contract =
            R"({"type":"event","action":"flight_removed","callsign":"DLH4TX","payload":{}})";
        gateway.Send(contract);
        {
            const auto deadline = Clock::now() + std::chrono::seconds(5);
            while (!server.gotWrappedEvent.load() && Clock::now() < deadline)
            {
                bool jc = false;
                gateway.Tick(jc);
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        CHECK(server.gotWrappedEvent.load());
        CHECK(server.wrappedContract == contract);
        CHECK(gateway.GetStatus().sent == 1);

        gateway.Disable();
    }

    // --- fatal pusher:error (bad app) -> long backoff, no reconnect storm --
    {
        PusherServer server;
        server.rejectWithFatalError = true;
        while (server.port.load() == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

        Gateway gateway;
        gateway.SetMode(GatewayMode::Pusher);
        gateway.SetPusherKey(kAppKey);
        gateway.SetPusherSecret(kSecret);
        gateway.SetPusherChannel(kChannel);
        CHECK(gateway.SetUrl("ws://127.0.0.1:" + std::to_string(server.port.load())).empty());
        CHECK(gateway.Enable().empty());

        std::vector<std::string> inbox;
        bool sawJustConnected = false;
        CHECK(PumpUntil(gateway, inbox, sawJustConnected,
                        [&] {
                            return gateway.GetStatus().lastError.find("4001") !=
                                   std::string::npos;
                        },
                        5000));
        CHECK(!sawJustConnected);
        CHECK(!gateway.IsConnected());
        // parked in a long backoff rather than hammering the server
        CHECK(gateway.GetStatus().state.find("waiting to reconnect") != std::string::npos);
    }

    // --- config validation --------------------------------------------------
    {
        Gateway gateway;
        gateway.SetMode(GatewayMode::Pusher);
        CHECK(gateway.SetUrl("ws://127.0.0.1:1234").empty());
        CHECK(!gateway.Enable().empty());  // no key
        gateway.SetPusherKey("k");
        gateway.SetPusherChannel("private-x");
        CHECK(!gateway.Enable().empty());  // private channel, no secret
        gateway.SetPusherChannel("public-x-actually-public");
        CHECK(gateway.Enable().empty());  // public channel: fine without secret
        gateway.Disable();
    }

    std::cout << (g_failures == 0 ? "PASS" : "FAIL") << ": " << g_checks
              << " checks, " << g_failures << " failure(s)\n";
    return g_failures == 0 ? 0 : 1;
}
