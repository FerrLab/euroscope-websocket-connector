#pragma once

// PusherSession — the Pusher Channels protocol (protocol 7) as a PURE
// state machine layered on top of the raw WebSocket connection. Works
// against pusher.com-compatible servers: Laravel Reverb, Soketi, and
// Pusher itself (the latter needs wss://, which the transport doesn't
// support yet).
//
// Flow (docs/PROTOCOL.md "Pusher transport"):
//
//   TCP/WS connect to  ws://host:port/app/{APP_KEY}?protocol=7&...
//     <- pusher:connection_established   {socket_id, activity_timeout}
//     -> pusher:subscribe                {channel, auth}
//     <- pusher_internal:subscription_succeeded
//   then: contract messages ride as client events on the channel
//     -> {"event":"client-euroscope","channel":C,"data":"<contract JSON>"}
//     <- same shape for inbound commands
//
// Token authentication: private channels ("private-...") require an auth
// token: "<app_key>:<hex HMAC-SHA256(secret, socket_id + \":\" + channel)>".
// This session computes it locally from the configured app secret — the
// exact token a Pusher auth endpoint would mint. Public channels (no
// "private-" prefix) skip the token.
//
// This class does no I/O: WsClient bytes are fed into HandleFrame(),
// which returns frames to send and contract commands to execute. Fully
// unit-tested in tests/pusher_test.cpp.

#include <string>
#include <vector>

namespace Pusher
{
    struct Config
    {
        std::string appKey;
        std::string secret;   // app secret; needed for private- channels
        std::string channel = "private-euroscope";
        std::string clientVersion;  // reported in the connect URL
    };

    // Pusher event name our contract messages ride on (both directions).
    // Client events must be prefixed "client-" per the protocol.
    extern const char* kContractEventName;

    class PusherSession
    {
    public:
        explicit PusherSession(const Config& config) : m_config(config) {}

        // Path + query for the WebSocket URL (host/port come from the
        // gateway URL setting).
        std::string ConnectionPath() const;

        // Result of processing one inbound WebSocket text message.
        struct Incoming
        {
            std::vector<std::string> sendNow;   // raw frames to send back
            std::vector<std::string> commands;  // contract messages (from client events)
            bool becameSubscribed = false;      // subscription just succeeded
            std::string notice;                 // human-readable info (errors etc.)
            bool fatal = false;                 // server rejected us; reconnecting
                                                // without a config change is pointless
        };
        Incoming HandleFrame(const std::string& rawFrame);

        // Wraps one contract message as a client event for the channel.
        // Only meaningful once IsSubscribed().
        std::string WrapOutgoing(const std::string& contractJson) const;

        bool IsSubscribed() const { return m_subscribed; }
        std::string SocketId() const { return m_socketId; }

        // Exposed for tests: the exact auth token sent in pusher:subscribe.
        static std::string AuthToken(const Config& config,
                                     const std::string& socketId);

    private:
        Config m_config;
        bool m_subscribed = false;
        std::string m_socketId;

        std::string BuildSubscribe() const;
    };
}
