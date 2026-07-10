#include "WsHandshake.h"

#include <algorithm>
#include <cctype>

namespace Ws
{
    // -------------------------------------------------------------------
    // SHA-1 (RFC 3174) — needed only for the handshake accept key. Small,
    // self-contained implementation; verified against the RFC 6455 sample
    // handshake in tests/ws_test.cpp.
    // -------------------------------------------------------------------

    namespace detail
    {
        namespace
        {
            inline uint32_t Rol(uint32_t v, int bits)
            {
                return (v << bits) | (v >> (32 - bits));
            }
        }

        std::vector<uint8_t> Sha1(const uint8_t* data, size_t len)
        {
            uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu,
                              0x10325476u, 0xC3D2E1F0u };

            // message + 0x80 + zero pad + 64-bit big-endian bit length
            std::vector<uint8_t> msg(data, data + len);
            msg.push_back(0x80);
            while (msg.size() % 64 != 56)
                msg.push_back(0x00);
            const uint64_t bits = static_cast<uint64_t>(len) * 8;
            for (int i = 7; i >= 0; --i)
                msg.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));

            for (size_t chunk = 0; chunk < msg.size(); chunk += 64)
            {
                uint32_t w[80];
                for (int i = 0; i < 16; ++i)
                    w[i] = (static_cast<uint32_t>(msg[chunk + i * 4]) << 24) |
                           (static_cast<uint32_t>(msg[chunk + i * 4 + 1]) << 16) |
                           (static_cast<uint32_t>(msg[chunk + i * 4 + 2]) << 8) |
                           static_cast<uint32_t>(msg[chunk + i * 4 + 3]);
                for (int i = 16; i < 80; ++i)
                    w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

                uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
                for (int i = 0; i < 80; ++i)
                {
                    uint32_t f, k;
                    if (i < 20)
                    {
                        f = (b & c) | ((~b) & d);
                        k = 0x5A827999u;
                    }
                    else if (i < 40)
                    {
                        f = b ^ c ^ d;
                        k = 0x6ED9EBA1u;
                    }
                    else if (i < 60)
                    {
                        f = (b & c) | (b & d) | (c & d);
                        k = 0x8F1BBCDCu;
                    }
                    else
                    {
                        f = b ^ c ^ d;
                        k = 0xCA62C1D6u;
                    }
                    const uint32_t tmp = Rol(a, 5) + f + e + k + w[i];
                    e = d;
                    d = c;
                    c = Rol(b, 30);
                    b = a;
                    a = tmp;
                }
                h[0] += a;
                h[1] += b;
                h[2] += c;
                h[3] += d;
                h[4] += e;
            }

            std::vector<uint8_t> digest(20);
            for (int i = 0; i < 5; ++i)
            {
                digest[i * 4] = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
                digest[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
                digest[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xFF);
                digest[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xFF);
            }
            return digest;
        }

        std::string Base64(const uint8_t* data, size_t len)
        {
            static const char* kAlphabet =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string out;
            out.reserve(((len + 2) / 3) * 4);
            for (size_t i = 0; i < len; i += 3)
            {
                const uint32_t b0 = data[i];
                const uint32_t b1 = i + 1 < len ? data[i + 1] : 0;
                const uint32_t b2 = i + 2 < len ? data[i + 2] : 0;
                const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
                out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
                out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
                out.push_back(i + 1 < len ? kAlphabet[(triple >> 6) & 0x3F] : '=');
                out.push_back(i + 2 < len ? kAlphabet[triple & 0x3F] : '=');
            }
            return out;
        }
    }

    // -------------------------------------------------------------------
    // URL
    // -------------------------------------------------------------------

    Url ParseUrl(const std::string& url)
    {
        Url out;
        const std::string kWs = "ws://";
        const std::string kWss = "wss://";

        if (url.compare(0, kWss.size(), kWss) == 0)
        {
            out.error = "wss:// (TLS) is not supported yet - use ws:// "
                        "(gateway on localhost/LAN)";
            return out;
        }
        if (url.compare(0, kWs.size(), kWs) != 0)
        {
            out.error = "URL must start with ws:// (e.g. ws://127.0.0.1:3000/)";
            return out;
        }

        std::string rest = url.substr(kWs.size());
        const size_t slash = rest.find('/');
        if (slash != std::string::npos)
        {
            out.path = rest.substr(slash);
            rest = rest.substr(0, slash);
        }

        const size_t colon = rest.find(':');
        if (colon != std::string::npos)
        {
            out.host = rest.substr(0, colon);
            out.port = rest.substr(colon + 1);
            if (out.port.empty() ||
                out.port.find_first_not_of("0123456789") != std::string::npos)
            {
                out.error = "invalid port in URL";
                return out;
            }
        }
        else
        {
            out.host = rest;
        }

        if (out.host.empty())
        {
            out.error = "missing host in URL";
            return out;
        }
        out.ok = true;
        return out;
    }

    // -------------------------------------------------------------------
    // handshake
    // -------------------------------------------------------------------

    std::string MakeKey(const uint8_t random16[16])
    {
        return detail::Base64(random16, 16);
    }

    std::string BuildRequest(const Url& url, const std::string& key)
    {
        std::string hostHeader = url.host;
        if (url.port != "80")
            hostHeader += ":" + url.port;

        std::string r;
        r += "GET " + url.path + " HTTP/1.1\r\n";
        r += "Host: " + hostHeader + "\r\n";
        r += "Upgrade: websocket\r\n";
        r += "Connection: Upgrade\r\n";
        r += "Sec-WebSocket-Key: " + key + "\r\n";
        r += "Sec-WebSocket-Version: 13\r\n";
        r += "\r\n";
        return r;
    }

    std::string ExpectedAccept(const std::string& key)
    {
        // Fixed GUID from RFC 6455 §1.3.
        const std::string material = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        const std::vector<uint8_t> digest = detail::Sha1(
            reinterpret_cast<const uint8_t*>(material.data()), material.size());
        return detail::Base64(digest.data(), digest.size());
    }

    namespace
    {
        std::string LowerCopy(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return s;
        }

        std::string Trim(const std::string& s)
        {
            size_t a = 0, b = s.size();
            while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
                ++a;
            while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
                --b;
            return s.substr(a, b - a);
        }
    }

    HandshakeResult ParseResponse(const std::string& received,
                                  const std::string& key,
                                  size_t& consumed,
                                  std::string& error)
    {
        const size_t end = received.find("\r\n\r\n");
        if (end == std::string::npos)
        {
            if (received.size() > 16 * 1024)
            {
                error = "handshake response too large";
                return HandshakeResult::Error;
            }
            return HandshakeResult::NeedMore;
        }
        consumed = end + 4;
        const std::string header = received.substr(0, end);

        // Status line: HTTP/1.1 101 ...
        const size_t lineEnd = header.find("\r\n");
        const std::string statusLine =
            lineEnd == std::string::npos ? header : header.substr(0, lineEnd);
        if (statusLine.find(" 101") == std::string::npos)
        {
            error = "server did not switch protocols: " + statusLine;
            return HandshakeResult::Error;
        }

        // Headers (case-insensitive names)
        std::string upgrade, connection, accept;
        size_t pos = lineEnd == std::string::npos ? header.size() : lineEnd + 2;
        while (pos < header.size())
        {
            size_t next = header.find("\r\n", pos);
            if (next == std::string::npos)
                next = header.size();
            const std::string line = header.substr(pos, next - pos);
            pos = next + 2;

            const size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            const std::string name = LowerCopy(Trim(line.substr(0, colon)));
            const std::string value = Trim(line.substr(colon + 1));
            if (name == "upgrade")
                upgrade = LowerCopy(value);
            else if (name == "connection")
                connection = LowerCopy(value);
            else if (name == "sec-websocket-accept")
                accept = value;
        }

        if (upgrade != "websocket")
        {
            error = "missing/invalid Upgrade header";
            return HandshakeResult::Error;
        }
        if (connection.find("upgrade") == std::string::npos)
        {
            error = "missing/invalid Connection header";
            return HandshakeResult::Error;
        }
        if (accept != ExpectedAccept(key))
        {
            error = "Sec-WebSocket-Accept mismatch";
            return HandshakeResult::Error;
        }
        return HandshakeResult::Ok;
    }
}
