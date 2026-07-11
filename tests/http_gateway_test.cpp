// Tests for the HTTPS transport: URL parsing (pure) and the REAL Gateway
// (poll thread + send thread + backoff + snapshot gating) end-to-end over
// live TCP against a scripted HTTP backend. UNIX-only (the test client and
// server use POSIX sockets); production uses WinHTTP over the same
// Http::Client interface.
//
// Covered: bearer-token auth on both endpoints, long-poll delivery of
// commands, 204 empty polls, batched POST of messages, justConnected
// gating + counters, 401 -> unhealthy with a clear error, backend-down
// error path, and Disable() promptly aborting an in-flight long poll.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "Gateway.h"
#include "http/HttpUrl.h"
#include "posix_http_client.h"

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

    const char* kGoodToken = "secret-token-1";

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

    // Minimal scripted HTTP/1.1 backend (Connection: close per request).
    class TestBackend
    {
    public:
        std::atomic<int> port{ 0 };
        std::atomic<int> polls{ 0 };
        std::atomic<int> unauthorized{ 0 };
        std::atomic<bool> holdNextPoll{ false };  // hold a poll ~10 s

        std::mutex mutex;
        std::vector<json> postedBatches;  // parsed bodies of POST /messages
        std::vector<std::string> pendingCommands;  // served on next poll

        TestBackend() : m_thread(&TestBackend::Run, this) {}

        ~TestBackend()
        {
            m_stopping = true;
            if (m_listenFd >= 0)
            {
                shutdown(m_listenFd, SHUT_RDWR);
                close(m_listenFd);
            }
            if (m_thread.joinable())
                m_thread.join();
        }

    private:
        std::atomic<bool> m_stopping{ false };
        int m_listenFd = -1;
        std::thread m_thread;

        static void Respond(int fd, int status, const std::string& body)
        {
            const char* text = status == 200   ? "OK"
                               : status == 204 ? "No Content"
                               : status == 401 ? "Unauthorized"
                                               : "Error";
            std::string response = "HTTP/1.1 " + std::to_string(status) + " " +
                                   text + "\r\n";
            response += "Content-Type: application/json\r\n";
            response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
            response += "Connection: close\r\n\r\n";
            response += body;
            (void)!write(fd, response.data(), response.size());
        }

        void Handle(int fd)
        {
            // Read the full request (headers + Content-Length body).
            std::string raw;
            char buf[8192];
            size_t bodyNeeded = std::string::npos;
            size_t headerEnd = std::string::npos;
            for (;;)
            {
                if (headerEnd != std::string::npos &&
                    raw.size() >= headerEnd + 4 + bodyNeeded)
                    break;
                const ssize_t n = read(fd, buf, sizeof(buf));
                if (n <= 0)
                    break;
                raw.append(buf, static_cast<size_t>(n));
                if (headerEnd == std::string::npos)
                {
                    headerEnd = raw.find("\r\n\r\n");
                    if (headerEnd != std::string::npos)
                    {
                        bodyNeeded = 0;
                        const std::string marker = "Content-Length: ";
                        const size_t cl = raw.find(marker);
                        if (cl != std::string::npos && cl < headerEnd)
                            bodyNeeded = static_cast<size_t>(
                                std::atoi(raw.c_str() + cl + marker.size()));
                    }
                }
            }
            if (headerEnd == std::string::npos)
            {
                close(fd);
                return;
            }

            const std::string head = raw.substr(0, headerEnd);
            const std::string body = raw.substr(headerEnd + 4);
            const bool authorized =
                head.find(std::string("Authorization: Bearer ") + kGoodToken) !=
                std::string::npos;

            if (!authorized)
            {
                ++unauthorized;
                Respond(fd, 401, R"({"message":"unauthenticated"})");
                close(fd);
                return;
            }

            if (head.compare(0, 4, "GET ") == 0 &&
                head.find("/euroscope/poll?timeout=") != std::string::npos)
            {
                ++polls;
                if (holdNextPoll.exchange(false))
                {
                    // Simulate the server holding the long poll open.
                    for (int i = 0; i < 1000 && !m_stopping; ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    Respond(fd, 204, "");
                }
                else
                {
                    std::vector<std::string> commands;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        commands.swap(pendingCommands);
                    }
                    if (commands.empty())
                    {
                        // Brief hold, then an empty poll result.
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        Respond(fd, 204, "");
                    }
                    else
                    {
                        json arr = json::array();
                        for (const std::string& c : commands)
                            arr.push_back(json::parse(c));
                        Respond(fd, 200, json{ { "commands", arr } }.dump());
                    }
                }
            }
            else if (head.compare(0, 5, "POST ") == 0 &&
                     head.find("/euroscope/messages") != std::string::npos)
            {
                const json parsed = json::parse(body, nullptr, false);
                if (parsed.is_discarded() || !parsed.contains("messages"))
                {
                    Respond(fd, 422, R"({"message":"bad body"})");
                }
                else
                {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        postedBatches.push_back(parsed);
                    }
                    Respond(fd, 204, "");
                }
            }
            else
            {
                Respond(fd, 404, R"({"message":"not found"})");
            }
            close(fd);
        }

        void Run()
        {
            m_listenFd = socket(AF_INET, SOCK_STREAM, 0);
            const int reuse = 1;
            setsockopt(m_listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
                listen(m_listenFd, 8) != 0)
                return;
            socklen_t alen = sizeof(addr);
            getsockname(m_listenFd, reinterpret_cast<sockaddr*>(&addr), &alen);
            port = ntohs(addr.sin_port);

            // One handler thread per connection so a held long poll does
            // not block the next request (the gateway polls and posts
            // concurrently).
            std::vector<std::thread> workers;
            for (;;)
            {
                const int fd = accept(m_listenFd, nullptr, nullptr);
                if (fd < 0)
                    break;
                workers.emplace_back(&TestBackend::Handle, this, fd);
            }
            for (std::thread& w : workers)
                if (w.joinable())
                    w.join();
        }
    };

    int PumpUntil(Gateway& gateway, std::vector<std::string>& inbox,
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
                return 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return done() ? 1 : 0;
    }
}

int main()
{
    // --- URL parsing --------------------------------------------------------
    {
        Http::Url u = Http::ParseUrl("https://api.example.com/euroscope");
        CHECK(u.ok && u.https && u.port == 443);
        CHECK(u.host == "api.example.com");
        CHECK(u.basePath == "/euroscope");

        Http::Url p = Http::ParseUrl("http://127.0.0.1:8000/api/es/");
        CHECK(p.ok && !p.https && p.port == 8000);
        CHECK(p.basePath == "/api/es");  // trailing slash stripped

        Http::Url root = Http::ParseUrl("https://x.test");
        CHECK(root.ok && root.basePath.empty());

        CHECK(!Http::ParseUrl("ws://x.test").ok);
        CHECK(!Http::ParseUrl("https://").ok);
        CHECK(!Http::ParseUrl("https://h:99999/x").ok);
    }

    // --- happy path: auth, poll, batch POST, snapshot gating ----------------
    {
        TestBackend backend;
        CHECK(WaitFor([&] { return backend.port.load() != 0; }, 2000));
        {
            std::lock_guard<std::mutex> lock(backend.mutex);
            backend.pendingCommands.push_back(
                R"({"type":"command","id":9,"action":"ping"})");
        }

        Gateway gateway(&MakePosixHttpClient);
        CHECK(gateway.SetUrl("http://127.0.0.1:" +
                             std::to_string(backend.port.load()) + "/euroscope")
                  .empty());
        CHECK(!gateway.Enable().empty());  // refuses without a token
        gateway.SetToken(kGoodToken);
        CHECK(gateway.Enable().empty());

        std::vector<std::string> inbox;
        bool sawJustConnected = false;
        CHECK(PumpUntil(gateway, inbox, sawJustConnected,
                        [&] { return gateway.IsConnected() && !inbox.empty(); },
                        5000));
        CHECK(sawJustConnected);
        CHECK(inbox.size() == 1u);
        if (!inbox.empty())
            CHECK(json::parse(inbox[0]).at("action").get<std::string>() == "ping");

        // messages batch into one POST
        gateway.Send(R"({"type":"event","action":"flight_removed","callsign":"A","payload":{}})");
        gateway.Send(R"({"type":"event","action":"flight_removed","callsign":"B","payload":{}})");
        CHECK(WaitFor(
            [&] {
                std::lock_guard<std::mutex> lock(backend.mutex);
                return !backend.postedBatches.empty();
            },
            5000));
        {
            std::lock_guard<std::mutex> lock(backend.mutex);
            CHECK(backend.postedBatches.size() == 1u);
            const json& batch = backend.postedBatches[0];
            CHECK(batch.at("messages").size() == 2u);
            CHECK(batch.at("messages")[0].at("callsign").get<std::string>() == "A");
            CHECK(batch.at("messages")[1].at("callsign").get<std::string>() == "B");
        }
        const Gateway::Status s = gateway.GetStatus();
        CHECK(s.state == "connected");
        CHECK(s.sent == 2);
        CHECK(s.received == 1);

        // commands keep flowing on later polls
        {
            std::lock_guard<std::mutex> lock(backend.mutex);
            backend.pendingCommands.push_back(
                R"({"type":"command","id":10,"action":"get_flight","callsign":"A"})");
        }
        inbox.clear();
        CHECK(PumpUntil(gateway, inbox, sawJustConnected,
                        [&] { return !inbox.empty(); }, 5000));

        // Disable() must abort an in-flight held long poll promptly.
        backend.holdNextPoll = true;
        CHECK(WaitFor([&] { return backend.holdNextPoll.load() == false; }, 5000));
        const auto t0 = Clock::now();
        gateway.Disable();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            Clock::now() - t0)
                            .count();
        CHECK(ms < 2000);
        CHECK(!gateway.IsConnected());
    }

    // --- bad token: 401 surfaces, never healthy ------------------------------
    {
        TestBackend backend;
        CHECK(WaitFor([&] { return backend.port.load() != 0; }, 2000));

        Gateway gateway(&MakePosixHttpClient);
        CHECK(gateway.SetUrl("http://127.0.0.1:" +
                             std::to_string(backend.port.load()) + "/euroscope")
                  .empty());
        gateway.SetToken("wrong-token");
        CHECK(gateway.Enable().empty());

        std::vector<std::string> inbox;
        bool sawJustConnected = false;
        CHECK(PumpUntil(gateway, inbox, sawJustConnected,
                        [&] {
                            const Gateway::Status s = gateway.GetStatus();
                            return s.lastError.find("401") != std::string::npos;
                        },
                        5000));
        CHECK(!sawJustConnected);
        CHECK(!gateway.IsConnected());
        CHECK(backend.unauthorized.load() >= 1);
        gateway.Disable();
    }

    // --- backend unreachable: clean error, no crash ---------------------------
    {
        Gateway gateway(&MakePosixHttpClient);
        CHECK(gateway.SetUrl("http://127.0.0.1:1/euroscope").empty());
        gateway.SetToken(kGoodToken);
        CHECK(gateway.Enable().empty());
        std::vector<std::string> inbox;
        bool sawJustConnected = false;
        CHECK(PumpUntil(gateway, inbox, sawJustConnected,
                        [&] {
                            return !gateway.GetStatus().lastError.empty();
                        },
                        5000));
        CHECK(!gateway.IsConnected());
        gateway.Disable();
    }

    std::cout << (g_failures == 0 ? "PASS" : "FAIL") << ": " << g_checks
              << " checks, " << g_failures << " failure(s)\n";
    return g_failures == 0 ? 0 : 1;
}
