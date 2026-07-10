#pragma once

// RFC 6455 frame codec + message assembler — the PURE part of the
// WebSocket implementation: no sockets, no Windows headers, fully
// unit-tested in tests/ws_test.cpp.
//
// Client rules implemented here:
//   * outgoing frames are always masked (the caller supplies the mask so
//     the codec stays deterministic and testable)
//   * incoming frames may be fragmented; MessageAssembler reassembles
//   * control frames (ping/pong/close) are surfaced to the caller

#include <cstdint>
#include <string>
#include <vector>

namespace Ws
{
    enum class Opcode : uint8_t
    {
        Continuation = 0x0,
        Text = 0x1,
        Binary = 0x2,
        Close = 0x8,
        Ping = 0x9,
        Pong = 0xA,
    };

    struct Frame
    {
        bool fin = true;
        Opcode opcode = Opcode::Text;
        std::vector<uint8_t> payload;
    };

    // Upper bound for a single message/frame we are willing to buffer.
    // Anything larger is treated as a protocol error (defensive: keeps a
    // misbehaving peer from ballooning EuroScope's 32-bit address space).
    const size_t kMaxPayload = 16 * 1024 * 1024;

    // Encodes one frame, masked with `mask` (client->server direction).
    std::vector<uint8_t> EncodeFrame(Opcode opcode,
                                     const uint8_t* payload, size_t len,
                                     const uint8_t mask[4],
                                     bool fin = true);

    // Convenience for text messages.
    std::vector<uint8_t> EncodeText(const std::string& text, const uint8_t mask[4]);

    enum class DecodeResult
    {
        NeedMore,  // incomplete frame, feed more bytes
        Ok,        // `out` filled, `consumed` bytes eaten
        Error,     // protocol violation, close the connection
    };

    // Tries to decode one frame from the front of [data, data+len).
    // Handles masked and unmasked frames (servers send unmasked).
    DecodeResult TryDecodeFrame(const uint8_t* data, size_t len,
                                Frame& out, size_t& consumed,
                                std::string& error);

    // Feeds frames, emits complete messages / control events.
    class MessageAssembler
    {
    public:
        enum class Event
        {
            None,     // frame absorbed, nothing complete yet
            Message,  // a complete text/binary message is in `message`
            Ping,     // respond with a Pong carrying `control`
            Pong,     // keepalive answer, usually ignored
            Close,    // peer wants to close; `control` = code+reason
            Error,    // protocol violation in `error`
        };

        Event Feed(const Frame& frame);

        std::string message;            // valid after Event::Message
        std::vector<uint8_t> control;   // valid after Ping/Pong/Close
        std::string error;              // valid after Event::Error

    private:
        std::vector<uint8_t> m_buffer;  // fragmented message in progress
        bool m_assembling = false;
    };
}
