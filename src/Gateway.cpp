#include "Gateway.h"

bool Gateway::ProcessInbound(std::vector<std::string> raw,
                             std::vector<std::string>& commands,
                             bool connectionAlive)
{
    if (m_mode != GatewayMode::Pusher)
    {
        commands = std::move(raw);
        return false;
    }
    if (!m_session)
        return false;

    bool fatal = false;
    for (const std::string& frame : raw)
    {
        Pusher::PusherSession::Incoming in = m_session->HandleFrame(frame);
        if (connectionAlive)
            for (const std::string& reply : in.sendNow)
                m_client->Send(reply);
        for (std::string& command : in.commands)
            commands.push_back(std::move(command));
        if (!in.notice.empty())
            m_lastError = in.notice;
        fatal = fatal || in.fatal;
    }
    return fatal;
}

void Gateway::ResetConnection()
{
    m_client.reset();
    m_session.reset();
    m_wasConnected = false;
    m_retryDelay = kInitialRetrySeconds;
    m_retryCountdown = 0;
}

void Gateway::ScheduleRetry(int seconds)
{
    m_client.reset();
    m_session.reset();
    m_wasConnected = false;
    m_retryCountdown = seconds;
}

std::string Gateway::SetUrl(const std::string& url)
{
    const Ws::Url parsed = Ws::ParseUrl(url);
    if (!parsed.ok)
        return parsed.error;

    m_url = parsed;
    m_urlString = url;
    ResetConnection();
    return std::string();
}

void Gateway::SetMode(GatewayMode mode)
{
    if (m_mode == mode)
        return;
    m_mode = mode;
    ResetConnection();
}

void Gateway::SetPusherKey(const std::string& key)
{
    m_pusherConfig.appKey = key;
    if (m_mode == GatewayMode::Pusher)
        ResetConnection();
}

void Gateway::SetPusherSecret(const std::string& secret)
{
    m_pusherConfig.secret = secret;
    if (m_mode == GatewayMode::Pusher)
        ResetConnection();
}

void Gateway::SetPusherChannel(const std::string& channel)
{
    m_pusherConfig.channel = channel;
    if (m_mode == GatewayMode::Pusher)
        ResetConnection();
}

void Gateway::SetClientVersion(const std::string& version)
{
    m_pusherConfig.clientVersion = version;
}

std::string Gateway::Enable()
{
    if (!m_url.ok)
        return "no gateway URL configured - use: .wsc gateway url ws://host:port/";
    if (m_mode == GatewayMode::Pusher)
    {
        if (m_pusherConfig.appKey.empty())
            return "pusher mode needs an app key - use: .wsc gateway key <app-key>";
        const bool isPrivate =
            m_pusherConfig.channel.rfind("private-", 0) == 0 ||
            m_pusherConfig.channel.rfind("presence-", 0) == 0;
        if (isPrivate && m_pusherConfig.secret.empty())
            return "channel '" + m_pusherConfig.channel +
                   "' needs token auth - use: .wsc gateway secret <app-secret> "
                   "(or use a public channel name)";
    }
    m_enabled = true;
    m_retryCountdown = 0;  // connect on the next tick
    m_retryDelay = kInitialRetrySeconds;
    return std::string();
}

void Gateway::Disable()
{
    m_enabled = false;
    m_client.reset();
    m_session.reset();
    m_wasConnected = false;
}

bool Gateway::IsConnected() const
{
    if (!m_client || m_client->State() != Ws::ClientState::Connected)
        return false;
    if (m_mode == GatewayMode::Pusher)
        return m_session && m_session->IsSubscribed();
    return true;
}

std::vector<std::string> Gateway::Tick(bool& justConnected)
{
    justConnected = false;

    if (!m_enabled)
    {
        m_client.reset();
        m_session.reset();
        m_wasConnected = false;
        return {};
    }

    if (m_client)
    {
        switch (m_client->State())
        {
            case Ws::ClientState::Connected:
            {
                std::vector<std::string> commands;
                const bool fatal =
                    ProcessInbound(m_client->TakeReceived(), commands,
                                   true /*connection alive*/);
                if (fatal)
                {
                    // Bad key/app/quota: retrying the same config fast is
                    // pointless - back off to the maximum.
                    ScheduleRetry(kMaxRetrySeconds);
                    m_retryDelay = kMaxRetrySeconds;
                    m_received += static_cast<unsigned long>(commands.size());
                    return commands;
                }

                if (IsConnected() && !m_wasConnected)
                {
                    m_wasConnected = true;
                    m_retryDelay = kInitialRetrySeconds;
                    justConnected = true;
                }
                m_received += static_cast<unsigned long>(commands.size());
                return commands;
            }
            case Ws::ClientState::Connecting:
                return {};
            case Ws::ClientState::Closed:
            {
                // Drain what arrived before the close FIRST - the peer's
                // final messages often explain the close (e.g. a fatal
                // pusher:error just before the server drops us).
                m_lastError = m_client->LastError();
                std::vector<std::string> commands;
                const bool fatal =
                    ProcessInbound(m_client->TakeReceived(), commands,
                                   false /*connection gone*/);
                m_received += static_cast<unsigned long>(commands.size());

                ScheduleRetry(fatal ? kMaxRetrySeconds : m_retryDelay);
                m_retryDelay = fatal ? kMaxRetrySeconds
                               : m_retryDelay * 2 > kMaxRetrySeconds
                                   ? kMaxRetrySeconds
                                   : m_retryDelay * 2;
                return commands;
            }
        }
    }

    // No client: wait out the backoff, then start a new attempt.
    if (m_retryCountdown > 0)
    {
        --m_retryCountdown;
        return {};
    }

    if (m_mode == GatewayMode::Pusher)
    {
        m_session.reset(new Pusher::PusherSession(m_pusherConfig));
        Ws::Url url = m_url;
        url.path = m_session->ConnectionPath();
        m_client.reset(new Ws::WsClient(url));
    }
    else
    {
        m_client.reset(new Ws::WsClient(m_url));
    }
    return {};
}

void Gateway::Send(const std::string& message)
{
    if (!m_enabled)
        return;  // silently ignore when the gateway is off
    if (!IsConnected())
    {
        ++m_dropped;
        return;
    }
    const std::string wire = m_mode == GatewayMode::Pusher
                                 ? m_session->WrapOutgoing(message)
                                 : message;
    if (!m_client->Send(wire))
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
    s.mode = m_mode == GatewayMode::Pusher ? "pusher" : "raw";
    s.url = m_urlString.empty() ? "(not set)" : m_urlString;
    s.channel = m_mode == GatewayMode::Pusher ? m_pusherConfig.channel : "";
    s.lastError = m_lastError;
    s.sent = m_sent;
    s.received = m_received;
    s.dropped = m_dropped;

    if (!m_enabled)
        s.state = "disabled";
    else if (IsConnected())
        s.state = "connected";
    else if (m_client && m_client->State() == Ws::ClientState::Connected)
        s.state = "subscribing";  // pusher handshake in progress
    else if (m_client)
        s.state = "connecting";
    else
        s.state = "waiting to reconnect (" + std::to_string(m_retryCountdown) + "s)";
    return s;
}
