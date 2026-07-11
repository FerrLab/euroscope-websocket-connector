#include "ConnectorPlugin.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "ChatInjection.h"
#include "GatewayConfig.h"

namespace
{
    std::vector<std::string> Tokenize(const std::string& line)
    {
        std::vector<std::string> tokens;
        std::istringstream in(line);
        std::string t;
        while (in >> t)
            tokens.push_back(t);
        return tokens;
    }

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // Original text that follows the first `count` whitespace-separated
    // tokens - preserves inner spacing of free-text arguments.
    std::string RemainderAfterTokens(const std::string& line, size_t count)
    {
        size_t pos = 0;
        for (size_t i = 0; i < count; ++i)
        {
            while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])))
                ++pos;
            while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos])))
                ++pos;
        }
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])))
            ++pos;
        return line.substr(pos);
    }

    // Accepts "FL340", "34000", "ils", "visual" or "clear"/"0".
    // Output is the value SetClearedAltitude()/SetFinalAltitude() expect:
    // feet, or the special values 0 (none), 1 (ILS), 2 (visual).
    bool ParseAltitude(const std::string& in, int& out)
    {
        const std::string v = Lower(in);
        if (v == "clear" || v == "0")
        {
            out = 0;
            return true;
        }
        if (v == "ils")
        {
            out = 1;
            return true;
        }
        if (v == "visual" || v == "vis")
        {
            out = 2;
            return true;
        }
        try
        {
            if (v.size() > 2 && v.compare(0, 2, "fl") == 0)
            {
                out = std::stoi(v.substr(2)) * 100;
                return true;
            }
            size_t used = 0;
            out = std::stoi(v, &used);
            return used == v.size() && out > 0;
        }
        catch (...)
        {
            return false;
        }
    }

    bool ParseInt(const std::string& in, int& out)
    {
        try
        {
            size_t used = 0;
            out = std::stoi(in, &used);
            return used == in.size();
        }
        catch (...)
        {
            return false;
        }
    }

    // Mach as "0.78", ".78" or "78" -> 78 (hundredths).
    bool ParseMach(const std::string& in, int& out)
    {
        std::string v = in;
        if (v.rfind("0.", 0) == 0)
            v = v.substr(2);
        else if (v.rfind(".", 0) == 0)
            v = v.substr(1);
        if (!ParseInt(v, out))
            return false;
        return out >= 0 && out <= 99;
    }
}

namespace
{
    // One compact summary line per flight, e.g.:
    // DLH4TX  A320  EDDM>EDDF  RFL240  CFL120  SQ1000  TAXI  GS18  [me]
    std::string FormatSummaryLine(const FlightInfo& f)
    {
        std::ostringstream line;
        line << f.callsign << "  " << f.aircraftType
             << "  " << (f.origin.empty() ? "????" : f.origin)
             << ">" << (f.destination.empty() ? "????" : f.destination)
             << "  RFL" << f.finalAltitude / 100;

        if (f.clearedAltitude == 1)
            line << "  CFL:ILS";
        else if (f.clearedAltitude == 2)
            line << "  CFL:VIS";
        else if (f.clearedAltitude != 0)
            line << "  CFL" << f.clearedAltitude / 100;

        if (!f.assignedSquawk.empty())
            line << "  SQ" << f.assignedSquawk;
        if (!f.groundState.empty())
            line << "  " << f.groundState;
        if (f.clearanceFlag)
            line << "  CLEA";
        if (f.correlated)
            line << "  GS" << f.groundSpeed;
        if (!f.trackingController.empty())
            line << "  [" << (f.trackedByMe ? "me" : f.trackingController) << "]";
        return line.str();
    }

    std::string FormatDetail(const FlightInfo& f)
    {
        std::ostringstream out;
        out << f.callsign << "  " << f.planType << "  " << f.aircraftType
            << " (WTC " << f.wtc << ")\n";

        out << "Routing: " << f.origin;
        if (!f.departureRunway.empty())
            out << "/" << f.departureRunway;
        out << " -> " << f.destination;
        if (!f.arrivalRunway.empty())
            out << "/" << f.arrivalRunway;
        if (!f.alternate.empty())
            out << " (ALTN " << f.alternate << ")";
        out << "\n";

        out << "SID: " << (f.sid.empty() ? "-" : f.sid)
            << "   STAR: " << (f.star.empty() ? "-" : f.star) << "\n";

        out << "RFL " << f.finalAltitude << " ft";
        if (f.clearedAltitude == 1)
            out << "   CFL: cleared ILS approach";
        else if (f.clearedAltitude == 2)
            out << "   CFL: cleared visual approach";
        else if (f.clearedAltitude != 0)
            out << "   CFL " << f.clearedAltitude << " ft";
        out << "\n";

        std::ostringstream assigned;
        if (f.assignedHeading != 0)
            assigned << "  HDG " << f.assignedHeading;
        if (f.assignedSpeed != 0)
            assigned << "  SPD " << f.assignedSpeed;
        if (f.assignedMach != 0)
            assigned << "  MACH 0." << f.assignedMach;
        if (f.assignedRate != 0)
            assigned << "  RATE " << f.assignedRate;
        if (!f.directTo.empty())
            assigned << "  DCT " << f.directTo;
        const std::string asgn = assigned.str();
        out << "Assigned:" << (asgn.empty() ? " -" : asgn) << "\n";

        out << "Squawk assigned: " << (f.assignedSquawk.empty() ? "-" : f.assignedSquawk);
        if (f.correlated)
        {
            out << "   transponder: " << f.transponderSquawk
                << "   FL" << f.flightLevel / 100 << "   GS" << f.groundSpeed;
        }
        out << "\n";

        out << "Ground state: " << (f.groundState.empty() ? "-" : f.groundState)
            << "   Clearance flag: " << (f.clearanceFlag ? "received" : "not received")
            << "\n";

        out << "Tracked by: "
            << (f.trackingController.empty()
                    ? "-"
                    : (f.trackedByMe ? f.trackingController + " (me)" : f.trackingController))
            << "   Comm: " << f.communicationType << "\n";

        out << "Scratch pad: " << (f.scratchPad.empty() ? "-" : f.scratchPad) << "\n";
        out << "Route: " << f.route << "\n";
        if (!f.remarks.empty())
            out << "Remarks: " << f.remarks;
        return out.str();
    }
}

ConnectorPlugin::ConnectorPlugin()
    : EuroScopePlugIn::CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE,
                               PLUGIN_NAME,
                               PLUGIN_VERSION,
                               PLUGIN_AUTHOR,
                               PLUGIN_COPYRIGHT),
      m_actions(this),
      m_jsonApi(m_actions)
{
    LoadSettings();
    Say(std::string(PLUGIN_NAME) + " v" PLUGIN_VERSION
        " loaded. Type '.lpc help' for available commands.");
    if (m_gateway.IsEnabled())
        Say("Auto-connecting to gateway " + m_gateway.GetUrl() + " ...");
}

void ConnectorPlugin::LoadSettings()
{
    const char* url = GetDataFromSettings("GatewayUrl");
    if (url && *url)
    {
        const std::string error = m_gateway.SetUrl(url);
        if (!error.empty())
            Say(std::string("Saved gateway URL is invalid: ") + error);
    }

    const char* token = GetDataFromSettings("GatewayToken");
    if (token && *token)
        m_gateway.SetToken(token);

    const char* positions = GetDataFromSettings("GatewayPositions");
    if (positions && *positions)
        m_gatewayPositions = std::string(positions) != "0";

    const char* autoConnect = GetDataFromSettings("GatewayAuto");
    if (autoConnect && std::string(autoConnect) == "1" && m_gateway.HasValidUrl())
    {
        const std::string error = m_gateway.Enable();
        if (!error.empty())
            Say("Gateway auto-connect skipped: " + error);
    }
}

void ConnectorPlugin::SaveSetting(const char* name, const char* description,
                                  const std::string& value)
{
    SaveDataToSettings(name, description, value.c_str());
}

ConnectorPlugin::~ConnectorPlugin() = default;

void ConnectorPlugin::Say(const std::string& text)
{
    DisplayUserMessage(MESSAGE_HANDLER, MESSAGE_HANDLER, text.c_str(),
                       true /*show handler*/, true /*mark unread*/,
                       false /*unread if busy*/, false /*flash*/,
                       false /*confirm*/);
}

void ConnectorPlugin::SayLines(const std::string& text)
{
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
        if (!line.empty())
            Say(line);
}

bool ConnectorPlugin::OnCompileCommand(const char* sCommandLine)
{
    const std::string line = sCommandLine ? sCommandLine : "";
    std::vector<std::string> tokens = Tokenize(line);
    if (tokens.empty() || Lower(tokens[0]) != ".lpc")
        return false; // not ours - let EuroScope / other plugins handle it

    const std::string sub = tokens.size() > 1 ? Lower(tokens[1]) : "help";

    if (sub == "help")
        CmdHelp();
    else if (sub == "list")
        CmdList(tokens);
    else if (sub == "show")
        CmdShow(tokens);
    else if (sub == "set")
        CmdSet(tokens);
    else if (sub == "pad")
        CmdPad(tokens, line);
    else if (sub == "state")
        CmdState(tokens);
    else if (sub == "sid")
        CmdSid(tokens, false);
    else if (sub == "star")
        CmdSid(tokens, true);
    else if (sub == "msg")
        CmdMsg(tokens, line);
    else if (sub == "freq")
        CmdFreq(tokens, line);
    else if (sub == "json")
        CmdJson(tokens, line);
    else if (sub == "events")
        CmdEvents(tokens);
    else if (sub == "gateway")
        CmdGateway(tokens);
    else
        Say("Unknown sub-command '" + tokens[1] + "'. Type '.lpc help'.");

    return true; // consume every ".lpc ..." line, even on bad syntax
}

void ConnectorPlugin::CmdHelp()
{
    Say(std::string(PLUGIN_NAME) + " v" PLUGIN_VERSION " - commands (full docs: docs/COMMANDS.md):");
    Say(".lpc list [filter]            - list flight plans (filter: callsign prefix or ICAO of dep/arr)");
    Say(".lpc show <callsign>          - full detail of one flight (incl. SID/STAR, scratch pad)");
    Say(".lpc set <cs> calt <FL240|24000|ils|visual|clear> - cleared altitude");
    Say(".lpc set <cs> rfl <FL340|34000>  - final altitude   | hdg <deg|0> - heading");
    Say(".lpc set <cs> spd <kts|0> | mach <0.78|0> | rate <fpm|0> - speeds/rate");
    Say(".lpc set <cs> sqk <code> | dct <point>   - squawk / direct-to");
    Say(".lpc pad <cs> <text|clear>    - set scratch pad");
    Say(".lpc state <cs> <NSTS|STUP|PUSH|TAXI|DEPA|TXIN|PARK|CLEA|NOTC|ARR> - ground state/clearance flag");
    Say(".lpc sid <cs> <SID[/RWY]> | star <cs> <STAR> - assign SID/STAR");
    Say(".lpc msg <cs> <text>          - private message (experimental, UI injection)");
    Say(".lpc freq <text>              - text to primary frequency (experimental, UI injection)");
    Say(".lpc json <message>           - run a JSON contract command (docs/PROTOCOL.md)");
    Say(".lpc events <on|off|pos on|pos off|status> - print the JSON event stream");
    Say(".lpc gateway config <base64 of url:token> - set backend URL + token in one command");
    Say(".lpc gateway connect | disconnect | status");
    Say(".lpc gateway auto <on|off> | pos <on|off>  - autoconnect / send positions");
}

void ConnectorPlugin::CmdList(const std::vector<std::string>& tokens)
{
    const std::string filter = tokens.size() > 2 ? tokens[2] : "";
    const size_t kMaxLines = 25;

    const std::vector<FlightInfo> flights = m_actions.CollectFlights(filter);

    if (flights.empty())
    {
        Say(filter.empty() ? "No flight plans in this session."
                           : "No flight plans matching '" + filter + "'.");
        return;
    }
    Say("--- " + std::to_string(flights.size()) + " flight(s)" +
        (filter.empty() ? "" : " matching '" + filter + "'") + " ---");
    for (size_t i = 0; i < flights.size() && i < kMaxLines; ++i)
        Say(FormatSummaryLine(flights[i]));
    if (flights.size() > kMaxLines)
        Say("(+" + std::to_string(flights.size() - kMaxLines) +
            " more - narrow it down with '.lpc list <filter>')");
}

void ConnectorPlugin::CmdShow(const std::vector<std::string>& tokens)
{
    if (tokens.size() < 3)
    {
        Say("Usage: .lpc show <callsign>");
        return;
    }
    FlightInfo info;
    std::string error;
    if (!m_actions.GetFlight(tokens[2], info, error))
    {
        Say(error);
        return;
    }
    SayLines(FormatDetail(info));
}

void ConnectorPlugin::CmdSet(const std::vector<std::string>& tokens)
{
    if (tokens.size() < 5)
    {
        Say("Usage: .lpc set <callsign> <calt|rfl|hdg|spd|mach|rate|sqk|dct> <value>");
        return;
    }
    const std::string& callsign = tokens[2];
    const std::string field = Lower(tokens[3]);
    const std::string& value = tokens[4];

    ActionResult r = ActionResult::Fail("Unknown field '" + field +
                                        "'. Valid: calt rfl hdg spd mach rate sqk dct");
    int n = 0;
    if (field == "calt" || field == "cfl")
    {
        if (ParseAltitude(value, n))
            r = m_actions.SetClearedAltitude(callsign, n);
        else
            r = ActionResult::Fail("Bad altitude '" + value + "' (use FL240, 24000, ils, visual or clear)");
    }
    else if (field == "rfl" || field == "final")
    {
        if (ParseAltitude(value, n) && n > 2)
            r = m_actions.SetFinalAltitude(callsign, n);
        else
            r = ActionResult::Fail("Bad altitude '" + value + "' (use FL340 or feet)");
    }
    else if (field == "hdg" || field == "heading")
    {
        if (ParseInt(value, n) && n >= 0 && n <= 360)
            r = m_actions.SetHeading(callsign, n);
        else
            r = ActionResult::Fail("Bad heading '" + value + "' (1-360, 0 clears)");
    }
    else if (field == "spd" || field == "speed")
    {
        if (ParseInt(value, n) && n >= 0)
            r = m_actions.SetSpeed(callsign, n);
        else
            r = ActionResult::Fail("Bad speed '" + value + "' (knots, 0 clears)");
    }
    else if (field == "mach")
    {
        if (ParseMach(value, n))
            r = m_actions.SetMach(callsign, n);
        else
            r = ActionResult::Fail("Bad mach '" + value + "' (use 0.78, .78 or 78; 0 clears)");
    }
    else if (field == "rate")
    {
        if (ParseInt(value, n))
            r = m_actions.SetRate(callsign, n);
        else
            r = ActionResult::Fail("Bad rate '" + value + "' (feet per minute, 0 clears)");
    }
    else if (field == "sqk" || field == "squawk")
    {
        r = m_actions.SetSquawk(callsign, value);
    }
    else if (field == "dct" || field == "direct")
    {
        r = m_actions.SetDirect(callsign, value);
    }
    Say(r.message);
}

void ConnectorPlugin::CmdPad(const std::vector<std::string>& tokens, const std::string& line)
{
    if (tokens.size() < 4)
    {
        Say("Usage: .lpc pad <callsign> <text|clear>");
        return;
    }
    // Keep the raw remainder so multi-word pad content survives.
    std::string text = RemainderAfterTokens(line, 3);
    if (Lower(text) == "clear")
        text.clear();
    Say(m_actions.SetScratchPad(tokens[2], text).message);
}

void ConnectorPlugin::CmdState(const std::vector<std::string>& tokens)
{
    if (tokens.size() < 4)
    {
        Say("Usage: .lpc state <callsign> <NSTS|STUP|PUSH|TAXI|DEPA|TXIN|PARK|CLEA|NOTC|ARR>");
        return;
    }
    Say(m_actions.BroadcastScratchPadToken(tokens[2], tokens[3]).message);
}

void ConnectorPlugin::CmdSid(const std::vector<std::string>& tokens, bool star)
{
    if (tokens.size() < 4)
    {
        Say(star ? "Usage: .lpc star <callsign> <STAR>"
                 : "Usage: .lpc sid <callsign> <SID[/RWY]>");
        return;
    }
    const ActionResult r = star ? m_actions.SetStar(tokens[2], tokens[3])
                                : m_actions.SetSid(tokens[2], tokens[3]);
    Say(r.message);
}

void ConnectorPlugin::CmdMsg(const std::vector<std::string>& tokens, const std::string& line)
{
    if (tokens.size() < 4)
    {
        Say("Usage: .lpc msg <callsign> <message text>");
        return;
    }
    const std::string text = RemainderAfterTokens(line, 3);
    // ".msg <callsign> <text>" is EuroScope's own private-message command;
    // we type it into the command line on the controller's behalf.
    const auto r = ChatInjection::SendCommandLine(
        ".msg " + tokens[2] + " " + text, "Private message to " + tokens[2]);
    Say(r.ok ? "Private message to " + tokens[2] + ": " + r.detail
             : "Private message FAILED: " + r.detail);
}

void ConnectorPlugin::CmdFreq(const std::vector<std::string>& tokens, const std::string& line)
{
    if (tokens.size() < 3)
    {
        Say("Usage: .lpc freq <message text>");
        return;
    }
    const std::string text = RemainderAfterTokens(line, 2);
    const auto r = ChatInjection::SendToPrimaryFrequency(text, "Frequency text");
    Say(r.ok ? "Frequency text: " + r.detail
             : "Frequency text FAILED: " + r.detail);
}

void ConnectorPlugin::CmdJson(const std::vector<std::string>& tokens, const std::string& line)
{
    if (tokens.size() < 3)
    {
        Say("Usage: .lpc json {\"type\":\"command\",\"callsign\":\"DLH4TX\",\"action\":\"get_flight\"}");
        Say("Contract reference: docs/PROTOCOL.md");
        return;
    }
    // Everything after ".lpc json" is the raw message; the response is the
    // exact payload a gateway backend would receive.
    Say(m_jsonApi.HandleMessage(RemainderAfterTokens(line, 2)));
}

void ConnectorPlugin::CmdEvents(const std::vector<std::string>& tokens)
{
    const std::string a = tokens.size() > 2 ? Lower(tokens[2]) : "status";
    const std::string b = tokens.size() > 3 ? Lower(tokens[3]) : "";

    if (a == "on")
        m_flightEvents = true;
    else if (a == "off")
        m_flightEvents = m_positionEvents = false;
    else if (a == "pos" && b == "on")
        m_positionEvents = true;
    else if (a == "pos" && b == "off")
        m_positionEvents = false;
    else if (a != "status")
    {
        Say("Usage: .lpc events <on|off|pos on|pos off|status>");
        return;
    }
    Say(std::string("Event stream: flight events ") +
        (m_flightEvents ? "ON" : "off") + ", position events " +
        (m_positionEvents ? "ON (one per target every few seconds!)" : "off"));
}

// ---------------------------------------------------------------------
// event callbacks -> JSON event stream (chat print + gateway)
// ---------------------------------------------------------------------

void ConnectorPlugin::EmitEvent(const std::string& eventJson, bool isPosition)
{
    if (isPosition ? m_positionEvents : m_flightEvents)
        Say(eventJson);
    if (m_gateway.IsConnected() && (!isPosition || m_gatewayPositions))
        m_gateway.Send(eventJson);
}

void ConnectorPlugin::OnFlightPlanFlightPlanDataUpdate(EuroScopePlugIn::CFlightPlan FlightPlan)
{
    // Cheap guard: don't build JSON nobody consumes.
    if ((!m_flightEvents && !m_gateway.IsConnected()) || !FlightPlan.IsValid())
        return;
    FlightInfo info;
    std::string error;
    if (m_actions.GetFlight(FlightPlan.GetCallsign() ? FlightPlan.GetCallsign() : "", info, error))
        EmitEvent(m_jsonApi.EventFlightUpdated(info), false);
}

void ConnectorPlugin::OnFlightPlanControllerAssignedDataUpdate(
    EuroScopePlugIn::CFlightPlan FlightPlan, int /*DataType*/)
{
    // Same event as filed-data changes: consumers get the full, fresh
    // flight object either way and don't need to care which side changed.
    OnFlightPlanFlightPlanDataUpdate(FlightPlan);
}

void ConnectorPlugin::OnFlightPlanDisconnect(EuroScopePlugIn::CFlightPlan FlightPlan)
{
    if ((!m_flightEvents && !m_gateway.IsConnected()) || !FlightPlan.IsValid())
        return;
    const char* callsign = FlightPlan.GetCallsign();
    EmitEvent(m_jsonApi.EventFlightRemoved(callsign ? callsign : ""), false);
}

void ConnectorPlugin::OnRadarTargetPositionUpdate(EuroScopePlugIn::CRadarTarget RadarTarget)
{
    const bool anyConsumer =
        m_positionEvents || (m_gateway.IsConnected() && m_gatewayPositions);
    if (!anyConsumer || !RadarTarget.IsValid())
        return;
    PositionUpdate pos;
    const char* callsign = RadarTarget.GetCallsign();
    pos.callsign = callsign ? callsign : "";
    pos.latitude = RadarTarget.GetPosition().GetPosition().m_Latitude;
    pos.longitude = RadarTarget.GetPosition().GetPosition().m_Longitude;
    pos.flightLevel = RadarTarget.GetPosition().GetFlightLevel();
    pos.groundSpeed = RadarTarget.GetPosition().GetReportedGS();
    const char* squawk = RadarTarget.GetPosition().GetSquawk();
    pos.squawk = squawk ? squawk : "";
    EmitEvent(m_jsonApi.EventPositionUpdated(pos), true);
}

// ---------------------------------------------------------------------
// gateway pump (1 Hz) and command
// ---------------------------------------------------------------------

void ConnectorPlugin::OnTimer(int /*Counter*/)
{
    const bool wasConnected = m_gateway.IsConnected();

    bool justConnected = false;
    const std::vector<std::string> inbound = m_gateway.Tick(justConnected);

    if (justConnected)
    {
        Say("Gateway connected: " + m_gateway.GetUrl());
        // Snapshot first, so the backend has the full state before any
        // incremental events or command responses arrive.
        m_gateway.Send(m_jsonApi.EventSessionSnapshot(m_actions.CollectFlights("")));
    }
    else if (wasConnected && !m_gateway.IsConnected() && m_gateway.IsEnabled())
    {
        Say("Gateway connection lost (" + m_gateway.GetStatus().lastError +
            ") - reconnecting with backoff.");
    }

    for (const std::string& message : inbound)
        m_gateway.Send(m_jsonApi.HandleMessage(message));

    // Pump the chat-injection queue AFTER the inbound commands so a send
    // queued by a gateway command dispatches this same tick; outcomes of
    // sends dispatched on earlier ticks are reported here (deferred
    // verification - see ChatInjection.h).
    for (const ChatInjection::Outcome& outcome : ChatInjection::Pump())
        Say(outcome.detail);
}

void ConnectorPlugin::CmdGateway(const std::vector<std::string>& tokens)
{
    const std::string a = tokens.size() > 2 ? Lower(tokens[2]) : "status";
    const std::string b = tokens.size() > 3 ? tokens[3] : "";

    if (a == "config")
    {
        // EuroScope's command line does not pass ':' through, so URL and
        // token arrive together as base64("<url>:<token>").
        if (b.empty())
        {
            Say("Usage: .lpc gateway config <base64 of url:token>");
            Say("Generate it from 'https://host[:port]/base:<bearer-token>' "
                "(your backend usually shows it ready to copy).");
            return;
        }
        const GatewayConfig config = ParseGatewayConfig(b);
        if (!config.error.empty())
        {
            Say("Invalid gateway config: " + config.error);
            return;
        }
        const std::string error = m_gateway.SetUrl(config.url);
        if (!error.empty())
        {
            Say("Invalid gateway URL in config: " + error);
            return;
        }
        m_gateway.SetToken(config.token);
        SaveSetting("GatewayUrl", "Backend base URL (https)", config.url);
        SaveSetting("GatewayToken", "Backend bearer token", config.token);
        Say("Gateway configured: " + config.url + ", token set" +
            (m_gateway.IsEnabled() ? " (reconnecting)"
                                   : " - '.lpc gateway connect' to connect"));
    }
    else if (a == "url")
    {
        if (b.empty())
        {
            Say("Usage: .lpc gateway url https://host[:port]/base-path");
            return;
        }
        const std::string error = m_gateway.SetUrl(b);
        if (!error.empty())
        {
            Say("Invalid gateway URL: " + error);
            return;
        }
        SaveSetting("GatewayUrl", "Backend base URL (https)", b);
        Say("Gateway URL set to " + b +
            (m_gateway.IsEnabled() ? " (reconnecting)" : " - '.lpc gateway connect' to connect"));
    }
    else if (a == "token")
    {
        if (b.empty())
        {
            Say("Usage: .lpc gateway token <bearer-token>");
            return;
        }
        m_gateway.SetToken(b);
        SaveSetting("GatewayToken", "Backend bearer token", b);
        Say("Gateway token set (stored in the EuroScope settings file - "
            "use a token you can revoke).");
    }
    else if (a == "connect")
    {
        const std::string error = m_gateway.Enable();
        Say(error.empty() ? "Connecting to " + m_gateway.GetUrl() + " ..." : error);
    }
    else if (a == "disconnect")
    {
        m_gateway.Disable();
        Say("Gateway disconnected.");
    }
    else if (a == "auto")
    {
        if (b != "on" && b != "off")
        {
            Say("Usage: .lpc gateway auto <on|off>");
            return;
        }
        SaveSetting("GatewayAuto", "Auto-connect to the gateway on load",
                    b == "on" ? "1" : "0");
        Say(std::string("Gateway auto-connect ") + (b == "on" ? "ON" : "off") +
            " (takes effect on plugin load).");
    }
    else if (a == "pos")
    {
        if (b != "on" && b != "off")
        {
            Say("Usage: .lpc gateway pos <on|off>");
            return;
        }
        m_gatewayPositions = b == "on";
        SaveSetting("GatewayPositions", "Send position events to the gateway",
                    m_gatewayPositions ? "1" : "0");
        Say(std::string("Position events to gateway ") +
            (m_gatewayPositions ? "ON" : "off") + ".");
    }
    else if (a == "status")
    {
        const Gateway::Status s = m_gateway.GetStatus();
        Say("Gateway: " + s.state + "   URL: " + s.url + "   Token: " +
            (m_gateway.HasToken() ? "(set)" : "(not set)"));
        Say("Sent: " + std::to_string(s.sent) +
            "   Received: " + std::to_string(s.received) +
            "   Dropped (while offline): " + std::to_string(s.dropped) +
            "   Positions: " + (m_gatewayPositions ? "on" : "off"));
        if (!s.lastError.empty())
            Say("Last error: " + s.lastError);
    }
    else
    {
        Say("Usage: .lpc gateway <config|url|token|connect|disconnect|auto on|off|pos on|off|status>");
    }
}
