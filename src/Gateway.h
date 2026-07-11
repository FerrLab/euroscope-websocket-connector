#pragma once

// Gateway — the connection manager between the plugin and the WebSocket
// gateway. Owns the WsClient (one instance per connection attempt) and
// drives reconnection with exponential backoff.
//
// Two wire modes:
//   * Raw    — contract messages go directly over the socket (our own
//              gateway implementations).
//   * Pusher — the connection speaks the Pusher Channels protocol
//              (Laravel Reverb, Soketi, ...): connect to /app/{key},
//              subscribe to a channel with a locally-minted auth token,
//              and ride contract messages as "client-euroscope" events.
//              See src/pusher/PusherSession.h and docs/PROTOCOL.md.
//
// All methods are called from the EuroScope MAIN thread only (commands and
// OnTimer). The socket thread lives inside WsClient and communicates
// through its thread-safe queues; nothing here needs a lock.
//
// Data flow per OnTimer tick (1 Hz):
//   plugin -> Tick() -> { justConnected?, inbound command strings }
//   plugin runs commands through JsonApi and calls Send() for responses;
//   event callbacks call Send() as events happen.

#include <memory>
#include <string>
#include <vector>

#include "pusher/PusherSession.h"
#include "ws/WsClient.h"

enum class GatewayMode
{
    Raw,
    Pusher,
};

class Gateway
{
public:
    struct Status
    {
        bool enabled = false;
        std::string mode;       // "raw" | "pusher"
        std::string url;
        std::string channel;    // pusher mode only
        std::string state;      // "disabled", "connecting", "subscribing",
                                // "connected", "waiting to reconnect (Ns)"
        std::string lastError;  // most recent connection failure, if any
        unsigned long sent = 0;
        unsigned long received = 0;
        unsigned long dropped = 0;  // messages dropped while not connected
    };

    // Validates and stores the gateway URL. Returns an error string, or
    // empty on success. Changing any connection setting while connected
    // drops the connection (reconnects on the next tick if enabled).
    std::string SetUrl(const std::string& url);
    std::string GetUrl() const { return m_urlString; }
    bool HasValidUrl() const { return m_url.ok; }

    void SetMode(GatewayMode mode);
    GatewayMode Mode() const { return m_mode; }

    // Pusher-mode settings (persisted by the plugin).
    void SetPusherKey(const std::string& key);
    void SetPusherSecret(const std::string& secret);
    void SetPusherChannel(const std::string& channel);
    void SetClientVersion(const std::string& version);
    const Pusher::Config& PusherConf() const { return m_pusherConfig; }

    // Enable = connect now and keep reconnecting; Disable = drop and stay
    // offline. Enable validates the configuration for the current mode.
    std::string Enable();
    void Disable();
    bool IsEnabled() const { return m_enabled; }

    // Raw mode: transport up. Pusher mode: transport up AND subscribed.
    bool IsConnected() const;

    // Call once per second (from OnTimer). Returns all contract messages
    // received from the gateway since the last tick; sets `justConnected`
    // on the tick the connection became usable (the caller sends the
    // snapshot).
    std::vector<std::string> Tick(bool& justConnected);

    // Queues a contract message when connected (wrapping it as a Pusher
    // client event in pusher mode); silently drops (and counts) it
    // otherwise - consumers recover state from the snapshot on reconnect.
    void Send(const std::string& message);

    Status GetStatus() const;

private:
    static const int kInitialRetrySeconds = 2;
    static const int kMaxRetrySeconds = 60;

    Ws::Url m_url;
    std::string m_urlString;
    GatewayMode m_mode = GatewayMode::Raw;
    Pusher::Config m_pusherConfig;

    bool m_enabled = false;
    bool m_wasConnected = false;

    int m_retryDelay = kInitialRetrySeconds;
    int m_retryCountdown = 0;

    std::string m_lastError;
    unsigned long m_sent = 0;
    unsigned long m_received = 0;
    unsigned long m_dropped = 0;

    std::unique_ptr<Ws::WsClient> m_client;
    std::unique_ptr<Pusher::PusherSession> m_session;  // pusher mode only

    // Drops the connection and restarts the (re)connect cycle on the next
    // tick; used when settings change.
    void ResetConnection();
    void ScheduleRetry(int seconds);

    // Runs received frames through the Pusher session (or passes them
    // through in raw mode), appending contract messages to `commands`.
    // Returns true when a fatal Pusher error was seen.
    bool ProcessInbound(std::vector<std::string> raw,
                        std::vector<std::string>& commands,
                        bool connectionAlive);
};
