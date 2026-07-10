#pragma once

// WsClient — one WebSocket connection on its own thread (Win32/WinSock).
//
// Lifecycle: the constructor starts the thread (state Connecting); the
// thread resolves/connects/handshakes and then pumps frames until Stop()
// is called, the peer closes, or an error occurs (state Closed, LastError
// set). One WsClient = one connection attempt — reconnecting means
// destroying this instance and creating a new one (Gateway does that,
// with backoff).
//
// Thread-safety: Send(), TakeReceived(), State() and LastError() may be
// called from any thread (the plugin calls them from the EuroScope main
// thread). The socket thread NEVER touches the EuroScope API.
//
// Protocol notes:
//   * text frames only (one JSON message per frame), ws:// only (no TLS)
//   * replies to server Pings with Pongs; sends its own Ping every ~30 s
//   * outbound queue is bounded (kMaxQueued) — oldest messages are
//     dropped first if the peer cannot keep up

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "WsFrame.h"
#include "WsHandshake.h"

namespace Ws
{
    enum class ClientState
    {
        Connecting,
        Connected,
        Closed,
    };

    class WsClient
    {
    public:
        static const size_t kMaxQueued = 5000;

        explicit WsClient(const Url& url);
        ~WsClient();  // Stop() + join

        WsClient(const WsClient&) = delete;
        WsClient& operator=(const WsClient&) = delete;

        void Stop();

        ClientState State() const { return m_state.load(); }
        std::string LastError() const;

        // Queues one text message for sending. Returns false when the
        // queue is full (message dropped).
        bool Send(const std::string& text);

        // Drains all received text messages.
        std::vector<std::string> TakeReceived();

    private:
        void Run();
        void Fail(const std::string& error);
        bool SendAll(const uint8_t* data, size_t len);
        bool SendFrame(Opcode opcode, const uint8_t* payload, size_t len);
        uint32_t NextRandom();

        Url m_url;
        std::atomic<ClientState> m_state{ ClientState::Connecting };
        std::atomic<bool> m_stop{ false };

        // Socket handle, kept as uintptr so this header stays free of
        // winsock includes. Guarded by m_socketMutex for the Stop() race.
        std::mutex m_socketMutex;
        uintptr_t m_socket;

        mutable std::mutex m_errorMutex;
        std::string m_lastError;

        std::mutex m_outMutex;
        std::deque<std::string> m_outbound;

        std::mutex m_inMutex;
        std::deque<std::string> m_inbound;

        uint32_t m_rngState;

        std::thread m_thread;  // must be the last member (starts in ctor)
    };
}
