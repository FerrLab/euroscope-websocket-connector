#pragma once

// Gateway — the connection manager between the plugin and the WebSocket
// gateway. Owns the WsClient (one instance per connection attempt) and
// drives reconnection with exponential backoff.
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

#include "ws/WsClient.h"

class Gateway
{
public:
    struct Status
    {
        bool enabled = false;
        std::string url;
        std::string state;      // "disabled", "connecting", "connected",
                                // "waiting to reconnect (Ns)"
        std::string lastError;  // most recent connection failure, if any
        unsigned long sent = 0;
        unsigned long received = 0;
        unsigned long dropped = 0;  // messages dropped while not connected
    };

    // Validates and stores the gateway URL. Returns an error string, or
    // empty on success. Changing the URL while connected drops the
    // connection (reconnects on the next tick if enabled).
    std::string SetUrl(const std::string& url);
    std::string GetUrl() const { return m_urlString; }
    bool HasValidUrl() const { return m_url.ok; }

    // Enable = connect now and keep reconnecting; Disable = drop and stay
    // offline.
    std::string Enable();
    void Disable();
    bool IsEnabled() const { return m_enabled; }
    bool IsConnected() const;

    // Call once per second (from OnTimer). Returns all messages received
    // from the gateway since the last tick; sets `justConnected` on the
    // tick the connection came up (the caller sends the session snapshot).
    std::vector<std::string> Tick(bool& justConnected);

    // Queues a message when connected; silently drops (and counts) it
    // otherwise - consumers recover state from the snapshot on reconnect.
    void Send(const std::string& message);

    Status GetStatus() const;

private:
    static const int kInitialRetrySeconds = 2;
    static const int kMaxRetrySeconds = 60;

    Ws::Url m_url;
    std::string m_urlString;
    bool m_enabled = false;
    bool m_wasConnected = false;

    int m_retryDelay = kInitialRetrySeconds;
    int m_retryCountdown = 0;

    std::string m_lastError;
    unsigned long m_sent = 0;
    unsigned long m_received = 0;
    unsigned long m_dropped = 0;

    std::unique_ptr<Ws::WsClient> m_client;
};
