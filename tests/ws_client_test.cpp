// End-to-end integration test for WsClient (POSIX build of the exact
// production code, see the #ifdef note in src/ws/WsClient.cpp): the real
// client connects over real TCP to a minimal in-process WebSocket server
// implemented on the same pure codec.
//
// Covered: TCP connect, RFC 6455 handshake, masked client frames, echo
// round-trips, server-initiated messages, fragmented server messages,
// ping/pong keepalive answer, server-initiated close, connection-refused
// error path, and Stop() responsiveness.
//
// UNIX-only (the test server uses POSIX sockets); the production DLL uses
// the Win32 branch of the same WsClient code.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "ws/WsClient.h"
#include "ws/WsFrame.h"
#include "ws/WsHandshake.h"

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

    bool WaitFor(const std::function<bool()>& predicate, int timeoutMs)
    {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        while (Clock::now() < deadline)
        {
            if (predicate())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return predicate();
    }

    // Server->client frames are unmasked.
    std::vector<uint8_t> EncodeServerFrame(Ws::Opcode opcode,
                                           const std::string& payload,
                                           bool fin = true)
    {
        std::vector<uint8_t> out;
        out.push_back(static_cast<uint8_t>((fin ? 0x80 : 0x00) |
                                           static_cast<uint8_t>(opcode)));
        const size_t len = payload.size();
        if (len < 126)
        {
            out.push_back(static_cast<uint8_t>(len));
        }
        else if (len <= 0xFFFF)
        {
            out.push_back(126);
            out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>(len & 0xFF));
        }
        else
        {
            out.push_back(127);
            for (int i = 7; i >= 0; --i)
                out.push_back(static_cast<uint8_t>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
        }
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    // Minimal scripted WebSocket server: handshakes one client, echoes
    // text messages, and reacts to magic payloads:
    //   "push"     -> echo + an extra server-initiated message
    //   "fragment" -> reply "Hello" split into two frames
    //   "pingme"   -> send a Ping (expects the client to Pong)
    //   "bye"      -> initiate close
    class TestServer
    {
    public:
        std::atomic<bool> sawPong{ false };
        std::atomic<bool> handshakeOk{ false };
        std::atomic<int> port{ 0 };

        TestServer() : m_thread(&TestServer::Run, this) {}

        ~TestServer()
        {
            if (m_listenFd >= 0)
                close(m_listenFd);
            if (m_thread.joinable())
                m_thread.join();
        }

    private:
        int m_listenFd = -1;
        std::thread m_thread;

        void SendRaw(int fd, const std::vector<uint8_t>& bytes)
        {
            (void)!write(fd, bytes.data(), bytes.size());
        }

        void Run()
        {
            m_listenFd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            if (bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
                listen(m_listenFd, 1) != 0)
                return;
            socklen_t len = sizeof(addr);
            getsockname(m_listenFd, reinterpret_cast<sockaddr*>(&addr), &len);
            port = ntohs(addr.sin_port);

            const int fd = accept(m_listenFd, nullptr, nullptr);
            if (fd < 0)
                return;

            // --- handshake ------------------------------------------------
            std::string request;
            char buf[4096];
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
            const std::string marker = "Sec-WebSocket-Key: ";
            const size_t kpos = request.find(marker);
            if (kpos == std::string::npos)
            {
                close(fd);
                return;
            }
            const size_t kend = request.find("\r\n", kpos);
            const std::string key =
                request.substr(kpos + marker.size(), kend - kpos - marker.size());

            const std::string response =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + Ws::ExpectedAccept(key) + "\r\n"
                "\r\n";
            (void)!write(fd, response.data(), response.size());
            handshakeOk = true;

            // --- frame loop -------------------------------------------------
            std::vector<uint8_t> stream;
            Ws::MessageAssembler assembler;
            for (;;)
            {
                const ssize_t n = read(fd, buf, sizeof(buf));
                if (n <= 0)
                    break;
                stream.insert(stream.end(), buf, buf + n);

                size_t offset = 0;
                bool done = false;
                for (;;)
                {
                    Ws::Frame frame;
                    size_t consumed = 0;
                    std::string error;
                    const Ws::DecodeResult dr = Ws::TryDecodeFrame(
                        stream.data() + offset, stream.size() - offset, frame,
                        consumed, error);
                    if (dr != Ws::DecodeResult::Ok)
                        break;
                    offset += consumed;

                    switch (assembler.Feed(frame))
                    {
                        case Ws::MessageAssembler::Event::Message:
                        {
                            const std::string msg = assembler.message;
                            SendRaw(fd, EncodeServerFrame(Ws::Opcode::Text, msg));
                            if (msg == "push")
                                SendRaw(fd, EncodeServerFrame(Ws::Opcode::Text,
                                                              "server-push"));
                            else if (msg == "fragment")
                            {
                                SendRaw(fd, EncodeServerFrame(Ws::Opcode::Text,
                                                              "Hel", false));
                                SendRaw(fd,
                                        EncodeServerFrame(Ws::Opcode::Continuation,
                                                          "lo", true));
                            }
                            else if (msg == "pingme")
                                SendRaw(fd, EncodeServerFrame(Ws::Opcode::Ping, "k"));
                            else if (msg == "bye")
                            {
                                SendRaw(fd, EncodeServerFrame(Ws::Opcode::Close,
                                                              std::string("\x03\xe8", 2)));
                                done = true;
                            }
                            break;
                        }
                        case Ws::MessageAssembler::Event::Pong:
                            sawPong = true;
                            break;
                        case Ws::MessageAssembler::Event::Ping:
                            SendRaw(fd, EncodeServerFrame(Ws::Opcode::Pong,
                                                          std::string(assembler.control.begin(),
                                                                      assembler.control.end())));
                            break;
                        case Ws::MessageAssembler::Event::Close:
                            done = true;
                            break;
                        default:
                            break;
                    }
                    if (done)
                        break;
                }
                if (offset > 0)
                    stream.erase(stream.begin(), stream.begin() + static_cast<long>(offset));
                if (done)
                    break;
            }
            close(fd);
        }
    };
}

int main()
{
    // --- connection refused ------------------------------------------------
    {
        Ws::Url url = Ws::ParseUrl("ws://127.0.0.1:1/");  // port 1: nothing there
        Ws::WsClient client(url);
        CHECK(WaitFor([&] { return client.State() == Ws::ClientState::Closed; }, 5000));
        CHECK(!client.LastError().empty());
    }

    // --- full lifecycle against the scripted server --------------------------
    {
        TestServer server;
        CHECK(WaitFor([&] { return server.port.load() != 0; }, 2000));

        Ws::Url url = Ws::ParseUrl("ws://127.0.0.1:" +
                                   std::to_string(server.port.load()) + "/session");
        Ws::WsClient client(url);

        CHECK(WaitFor([&] { return client.State() == Ws::ClientState::Connected; }, 5000));
        CHECK(server.handshakeOk);

        std::vector<std::string> received;
        auto drainUntil = [&](size_t count, int timeoutMs) {
            return WaitFor(
                [&] {
                    for (std::string& m : client.TakeReceived())
                        received.push_back(std::move(m));
                    return received.size() >= count;
                },
                timeoutMs);
        };

        // echo round-trips
        CHECK(client.Send("hello-1"));
        CHECK(client.Send("hello-2"));
        CHECK(drainUntil(2, 5000));
        CHECK(received[0] == "hello-1");
        CHECK(received[1] == "hello-2");

        // server-initiated push
        CHECK(client.Send("push"));
        CHECK(drainUntil(4, 5000));
        CHECK(received[2] == "push");
        CHECK(received[3] == "server-push");

        // fragmented server message is reassembled
        CHECK(client.Send("fragment"));
        CHECK(drainUntil(6, 5000));
        CHECK(received[4] == "fragment");
        CHECK(received[5] == "Hello");

        // client answers server pings automatically
        CHECK(client.Send("pingme"));
        CHECK(WaitFor([&] { return server.sawPong.load(); }, 5000));

        // server-initiated close -> client reports Closed
        CHECK(client.Send("bye"));
        CHECK(WaitFor([&] { return client.State() == Ws::ClientState::Closed; }, 5000));
        CHECK(client.LastError().find("closed") != std::string::npos);
    }

    // --- Stop() is prompt ------------------------------------------------------
    {
        TestServer server;
        CHECK(WaitFor([&] { return server.port.load() != 0; }, 2000));
        Ws::Url url = Ws::ParseUrl("ws://127.0.0.1:" +
                                   std::to_string(server.port.load()) + "/");
        const auto t0 = Clock::now();
        {
            Ws::WsClient client(url);
            WaitFor([&] { return client.State() == Ws::ClientState::Connected; }, 5000);
            client.Stop();
        }  // destructor joins the thread
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 Clock::now() - t0)
                                 .count();
        CHECK(elapsed < 2000);
    }

    std::cout << (g_failures == 0 ? "PASS" : "FAIL") << ": " << g_checks
              << " checks, " << g_failures << " failure(s)\n";
    return g_failures == 0 ? 0 : 1;
}
