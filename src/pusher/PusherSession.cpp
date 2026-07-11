#include "PusherSession.h"

#include <nlohmann/json.hpp>

#include "Crypto.h"

using nlohmann::json;

namespace Pusher
{
    const char* kContractEventName = "client-euroscope";

    namespace
    {
        bool IsPrivateChannel(const std::string& channel)
        {
            return channel.rfind("private-", 0) == 0 ||
                   channel.rfind("presence-", 0) == 0;
        }

        // Pusher "data" fields are usually double-encoded (a JSON string
        // containing JSON). Accept both forms.
        json UnpackData(const json& message)
        {
            if (!message.contains("data"))
                return json::object();
            const json& d = message.at("data");
            if (d.is_object())
                return d;
            if (d.is_string())
            {
                json parsed = json::parse(d.get<std::string>(), nullptr, false);
                if (!parsed.is_discarded())
                    return parsed;
            }
            return json::object();
        }
    }

    std::string PusherSession::ConnectionPath() const
    {
        return "/app/" + m_config.appKey +
               "?protocol=7&client=euroscope-websocket-connector&version=" +
               (m_config.clientVersion.empty() ? "0.0.0" : m_config.clientVersion) +
               "&flash=false";
    }

    std::string PusherSession::AuthToken(const Config& config,
                                         const std::string& socketId)
    {
        // RFC-documented Pusher signature: HMAC-SHA256 over
        // "<socket_id>:<channel>", keyed with the app secret, hex-encoded,
        // prefixed with the app key. (Presence channels would append
        // ":<channel_data>" - not used here.)
        const std::string toSign = socketId + ":" + config.channel;
        return config.appKey + ":" + Crypto::HmacSha256Hex(config.secret, toSign);
    }

    std::string PusherSession::BuildSubscribe() const
    {
        json data = { { "channel", m_config.channel } };
        if (IsPrivateChannel(m_config.channel))
            data["auth"] = AuthToken(m_config, m_socketId);

        const json msg = {
            { "event", "pusher:subscribe" },
            { "data", data },
        };
        return msg.dump();
    }

    std::string PusherSession::WrapOutgoing(const std::string& contractJson) const
    {
        const json msg = {
            { "event", kContractEventName },
            { "channel", m_config.channel },
            // Double-encoded, as the protocol prescribes for data fields.
            { "data", contractJson },
        };
        return msg.dump();
    }

    PusherSession::Incoming PusherSession::HandleFrame(const std::string& rawFrame)
    {
        Incoming out;

        const json message = json::parse(rawFrame, nullptr, false);
        if (message.is_discarded() || !message.is_object() ||
            !message.contains("event") || !message.at("event").is_string())
        {
            out.notice = "ignoring non-Pusher frame";
            return out;
        }
        const std::string event = message.at("event").get<std::string>();

        if (event == "pusher:connection_established")
        {
            const json data = UnpackData(message);
            m_socketId = data.contains("socket_id") && data.at("socket_id").is_string()
                             ? data.at("socket_id").get<std::string>()
                             : "";
            if (m_socketId.empty())
            {
                out.notice = "connection_established without socket_id";
                out.fatal = true;
                return out;
            }
            out.sendNow.push_back(BuildSubscribe());
            return out;
        }

        if (event == "pusher_internal:subscription_succeeded")
        {
            if (!m_subscribed)
            {
                m_subscribed = true;
                out.becameSubscribed = true;
            }
            return out;
        }

        if (event == "pusher:ping")
        {
            out.sendNow.push_back(R"({"event":"pusher:pong","data":"{}"})");
            return out;
        }

        if (event == "pusher:pong")
            return out;

        if (event == "pusher:error")
        {
            const json data = UnpackData(message);
            const int code = data.contains("code") && data.at("code").is_number_integer()
                                 ? data.at("code").get<int>()
                                 : 0;
            const std::string text =
                data.contains("message") && data.at("message").is_string()
                    ? data.at("message").get<std::string>()
                    : rawFrame;
            out.notice = "pusher:error " + std::to_string(code) + ": " + text;
            // 4000-4099: the server will close and reconnecting with the
            // same parameters won't help (bad key, app disabled, over
            // quota). 41xx/42xx are transient.
            out.fatal = code >= 4000 && code <= 4099;
            return out;
        }

        if (event == kContractEventName)
        {
            // Inbound contract message. The data field may be the contract
            // object double-encoded (normal) or inline.
            if (message.contains("data"))
            {
                const json& d = message.at("data");
                if (d.is_string())
                    out.commands.push_back(d.get<std::string>());
                else if (d.is_object())
                    out.commands.push_back(d.dump());
            }
            return out;
        }

        // Other events on the channel (from other subscribers/frameworks)
        // are none of our business.
        return out;
    }
}
