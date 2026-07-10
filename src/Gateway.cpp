#include "Gateway.h"

std::string Gateway::SetUrl(const std::string& url)
{
    const Ws::Url parsed = Ws::ParseUrl(url);
    if (!parsed.ok)
        return parsed.error;

    m_url = parsed;
    m_urlString = url;

    // Force a clean reconnect against the new target.
    m_client.reset();
    m_wasConnected = false;
    m_retryDelay = kInitialRetrySeconds;
    m_retryCountdown = 0;
    return std::string();
}

std::string Gateway::Enable()
{
    if (!m_url.ok)
        return "no gateway URL configured - use: .wsc gateway url ws://host:port/";
    m_enabled = true;
    m_retryCountdown = 0;  // connect on the next tick
    m_retryDelay = kInitialRetrySeconds;
    return std::string();
}

void Gateway::Disable()
{
    m_enabled = false;
    m_client.reset();
    m_wasConnected = false;
}

bool Gateway::IsConnected() const
{
    return m_client && m_client->State() == Ws::ClientState::Connected;
}

std::vector<std::string> Gateway::Tick(bool& justConnected)
{
    justConnected = false;

    if (!m_enabled)
    {
        m_client.reset();
        m_wasConnected = false;
        return {};
    }

    if (m_client)
    {
        switch (m_client->State())
        {
            case Ws::ClientState::Connected:
            {
                if (!m_wasConnected)
                {
                    m_wasConnected = true;
                    m_retryDelay = kInitialRetrySeconds;
                    justConnected = true;
                }
                std::vector<std::string> inbound = m_client->TakeReceived();
                m_received += static_cast<unsigned long>(inbound.size());
                return inbound;
            }
            case Ws::ClientState::Connecting:
                return {};
            case Ws::ClientState::Closed:
                m_lastError = m_client->LastError();
                m_client.reset();
                m_wasConnected = false;
                m_retryCountdown = m_retryDelay;
                m_retryDelay = m_retryDelay * 2 > kMaxRetrySeconds
                                   ? kMaxRetrySeconds
                                   : m_retryDelay * 2;
                return {};
        }
    }

    // No client: wait out the backoff, then start a new attempt.
    if (m_retryCountdown > 0)
    {
        --m_retryCountdown;
        return {};
    }
    m_client.reset(new Ws::WsClient(m_url));
    return {};
}

void Gateway::Send(const std::string& message)
{
    if (!m_enabled)
        return;  // silently ignore when the gateway is off
    if (!IsConnected() || !m_client->Send(message))
    {
        ++m_dropped;
        return;
    }
    ++m_sent;
}

Gateway::Status Gateway::GetStatus() const
{
    Status s;
    s.enabled = m_enabled;
    s.url = m_urlString.empty() ? "(not set)" : m_urlString;
    s.lastError = m_lastError;
    s.sent = m_sent;
    s.received = m_received;
    s.dropped = m_dropped;

    if (!m_enabled)
        s.state = "disabled";
    else if (IsConnected())
        s.state = "connected";
    else if (m_client)
        s.state = "connecting";
    else
        s.state = "waiting to reconnect (" + std::to_string(m_retryCountdown) + "s)";
    return s;
}
