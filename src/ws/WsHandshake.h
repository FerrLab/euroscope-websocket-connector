#pragma once

// RFC 6455 client handshake + ws:// URL parsing — the PURE part of the
// WebSocket implementation: no sockets, no Windows headers, fully
// unit-tested in tests/ws_test.cpp. The socket thread (WsClient) feeds
// bytes in and out.
//
// Scope (deliberate): client side only, ws:// only (no TLS — the gateway
// is expected on localhost/LAN; wss support would need Schannel/mbedTLS
// and is future work, see docs/PROTOCOL.md "Transport").

#include <cstdint>
#include <string>
#include <vector>

namespace Ws
{
    // --- URL ------------------------------------------------------------

    struct Url
    {
        bool ok = false;
        std::string error;
        std::string host;
        std::string port = "80";
        std::string path = "/";
    };

    // Accepts "ws://host[:port][/path]". Rejects wss:// (unsupported) and
    // anything else with a helpful error.
    Url ParseUrl(const std::string& url);

    // --- handshake --------------------------------------------------------

    // 16 random bytes, base64 — the Sec-WebSocket-Key. The caller provides
    // the randomness (the codec stays deterministic and testable).
    std::string MakeKey(const uint8_t random16[16]);

    // The full HTTP upgrade request to send after the TCP connect.
    std::string BuildRequest(const Url& url, const std::string& key);

    // The Sec-WebSocket-Accept value the server must answer with.
    std::string ExpectedAccept(const std::string& key);

    // Incremental response parser: call with everything received so far.
    //   NeedMore  - header not complete yet, read more bytes
    //   Ok        - valid 101 response; `consumed` = header length in bytes
    //               (bytes after it already belong to the frame stream)
    //   Error     - handshake failed; `error` says why
    enum class HandshakeResult
    {
        NeedMore,
        Ok,
        Error
    };
    HandshakeResult ParseResponse(const std::string& received,
                                  const std::string& key,
                                  size_t& consumed,
                                  std::string& error);

    // --- exposed for unit tests ------------------------------------------

    namespace detail
    {
        std::vector<uint8_t> Sha1(const uint8_t* data, size_t len);
        std::string Base64(const uint8_t* data, size_t len);
    }
}
