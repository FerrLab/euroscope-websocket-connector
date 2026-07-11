#pragma once

// Gateway — the HTTPS transport between the plugin and the backend.
//
// Wire model (docs/PROTOCOL.md "Transport"):
//   * plugin -> backend : POST {base}/messages  {"messages":[ ... ]}
//                         batched contract messages (events, responses)
//   * backend -> plugin : GET  {base}/poll?timeout=25   (long poll)
//                         200 {"commands":[ ... ]} or 204 when the hold
//                         expires with nothing to deliver
//   * both requests carry "Authorization: Bearer <token>"
//
// The backend is a plain HTTPS app (e.g. Laravel): it stores/forwards the
// plugin's messages (typically re-broadcasting them via Soketi/Reverb to
// browsers) and queues commands for the plugin per token.
//
// Threading: two worker threads (poll + send) own all HTTP I/O; the
// EuroScope main thread only touches the mutex-guarded queues via the
// public API (SetUrl/Enable/Tick/Send/... are main-thread only). On
// Windows the HTTP work is done by WinHTTP - TLS and certificate
// validation are the OS's job.
//
// Failure model: on any failure the affected thread backs off
// exponentially (2s -> 60s); auth failures (401/403) park at the maximum
// straight away. Messages produced while unhealthy are dropped and
// counted - consumers are made consistent again by the session snapshot
// the plugin sends on every unhealthy->healthy transition.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "http/HttpClient.h"

class Gateway
{
public:
    struct Status
    {
        bool enabled = false;
        std::string url;
        std::string state;      // "disabled", "connecting", "connected",
                                // "retrying"
        std::string lastError;  // most recent failure, if any
        unsigned long sent = 0;      // messages POSTed successfully
        unsigned long received = 0;  // commands received via poll
        unsigned long dropped = 0;   // messages dropped while unhealthy
    };

    // `factory` creates one Http::Client per request; the default uses
    // WinHTTP on Windows. Tests inject a plain-HTTP client here.
    explicit Gateway(Http::ClientFactory factory = nullptr);
    ~Gateway();

    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    // Validates and stores the backend base URL
    // (https://host[:port]/base). Returns an error string, or empty on
    // success. Reconnects if currently enabled.
    std::string SetUrl(const std::string& url);
    std::string GetUrl() const { return m_urlString; }
    bool HasValidUrl() const { return m_url.ok; }

    // Bearer token sent on every request. Reconnects if enabled.
    void SetToken(const std::string& token);
    bool HasToken() const { return !m_token.empty(); }

    // Enable = start the workers and keep retrying; Disable = stop
    // everything (blocks briefly to join the threads).
    std::string Enable();
    void Disable();
    bool IsEnabled() const { return m_enabled; }

    // Healthy = the last poll or POST round-trip succeeded.
    bool IsConnected() const;

    // Call once per second (from OnTimer). Returns commands received from
    // the backend; sets `justConnected` on the tick the transport became
    // healthy (the caller sends the session snapshot).
    std::vector<std::string> Tick(bool& justConnected);

    // Queues a contract message for the next POST batch when healthy;
    // silently drops (and counts) it otherwise.
    void Send(const std::string& message);

    Status GetStatus() const;

    // Tunables (exposed for tests/documentation).
    static constexpr int kPollHoldSeconds = 25;   // server-side hold hint
    static constexpr int kPollTimeoutMs = 40000;  // client cap > hold time
    static constexpr int kPostTimeoutMs = 30000;
    static constexpr int kInitialBackoffSeconds = 2;
    static constexpr int kMaxBackoffSeconds = 60;
    static constexpr size_t kMaxQueued = 5000;    // outbound queue cap
    static constexpr size_t kMaxBatchMessages = 200;
    static constexpr size_t kMaxBatchBytes = 512 * 1024;

private:
    void PollLoop();
    void SendLoop();
    void MarkHealthy();
    void MarkUnhealthy(const std::string& error);
    // Interruptible sleep; returns false when stopping.
    bool SleepFor(int seconds);
    std::unique_ptr<Http::Client> MakeClient();

    // --- configuration (main thread; only changed while workers are
    // stopped) ---------------------------------------------------------
    Http::ClientFactory m_factory;
    Http::Url m_url;
    std::string m_urlString;
    std::string m_token;
    bool m_enabled = false;
    bool m_connectAnnounced = false;  // main-thread: justConnected edge

    // --- shared with the worker threads --------------------------------
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stop = false;
    bool m_healthy = false;
    std::string m_lastError;
    std::deque<std::string> m_outbound;
    std::deque<std::string> m_inbound;
    unsigned long m_sent = 0;
    unsigned long m_received = 0;
    unsigned long m_dropped = 0;
    // in-flight clients, so Disable() can abort blocked requests
    std::shared_ptr<Http::Client> m_activePoll;
    std::shared_ptr<Http::Client> m_activeSend;

    std::thread m_pollThread;
    std::thread m_sendThread;
};
