// Unit tests for the pure WebSocket parts (src/ws/): handshake, URL
// parsing and the frame codec. Runs on any platform. Vectors come from
// RFC 6455 itself (sample handshake §1.3, sample frames §5.7) and RFC 3174.

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "ws/WsFrame.h"
#include "ws/WsHandshake.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                        \
    do                                                                     \
    {                                                                      \
        ++g_checks;                                                        \
        if (!(cond))                                                       \
        {                                                                  \
            ++g_failures;                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  "    \
                      << #cond << "\n";                                    \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do                                                                     \
    {                                                                      \
        ++g_checks;                                                        \
        if (!((a) == (b)))                                                 \
        {                                                                  \
            ++g_failures;                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  "    \
                      << #a << " == " << #b << "\n";                       \
        }                                                                  \
    } while (0)

static std::string HexDigest(const std::vector<uint8_t>& d)
{
    static const char* k = "0123456789abcdef";
    std::string s;
    for (uint8_t b : d)
    {
        s.push_back(k[b >> 4]);
        s.push_back(k[b & 0xF]);
    }
    return s;
}

int main()
{
    using namespace Ws;

    // --- SHA-1 / Base64 primitives -------------------------------------
    {
        const char* abc = "abc";
        CHECK_EQ(HexDigest(detail::Sha1(reinterpret_cast<const uint8_t*>(abc), 3)),
                 std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
        const char* empty = "";
        CHECK_EQ(HexDigest(detail::Sha1(reinterpret_cast<const uint8_t*>(empty), 0)),
                 std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));

        CHECK_EQ(detail::Base64(reinterpret_cast<const uint8_t*>("Man"), 3), std::string("TWFu"));
        CHECK_EQ(detail::Base64(reinterpret_cast<const uint8_t*>("Ma"), 2), std::string("TWE="));
        CHECK_EQ(detail::Base64(reinterpret_cast<const uint8_t*>("M"), 1), std::string("TQ=="));
    }

    // --- URL parsing -----------------------------------------------------
    {
        Url u = ParseUrl("ws://127.0.0.1:3000/session");
        CHECK(u.ok);
        CHECK_EQ(u.host, std::string("127.0.0.1"));
        CHECK_EQ(u.port, std::string("3000"));
        CHECK_EQ(u.path, std::string("/session"));

        Url d = ParseUrl("ws://gateway.example.com");
        CHECK(d.ok);
        CHECK_EQ(d.port, std::string("80"));
        CHECK_EQ(d.path, std::string("/"));

        CHECK(!ParseUrl("wss://secure.example.com").ok);
        CHECK(!ParseUrl("http://example.com").ok);
        CHECK(!ParseUrl("ws://").ok);
        CHECK(!ParseUrl("ws://host:notaport/").ok);
    }

    // --- handshake (RFC 6455 §1.3 sample) --------------------------------
    {
        const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
        CHECK_EQ(ExpectedAccept(key), std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));

        Url u = ParseUrl("ws://server.example.com/chat");
        const std::string req = BuildRequest(u, key);
        CHECK(req.find("GET /chat HTTP/1.1\r\n") == 0);
        CHECK(req.find("Host: server.example.com\r\n") != std::string::npos);
        CHECK(req.find("Upgrade: websocket\r\n") != std::string::npos);
        CHECK(req.find("Sec-WebSocket-Key: " + key + "\r\n") != std::string::npos);
        CHECK(req.find("Sec-WebSocket-Version: 13\r\n") != std::string::npos);
        CHECK(req.substr(req.size() - 4) == "\r\n\r\n");

        const std::string ok101 =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
            "\r\n";

        size_t consumed = 0;
        std::string error;
        // incremental: partial header first
        CHECK(ParseResponse(ok101.substr(0, 30), key, consumed, error) ==
              HandshakeResult::NeedMore);
        CHECK(ParseResponse(ok101, key, consumed, error) == HandshakeResult::Ok);
        CHECK_EQ(consumed, ok101.size());

        // trailing frame bytes after the header are not consumed
        CHECK(ParseResponse(ok101 + "XYZ", key, consumed, error) == HandshakeResult::Ok);
        CHECK_EQ(consumed, ok101.size());

        // wrong accept
        std::string bad = ok101;
        bad.replace(bad.find("s3pP"), 4, "s3pQ");
        CHECK(ParseResponse(bad, key, consumed, error) == HandshakeResult::Error);

        // non-101
        CHECK(ParseResponse("HTTP/1.1 403 Forbidden\r\n\r\n", key, consumed, error) ==
              HandshakeResult::Error);
    }

    // --- frame encode (RFC 6455 §5.7 samples) -----------------------------
    {
        // "A single-frame masked text message" - "Hello", mask 37 fa 21 3d
        const uint8_t mask[4] = { 0x37, 0xfa, 0x21, 0x3d };
        const std::vector<uint8_t> wire =
            EncodeText("Hello", mask);
        const uint8_t expected[] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                     0x7f, 0x9f, 0x4d, 0x51, 0x58 };
        CHECK_EQ(wire.size(), sizeof(expected));
        CHECK(std::memcmp(wire.data(), expected, sizeof(expected)) == 0);
    }

    // --- frame decode ------------------------------------------------------
    {
        // unmasked "Hello" (server->client form)
        const uint8_t wire[] = { 0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f };
        Frame f;
        size_t consumed = 0;
        std::string error;
        CHECK(TryDecodeFrame(wire, sizeof(wire), f, consumed, error) == DecodeResult::Ok);
        CHECK_EQ(consumed, sizeof(wire));
        CHECK(f.fin);
        CHECK(f.opcode == Opcode::Text);
        CHECK_EQ(std::string(f.payload.begin(), f.payload.end()), std::string("Hello"));

        // masked round-trip through the decoder
        const uint8_t mask[4] = { 0x37, 0xfa, 0x21, 0x3d };
        const std::vector<uint8_t> enc = EncodeText("Hello", mask);
        CHECK(TryDecodeFrame(enc.data(), enc.size(), f, consumed, error) == DecodeResult::Ok);
        CHECK_EQ(std::string(f.payload.begin(), f.payload.end()), std::string("Hello"));

        // partial input -> NeedMore at every cut point
        for (size_t cut = 0; cut < sizeof(wire); ++cut)
        {
            Frame partial;
            size_t c = 0;
            std::string e;
            CHECK(TryDecodeFrame(wire, cut, partial, c, e) == DecodeResult::NeedMore);
        }
    }
    {
        // 126-length form round-trip (256-byte payload)
        const uint8_t mask[4] = { 1, 2, 3, 4 };
        std::string big(256, 'x');
        const std::vector<uint8_t> enc = EncodeText(big, mask);
        CHECK_EQ(enc.size(), 2u + 2u + 4u + 256u);
        CHECK_EQ(enc[1] & 0x7F, 126);
        Frame f;
        size_t consumed = 0;
        std::string error;
        CHECK(TryDecodeFrame(enc.data(), enc.size(), f, consumed, error) == DecodeResult::Ok);
        CHECK_EQ(std::string(f.payload.begin(), f.payload.end()), big);
    }
    {
        // 127-length form round-trip (70000-byte payload)
        const uint8_t mask[4] = { 9, 8, 7, 6 };
        std::string huge(70000, 'y');
        const std::vector<uint8_t> enc = EncodeText(huge, mask);
        CHECK_EQ(enc[1] & 0x7F, 127);
        Frame f;
        size_t consumed = 0;
        std::string error;
        CHECK(TryDecodeFrame(enc.data(), enc.size(), f, consumed, error) == DecodeResult::Ok);
        CHECK_EQ(f.payload.size(), huge.size());

        // oversized declared length -> protocol error
        std::vector<uint8_t> evil = { 0x81, 127, 0xFF, 0xFF, 0xFF, 0xFF,
                                      0xFF, 0xFF, 0xFF, 0xFF };
        CHECK(TryDecodeFrame(evil.data(), evil.size(), f, consumed, error) ==
              DecodeResult::Error);
    }
    {
        // reserved bits / unknown opcode
        Frame f;
        size_t consumed = 0;
        std::string error;
        const uint8_t rsv[] = { 0xC1, 0x00 };
        CHECK(TryDecodeFrame(rsv, sizeof(rsv), f, consumed, error) == DecodeResult::Error);
        const uint8_t opc[] = { 0x83, 0x00 };
        CHECK(TryDecodeFrame(opc, sizeof(opc), f, consumed, error) == DecodeResult::Error);
    }

    // --- message assembler --------------------------------------------------
    {
        MessageAssembler asmb;

        // single-frame message
        Frame hello;
        hello.fin = true;
        hello.opcode = Opcode::Text;
        hello.payload = { 'H', 'i' };
        CHECK(asmb.Feed(hello) == MessageAssembler::Event::Message);
        CHECK_EQ(asmb.message, std::string("Hi"));

        // fragmented "Hel" + "lo" (RFC 6455 §5.7)
        Frame f1;
        f1.fin = false;
        f1.opcode = Opcode::Text;
        f1.payload = { 'H', 'e', 'l' };
        Frame f2;
        f2.fin = true;
        f2.opcode = Opcode::Continuation;
        f2.payload = { 'l', 'o' };
        CHECK(asmb.Feed(f1) == MessageAssembler::Event::None);
        CHECK(asmb.Feed(f2) == MessageAssembler::Event::Message);
        CHECK_EQ(asmb.message, std::string("Hello"));

        // ping surfaces with payload
        Frame ping;
        ping.fin = true;
        ping.opcode = Opcode::Ping;
        ping.payload = { 'p' };
        CHECK(asmb.Feed(ping) == MessageAssembler::Event::Ping);
        CHECK_EQ(asmb.control.size(), 1u);

        // control frame interleaved inside fragmentation is fine
        CHECK(asmb.Feed(f1) == MessageAssembler::Event::None);
        CHECK(asmb.Feed(ping) == MessageAssembler::Event::Ping);
        CHECK(asmb.Feed(f2) == MessageAssembler::Event::Message);
        CHECK_EQ(asmb.message, std::string("Hello"));

        // protocol errors
        MessageAssembler bad1;
        CHECK(bad1.Feed(f2) == MessageAssembler::Event::Error);  // orphan continuation
        MessageAssembler bad2;
        CHECK(bad2.Feed(f1) == MessageAssembler::Event::None);
        Frame newText;
        newText.fin = true;
        newText.opcode = Opcode::Text;
        newText.payload = { 'x' };
        CHECK(bad2.Feed(newText) == MessageAssembler::Event::Error);  // text mid-fragment

        // close
        MessageAssembler closer;
        Frame close;
        close.fin = true;
        close.opcode = Opcode::Close;
        close.payload = { 0x03, 0xE8 };  // 1000 normal closure
        CHECK(closer.Feed(close) == MessageAssembler::Event::Close);
    }

    std::cout << (g_failures == 0 ? "PASS" : "FAIL") << ": " << g_checks
              << " checks, " << g_failures << " failure(s)\n";
    return g_failures == 0 ? 0 : 1;
}
