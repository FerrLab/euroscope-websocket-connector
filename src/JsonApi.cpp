#include "JsonApi.h"

#include <cmath>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "ChatInjection.h"
#include "Types.h"

using nlohmann::json;

// ---------------------------------------------------------------------
// serialization
// ---------------------------------------------------------------------

namespace
{
    // FlightInfo -> JSON. Field names are part of the public contract
    // (docs/PROTOCOL.md) - do not rename casually.
    json ToJson(const FlightInfo& f)
    {
        json j = {
            { "callsign", f.callsign },
            { "planType", f.planType },
            { "aircraftType", f.aircraftType },
            { "wtc", std::string(1, f.wtc) },
            { "origin", f.origin },
            { "destination", f.destination },
            { "alternate", f.alternate },
            { "departureRunway", f.departureRunway },
            { "arrivalRunway", f.arrivalRunway },
            { "sid", f.sid },
            { "star", f.star },
            { "route", f.route },
            { "remarks", f.remarks },
            { "finalAltitude", f.finalAltitude },
            { "clearedAltitude", f.clearedAltitude },
            { "assignedHeading", f.assignedHeading },
            { "assignedSpeed", f.assignedSpeed },
            { "assignedMach", f.assignedMach },
            { "assignedRate", f.assignedRate },
            { "directTo", f.directTo },
            { "assignedSquawk", f.assignedSquawk },
            { "scratchPad", f.scratchPad },
            { "groundState", f.groundState },
            { "clearanceFlag", f.clearanceFlag },
            { "trackingController", f.trackingController },
            { "trackedByMe", f.trackedByMe },
            { "communicationType", std::string(1, f.communicationType) },
            { "correlated", f.correlated },
        };
        if (f.correlated)
        {
            j["transponderSquawk"] = f.transponderSquawk;
            j["flightLevel"] = f.flightLevel;
            j["groundSpeed"] = f.groundSpeed;
            j["latitude"] = f.latitude;
            j["longitude"] = f.longitude;
        }
        return j;
    }

    // Envelope for plugin -> outside messages.
    json Envelope(const char* type, const std::string& action,
                  const std::string& callsign)
    {
        json j = {
            { "type", type },
            { "action", action },
        };
        if (!callsign.empty())
            j["callsign"] = callsign;
        return j;
    }

    // ---- payload extraction helpers ------------------------------------
    // Throw std::runtime_error with a caller-friendly message; the
    // dispatcher turns that into an ok:false response.

    [[noreturn]] void Bad(const std::string& msg)
    {
        throw std::runtime_error(msg);
    }

    int RequireInt(const json& payload, const char* name)
    {
        if (!payload.contains(name))
            Bad(std::string("missing payload field '") + name + "' (integer)");
        const json& v = payload.at(name);
        if (!v.is_number_integer())
            Bad(std::string("payload field '") + name + "' must be an integer");
        return v.get<int>();
    }

    std::string RequireString(const json& payload, const char* name)
    {
        if (!payload.contains(name))
            Bad(std::string("missing payload field '") + name + "' (string)");
        const json& v = payload.at(name);
        if (!v.is_string())
            Bad(std::string("payload field '") + name + "' must be a string");
        return v.get<std::string>();
    }

    std::string RequireCallsign(const std::string& callsign)
    {
        if (callsign.empty())
            Bad("this action requires a top-level \"callsign\"");
        return callsign;
    }

    // Altitude: {"feet": 12000} or {"special": "ils"|"visual"|"clear"}.
    int AltitudeParam(const json& payload)
    {
        if (payload.contains("special"))
        {
            const std::string s = payload.at("special").is_string()
                                      ? payload.at("special").get<std::string>()
                                      : "";
            if (s == "clear")
                return 0;
            if (s == "ils")
                return 1;
            if (s == "visual")
                return 2;
            Bad("payload field 'special' must be one of: ils, visual, clear");
        }
        const int feet = RequireInt(payload, "feet");
        if (feet < 0 || feet > 99999)
            Bad("payload field 'feet' out of range");
        return feet;
    }

    // Mach: {"mach": 0.78} (number < 1) or {"mach": 78} (hundredths).
    int MachParam(const json& payload)
    {
        if (!payload.contains("mach"))
            Bad("missing payload field 'mach' (0.78 or 78)");
        const json& v = payload.at("mach");
        if (v.is_number_float())
        {
            const double d = v.get<double>();
            if (d < 0.0 || d >= 1.0)
                Bad("payload field 'mach' as a decimal must be in [0, 1)");
            return static_cast<int>(std::lround(d * 100.0));
        }
        if (v.is_number_integer())
        {
            const int n = v.get<int>();
            if (n < 0 || n > 99)
                Bad("payload field 'mach' as hundredths must be 0-99");
            return n;
        }
        Bad("payload field 'mach' must be a number");
    }
}

// ---------------------------------------------------------------------
// command dispatch
// ---------------------------------------------------------------------

std::string JsonApi::HandleMessage(const std::string& messageJson)
{
    json response = {
        { "type", "response" },
        { "ok", false },
    };

    // Parse (non-throwing), validate the envelope.
    json message = json::parse(messageJson, nullptr, false);
    if (message.is_discarded() || !message.is_object())
    {
        response["error"] = "message is not a JSON object";
        return response.dump();
    }

    if (message.contains("id"))
        response["id"] = message.at("id");

    const std::string type =
        message.contains("type") && message.at("type").is_string()
            ? message.at("type").get<std::string>()
            : "";
    const std::string action =
        message.contains("action") && message.at("action").is_string()
            ? message.at("action").get<std::string>()
            : "";
    const std::string callsign =
        message.contains("callsign") && message.at("callsign").is_string()
            ? message.at("callsign").get<std::string>()
            : "";
    const json payload = message.contains("payload") &&
                                 message.at("payload").is_object()
                             ? message.at("payload")
                             : json::object();

    response["action"] = action;
    if (!callsign.empty())
        response["callsign"] = callsign;

    try
    {
        if (type != "command")
            Bad(type.empty()
                    ? "missing \"type\" - inbound messages must have type \"command\""
                    : "unsupported type '" + type + "' - the plugin only accepts \"command\"");

        json result = json::object();

        if (action == "ping")
        {
            result["plugin"] = "euroscope-websocket-connector";
            result["protocolVersion"] = kProtocolVersion;
        }
        else if (action == "list_flights")
        {
            const std::string filter =
                payload.contains("filter") && payload.at("filter").is_string()
                    ? payload.at("filter").get<std::string>()
                    : "";
            std::vector<FlightInfo> flights = m_actions.CollectFlights(filter);
            result["count"] = flights.size();
            json arr = json::array();
            for (const FlightInfo& f : flights)
                arr.push_back(ToJson(f));
            result["flights"] = std::move(arr);
        }
        else if (action == "get_flight")
        {
            FlightInfo info;
            std::string error;
            if (!m_actions.GetFlight(RequireCallsign(callsign), info, error))
                Bad(error);
            result = ToJson(info);
        }
        else
        {
            // Write actions: map onto Actions 1:1 and reuse its outcome
            // message. `ar.ok == false` becomes an error response.
            ActionResult ar;

            if (action == "set_cleared_altitude")
                ar = m_actions.SetClearedAltitude(RequireCallsign(callsign), AltitudeParam(payload));
            else if (action == "set_final_altitude")
                ar = m_actions.SetFinalAltitude(RequireCallsign(callsign), RequireInt(payload, "feet"));
            else if (action == "set_heading")
                ar = m_actions.SetHeading(RequireCallsign(callsign), RequireInt(payload, "degrees"));
            else if (action == "set_speed")
                ar = m_actions.SetSpeed(RequireCallsign(callsign), RequireInt(payload, "knots"));
            else if (action == "set_mach")
                ar = m_actions.SetMach(RequireCallsign(callsign), MachParam(payload));
            else if (action == "set_rate")
                ar = m_actions.SetRate(RequireCallsign(callsign), RequireInt(payload, "feetPerMinute"));
            else if (action == "set_squawk")
                ar = m_actions.SetSquawk(RequireCallsign(callsign), RequireString(payload, "code"));
            else if (action == "set_direct")
                ar = m_actions.SetDirect(RequireCallsign(callsign), RequireString(payload, "point"));
            else if (action == "set_scratchpad")
                ar = m_actions.SetScratchPad(RequireCallsign(callsign), RequireString(payload, "text"));
            else if (action == "set_ground_state")
                ar = m_actions.BroadcastScratchPadToken(RequireCallsign(callsign), RequireString(payload, "state"));
            else if (action == "set_sid")
                ar = m_actions.SetSid(RequireCallsign(callsign), RequireString(payload, "sid"));
            else if (action == "set_star")
                ar = m_actions.SetStar(RequireCallsign(callsign), RequireString(payload, "star"));
            else if (action == "send_private_message")
            {
                const auto r = ChatInjection::SendCommandLine(
                    ".msg " + RequireCallsign(callsign) + " " + RequireString(payload, "message"));
                ar = r.ok ? ActionResult::Ok("private message handed to EuroScope")
                          : ActionResult::Fail(r.detail);
            }
            else if (action == "send_frequency_message")
            {
                const auto r = ChatInjection::SendToPrimaryFrequency(
                    RequireString(payload, "message"));
                ar = r.ok ? ActionResult::Ok("frequency message handed to EuroScope")
                          : ActionResult::Fail(r.detail);
            }
            else if (action.empty())
                Bad("missing \"action\"");
            else
                Bad("unknown action '" + action +
                    "' - see docs/PROTOCOL.md for the list of actions");

            if (!ar.ok)
                Bad(ar.message);
            result["message"] = ar.message;
        }

        response["ok"] = true;
        response["payload"] = std::move(result);
    }
    catch (const std::exception& e)
    {
        response["ok"] = false;
        response["error"] = e.what();
        response.erase("payload");
    }
    catch (...)
    {
        response["ok"] = false;
        response["error"] = "internal error while handling the message";
        response.erase("payload");
    }

    return response.dump();
}

// ---------------------------------------------------------------------
// events (plugin -> outside)
// ---------------------------------------------------------------------

std::string JsonApi::EventFlightUpdated(const FlightInfo& flight) const
{
    json j = Envelope("event", "flight_updated", flight.callsign);
    j["payload"] = ToJson(flight);
    return j.dump();
}

std::string JsonApi::EventFlightRemoved(const std::string& callsign) const
{
    json j = Envelope("event", "flight_removed", callsign);
    j["payload"] = json::object();
    return j.dump();
}

std::string JsonApi::EventPositionUpdated(const PositionUpdate& position) const
{
    json j = Envelope("event", "position_updated", position.callsign);
    j["payload"] = {
        { "latitude", position.latitude },
        { "longitude", position.longitude },
        { "flightLevel", position.flightLevel },
        { "groundSpeed", position.groundSpeed },
        { "squawk", position.squawk },
    };
    return j.dump();
}

std::string JsonApi::EventSessionReset() const
{
    json j = Envelope("event", "session_reset", std::string());
    j["payload"] = json::object();
    return j.dump();
}

std::string JsonApi::EventSessionSnapshot(const std::vector<FlightInfo>& flights) const
{
    json j = Envelope("event", "session_snapshot", std::string());
    json arr = json::array();
    for (const FlightInfo& f : flights)
        arr.push_back(ToJson(f));
    j["payload"] = {
        { "count", flights.size() },
        { "flights", std::move(arr) },
    };
    return j.dump();
}
