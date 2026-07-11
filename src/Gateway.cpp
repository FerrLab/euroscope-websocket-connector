#include "Gateway.h"

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace
{
    bool IsAuthStatus(int status)
    {
        return status == 401 || status == 403;
    }
}

Gateway::Gateway(Http::ClientFactory factory) : m_factory(factory)
{
#ifdef _WIN32
    if (!m_factory)
        m_factory = [] { return Http::CreateWinHttpClient(); };
#endif
}

Gateway::~Gateway()
{
    Disable();
}

std::unique_ptr<Http::Client> Gateway::MakeClient()
{
    return m_factory ? m_factory() : nullptr;
}

std::string Gateway::SetUrl(const std::string& url)
{
    const Http::Url parsed = Http::ParseUrl(url);
    if (!parsed.ok)
        return parsed.error;

    const bool wasEnabled = m_enabled;
    Disable();
    m_url = parsed;
    m_urlString = url;
    if (wasEnabled)
        Enable();
    return std::string();
}

void Gateway::SetToken(const std::string& token)
{
    const bool wasEnabled = m_enabled;
    Disable();
    m_token = token;
    if (wasEnabled)
        Enable();
}

std::string Gateway::Enable()
{
    if (m_enabled)
        return std::string();
    if (!m_url.ok)
        return "no backend URL configured - use: .lpc gateway url https://host/base";
    if (m_token.empty())
        return "no auth token configured - use: .lpc gateway token <token>";
    if (!m_factory)
        return "no HTTP backend available on this platform";

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = false;
        m_healthy = false;
        m_outbound.clear();
        m_inbound.clear();
    }
    m_connectAnnounced = false;
    m_enabled = true;
    m_pollThread = std::thread(&Gateway::PollLoop, this);
    m_sendThread = std::thread(&Gateway::SendLoop, this);
    return std::string();
}

void Gateway::Disable()
{
    if (!m_enabled && !m_pollThread.joinable() && !m_sendThread.joinable())
        return;

    std::shared_ptr<Http::Client> poll;
    std::shared_ptr<Http::Client> send;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
        poll = m_activePoll;
        send = m_activeSend;
    }
    m_wake.notify_all();
    // Unblock requests in flight (a long poll can otherwise hold the
    // thread for tens of seconds).
    if (poll)
        poll->Abort();
    if (send)
        send->Abort();

    if (m_pollThread.joinable())
        m_pollThread.join();
    if (m_sendThread.joinable())
        m_sendThread.join();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_healthy = false;
    m_activePoll.reset();
    m_activeSend.reset();
    m_enabled = false;
}

bool Gateway::IsConnected() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_enabled && m_healthy;
}

void Gateway::MarkHealthy()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_healthy = true;
}

void Gateway::MarkUnhealthy(const std::string& error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_healthy = false;
    m_lastError = error;
}

bool Gateway::SleepFor(int seconds)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_wake.wait_for(lock, std::chrono::seconds(seconds),
                    [this] { return m_stop; });
    return !m_stop;
}

// ---------------------------------------------------------------------
// poll worker: GET {base}/poll?timeout=N  ->  inbound commands
// ---------------------------------------------------------------------

void Gateway::PollLoop()
{
    int backoff = kInitialBackoffSeconds;
    const std::string path =
        "/poll?timeout=" + std::to_string(kPollHoldSeconds);

    for (;;)
    {
        std::shared_ptr<Http::Client> client(MakeClient().release());
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stop)
                return;
            m_activePoll = client;
        }

        const Http::Response r =
            client->Get(m_url, path, m_token, kPollTimeoutMs);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_activePoll.reset();
            if (m_stop)
                return;
        }

        if (!r.ok)
        {
            MarkUnhealthy("poll: " + r.error);
            if (!SleepFor(backoff))
                return;
            backoff = backoff * 2 > kMaxBackoffSeconds ? kMaxBackoffSeconds
                                                       : backoff * 2;
            continue;
        }

        if (IsAuthStatus(r.status))
        {
            MarkUnhealthy("poll: HTTP " + std::to_string(r.status) +
                          " - check the auth token");
            if (!SleepFor(kMaxBackoffSeconds))
                return;
            continue;
        }

        if (r.status == 204)
        {
            // Hold expired with nothing to deliver - poll again.
            MarkHealthy();
            backoff = kInitialBackoffSeconds;
            continue;
        }

        if (r.status != 200)
        {
            MarkUnhealthy("poll: HTTP " + std::to_string(r.status));
            if (!SleepFor(backoff))
                return;
            backoff = backoff * 2 > kMaxBackoffSeconds ? kMaxBackoffSeconds
                                                       : backoff * 2;
            continue;
        }

        // 200: {"commands":[ ... ]} (bare arrays are accepted too)
        backoff = kInitialBackoffSeconds;
        const json body = json::parse(r.body, nullptr, false);
        const json* commands = nullptr;
        if (body.is_array())
            commands = &body;
        else if (body.is_object() && body.contains("commands") &&
                 body.at("commands").is_array())
            commands = &body.at("commands");

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_healthy = true;
            if (commands)
            {
                for (const json& c : *commands)
                {
                    if (!c.is_object())
                        continue;
                    m_inbound.push_back(c.dump());
                    ++m_received;
                }
            }
            else if (!r.body.empty())
            {
                m_lastError = "poll: response body is not a command list";
            }
        }
    }
}

// ---------------------------------------------------------------------
// send worker: outbound queue -> POST {base}/messages
// ---------------------------------------------------------------------

void Gateway::SendLoop()
{
    int backoff = kInitialBackoffSeconds;

    for (;;)
    {
        // Wait until there is something to send (or we are stopping).
        std::vector<std::string> batch;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait_for(lock, std::chrono::milliseconds(250), [this] {
                return m_stop || !m_outbound.empty();
            });
            if (m_stop)
                return;
            size_t bytes = 0;
            while (!m_outbound.empty() && batch.size() < kMaxBatchMessages &&
                   bytes < kMaxBatchBytes)
            {
                bytes += m_outbound.front().size();
                batch.push_back(std::move(m_outbound.front()));
                m_outbound.pop_front();
            }
        }
        if (batch.empty())
            continue;

        json payload = json::object();
        json arr = json::array();
        for (const std::string& message : batch)
        {
            json m = json::parse(message, nullptr, false);
            if (!m.is_discarded())
                arr.push_back(std::move(m));
        }
        payload["messages"] = std::move(arr);
        const std::string body = payload.dump();

        std::shared_ptr<Http::Client> client(MakeClient().release());
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stop)
                return;
            m_activeSend = client;
        }

        const Http::Response r =
            client->Post(m_url, "/messages", body, m_token, kPostTimeoutMs);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_activeSend.reset();
            if (m_stop)
                return;
        }

        if (r.ok && r.status >= 200 && r.status < 300)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_healthy = true;
                m_sent += static_cast<unsigned long>(batch.size());
            }
            backoff = kInitialBackoffSeconds;
            continue;
        }

        // Failure: the batch is dropped (events heal via the snapshot the
        // plugin sends when we become healthy again).
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_dropped += static_cast<unsigned long>(batch.size());
        }
        if (!r.ok)
            MarkUnhealthy("send: " + r.error);
        else
            MarkUnhealthy("send: HTTP " + std::to_string(r.status) +
                          (IsAuthStatus(r.status) ? " - check the auth token" : ""));

        const int wait = IsAuthStatus(r.status) ? kMaxBackoffSeconds : backoff;
        if (!SleepFor(wait))
            return;
        backoff = backoff * 2 > kMaxBackoffSeconds ? kMaxBackoffSeconds
                                                   : backoff * 2;
    }
}

// ---------------------------------------------------------------------
// main-thread API
// ---------------------------------------------------------------------

std::vector<std::string> Gateway::Tick(bool& justConnected)
{
    justConnected = false;
    std::vector<std::string> commands;

    bool healthy = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        healthy = m_enabled && m_healthy;
        commands.assign(m_inbound.begin(), m_inbound.end());
        m_inbound.clear();
    }

    if (healthy && !m_connectAnnounced)
    {
        m_connectAnnounced = true;
        justConnected = true;
    }
    else if (!healthy)
    {
        m_connectAnnounced = false;
    }
    return commands;
}

void Gateway::Send(const std::string& message)
{
    if (!m_enabled)
        return;  // silently ignore when the gateway is off
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_healthy || m_outbound.size() >= kMaxQueued)
        {
            ++m_dropped;
            return;
        }
        m_outbound.push_back(message);
    }
    m_wake.notify_all();
}

Gateway::Status Gateway::GetStatus() const
{
    Status s;
    std::lock_guard<std::mutex> lock(m_mutex);
    s.enabled = m_enabled;
    s.url = m_urlString.empty() ? "(not set)" : m_urlString;
    s.lastError = m_lastError;
    s.sent = m_sent;
    s.received = m_received;
    s.dropped = m_dropped;
    if (!m_enabled)
        s.state = "disabled";
    else if (m_healthy)
        s.state = "connected";
    else
        s.state = s.lastError.empty() ? "connecting" : "retrying";
    return s;
}
