// Unit tests for the Pusher protocol layer: crypto primitives (FIPS 180-4
// / RFC 4231 vectors), the auth token (the exact example from Pusher's
// auth-signature documentation), and the PusherSession state machine.
// Runs on any platform.

#include <cstring>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "pusher/Crypto.h"
#include "pusher/PusherSession.h"

using nlohmann::json;

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
                      << #a << " == " << #b << "  (got: " << (a) << ")\n"; \
        }                                                                  \
    } while (0)

static std::string Hex(const std::vector<uint8_t>& d)
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
    // --- SHA-256 (FIPS 180-4 examples) -----------------------------------
    {
        const char* abc = "abc";
        CHECK_EQ(Hex(Crypto::Sha256(reinterpret_cast<const uint8_t*>(abc), 3)),
                 std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        const char* empty = "";
        CHECK_EQ(Hex(Crypto::Sha256(reinterpret_cast<const uint8_t*>(empty), 0)),
                 std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
        const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        CHECK_EQ(Hex(Crypto::Sha256(reinterpret_cast<const uint8_t*>(two), 56)),
                 std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    }

    // --- HMAC-SHA256 (RFC 4231) -------------------------------------------
    {
        // Case 1: key = 20x 0x0b, data "Hi There"
        std::vector<uint8_t> key1(20, 0x0b);
        const char* data1 = "Hi There";
        CHECK_EQ(Hex(Crypto::HmacSha256(key1.data(), key1.size(),
                                        reinterpret_cast<const uint8_t*>(data1), 8)),
                 std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));

        // Case 2: key "Jefe", data "what do ya want for nothing?"
        CHECK_EQ(Crypto::HmacSha256Hex("Jefe", "what do ya want for nothing?"),
                 std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));

        // Case 6: 131-byte key (forces the key-hash path)
        std::vector<uint8_t> key6(131, 0xaa);
        const char* data6 = "Test Using Larger Than Block-Size Key - Hash Key First";
        CHECK_EQ(Hex(Crypto::HmacSha256(key6.data(), key6.size(),
                                        reinterpret_cast<const uint8_t*>(data6),
                                        std::strlen(data6))),
                 std::string("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"));
    }

    // --- auth token: the example from Pusher's own documentation ----------
    {
        Pusher::Config cfg;
        cfg.appKey = "278d425bdf160c739803";
        cfg.secret = "7ad3773142a6692b25b8";
        cfg.channel = "private-foobar";
        CHECK_EQ(Pusher::PusherSession::AuthToken(cfg, "1234.1234"),
                 std::string("278d425bdf160c739803:"
                             "58df8b0c36d6982b82c3ecf6b4662e34fe8c25bba48f5369f135bf843651c3a4"));
    }

    // --- connection path ---------------------------------------------------
    {
        Pusher::Config cfg;
        cfg.appKey = "appkey1";
        cfg.clientVersion = "0.4.0";
        Pusher::PusherSession session(cfg);
        CHECK_EQ(session.ConnectionPath(),
                 std::string("/app/appkey1?protocol=7&client=euroscope-websocket-connector"
                             "&version=0.4.0&flash=false"));
    }

    // --- subscribe flow: private channel with token auth --------------------
    {
        Pusher::Config cfg;
        cfg.appKey = "k3y";
        cfg.secret = "s3cret";
        cfg.channel = "private-euroscope";
        Pusher::PusherSession session(cfg);
        CHECK(!session.IsSubscribed());

        // connection_established with double-encoded data (as on the wire)
        auto in = session.HandleFrame(
            R"({"event":"pusher:connection_established","data":"{\"socket_id\":\"81607.9500\",\"activity_timeout\":120}"})");
        CHECK(!in.fatal);
        CHECK_EQ(session.SocketId(), std::string("81607.9500"));
        CHECK_EQ(in.sendNow.size(), 1u);

        const json sub = json::parse(in.sendNow[0]);
        CHECK_EQ(sub.at("event").get<std::string>(), "pusher:subscribe");
        CHECK_EQ(sub.at("data").at("channel").get<std::string>(), "private-euroscope");
        CHECK_EQ(sub.at("data").at("auth").get<std::string>(),
                 "k3y:" + Crypto::HmacSha256Hex("s3cret", "81607.9500:private-euroscope"));

        // subscription succeeded -> becameSubscribed exactly once
        auto ok = session.HandleFrame(
            R"({"event":"pusher_internal:subscription_succeeded","channel":"private-euroscope","data":"{}"})");
        CHECK(ok.becameSubscribed);
        CHECK(session.IsSubscribed());
        auto again = session.HandleFrame(
            R"({"event":"pusher_internal:subscription_succeeded","channel":"private-euroscope","data":"{}"})");
        CHECK(!again.becameSubscribed);
    }

    // --- public channel: no auth field --------------------------------------
    {
        Pusher::Config cfg;
        cfg.appKey = "k";
        cfg.channel = "euroscope";  // no private- prefix
        Pusher::PusherSession session(cfg);
        auto in = session.HandleFrame(
            R"({"event":"pusher:connection_established","data":"{\"socket_id\":\"1.1\"}"})");
        const json sub = json::parse(in.sendNow[0]);
        CHECK(!sub.at("data").contains("auth"));
    }

    // --- ping/pong, errors ---------------------------------------------------
    {
        Pusher::Config cfg;
        cfg.appKey = "k";
        cfg.secret = "s";
        Pusher::PusherSession session(cfg);

        auto ping = session.HandleFrame(R"({"event":"pusher:ping","data":"{}"})");
        CHECK_EQ(ping.sendNow.size(), 1u);
        CHECK(json::parse(ping.sendNow[0]).at("event").get<std::string>() == "pusher:pong");

        // 4001 (bad app key): fatal
        auto fatal = session.HandleFrame(
            R"({"event":"pusher:error","data":{"code":4001,"message":"App key does not exist"}})");
        CHECK(fatal.fatal);
        CHECK(fatal.notice.find("4001") != std::string::npos);

        // 4201 (transient): not fatal
        auto transient = session.HandleFrame(
            R"({"event":"pusher:error","data":{"code":4201,"message":"ping timeout"}})");
        CHECK(!transient.fatal);

        // garbage frames are ignored, not fatal
        auto garbage = session.HandleFrame("not json at all");
        CHECK(!garbage.fatal);
        CHECK(garbage.commands.empty());
    }

    // --- contract messages over client events --------------------------------
    {
        Pusher::Config cfg;
        cfg.appKey = "k";
        cfg.secret = "s";
        cfg.channel = "private-euroscope";
        Pusher::PusherSession session(cfg);
        session.HandleFrame(
            R"({"event":"pusher:connection_established","data":"{\"socket_id\":\"1.1\"}"})");
        session.HandleFrame(
            R"({"event":"pusher_internal:subscription_succeeded","channel":"private-euroscope","data":"{}"})");

        // outgoing: wrapped as a client event, data double-encoded
        const std::string contract =
            R"({"type":"event","action":"flight_removed","callsign":"DLH4TX","payload":{}})";
        const json wrapped = json::parse(session.WrapOutgoing(contract));
        CHECK_EQ(wrapped.at("event").get<std::string>(), Pusher::kContractEventName);
        CHECK_EQ(wrapped.at("channel").get<std::string>(), "private-euroscope");
        CHECK(wrapped.at("data").is_string());
        CHECK_EQ(wrapped.at("data").get<std::string>(), contract);

        // incoming: data as double-encoded string
        auto cmd1 = session.HandleFrame(
            R"({"event":"client-euroscope","channel":"private-euroscope","data":"{\"type\":\"command\",\"action\":\"ping\"}"})");
        CHECK_EQ(cmd1.commands.size(), 1u);
        CHECK(json::parse(cmd1.commands[0]).at("action").get<std::string>() == "ping");

        // incoming: data as inline object (lenient)
        auto cmd2 = session.HandleFrame(
            R"({"event":"client-euroscope","channel":"private-euroscope","data":{"type":"command","action":"ping"}})");
        CHECK_EQ(cmd2.commands.size(), 1u);

        // unrelated events on the channel are ignored
        auto other = session.HandleFrame(
            R"({"event":"App\\Events\\SomethingElse","channel":"private-euroscope","data":"{}"})");
        CHECK(other.commands.empty());
    }

    std::cout << (g_failures == 0 ? "PASS" : "FAIL") << ": " << g_checks
              << " checks, " << g_failures << " failure(s)\n";
    return g_failures == 0 ? 0 : 1;
}
