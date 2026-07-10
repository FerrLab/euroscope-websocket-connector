#include "ConnectorPlugin.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "ChatInjection.h"

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

ConnectorPlugin::ConnectorPlugin()
    : EuroScopePlugIn::CPlugIn(EuroScopePlugIn::COMPATIBILITY_CODE,
                               PLUGIN_NAME,
                               PLUGIN_VERSION,
                               PLUGIN_AUTHOR,
                               PLUGIN_COPYRIGHT),
      m_actions(this)
{
    Say(std::string(PLUGIN_NAME) + " v" PLUGIN_VERSION
        " loaded. Type '.wsc help' for available commands.");
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
    if (tokens.empty() || Lower(tokens[0]) != ".wsc")
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
    else
        Say("Unknown sub-command '" + tokens[1] + "'. Type '.wsc help'.");

    return true; // consume every ".wsc ..." line, even on bad syntax
}

void ConnectorPlugin::CmdHelp()
{
    Say(std::string(PLUGIN_NAME) + " v" PLUGIN_VERSION " - commands (full docs: docs/COMMANDS.md):");
    Say(".wsc list [filter]            - list flight plans (filter: callsign prefix or ICAO of dep/arr)");
    Say(".wsc show <callsign>          - full detail of one flight (incl. SID/STAR, scratch pad)");
    Say(".wsc set <cs> calt <FL240|24000|ils|visual|clear> - cleared altitude");
    Say(".wsc set <cs> rfl <FL340|34000>  - final altitude   | hdg <deg|0> - heading");
    Say(".wsc set <cs> spd <kts|0> | mach <0.78|0> | rate <fpm|0> - speeds/rate");
    Say(".wsc set <cs> sqk <code> | dct <point>   - squawk / direct-to");
    Say(".wsc pad <cs> <text|clear>    - set scratch pad");
    Say(".wsc state <cs> <NSTS|STUP|PUSH|TAXI|DEPA|TXIN|PARK|CLEA|NOTC|ARR> - ground state/clearance flag");
    Say(".wsc sid <cs> <SID[/RWY]> | star <cs> <STAR> - assign SID/STAR");
    Say(".wsc msg <cs> <text>          - private message (experimental, UI injection)");
    Say(".wsc freq <text>              - text to primary frequency (experimental, UI injection)");
}

void ConnectorPlugin::CmdList(const std::vector<std::string>& tokens)
{
    const std::string filter = tokens.size() > 2 ? tokens[2] : "";
    const size_t kMaxLines = 25;

    size_t total = 0;
    std::vector<std::string> lines = m_actions.ListFlights(filter, kMaxLines, total);

    if (total == 0)
    {
        Say(filter.empty() ? "No flight plans in this session."
                           : "No flight plans matching '" + filter + "'.");
        return;
    }
    Say("--- " + std::to_string(total) + " flight(s)" +
        (filter.empty() ? "" : " matching '" + filter + "'") + " ---");
    for (const auto& l : lines)
        Say(l);
    if (total > lines.size())
        Say("(+" + std::to_string(total - lines.size()) +
            " more - narrow it down with '.wsc list <filter>')");
}

void ConnectorPlugin::CmdShow(const std::vector<std::string>& tokens)
{
    if (tokens.size() < 3)
    {
        Say("Usage: .wsc show <callsign>");
        return;
    }
    const ActionResult r = m_actions.DescribeFlight(tokens[2]);
    SayLines(r.message);
}

void ConnectorPlugin::CmdSet(const std::vector<std::string>& tokens)
{
    if (tokens.size() < 5)
    {
        Say("Usage: .wsc set <callsign> <calt|rfl|hdg|spd|mach|rate|sqk|dct> <value>");
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
        Say("Usage: .wsc pad <callsign> <text|clear>");
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
        Say("Usage: .wsc state <callsign> <NSTS|STUP|PUSH|TAXI|DEPA|TXIN|PARK|CLEA|NOTC|ARR>");
        return;
    }
    Say(m_actions.BroadcastScratchPadToken(tokens[2], tokens[3]).message);
}

void ConnectorPlugin::CmdSid(const std::vector<std::string>& tokens, bool star)
{
    if (tokens.size() < 4)
    {
        Say(star ? "Usage: .wsc star <callsign> <STAR>"
                 : "Usage: .wsc sid <callsign> <SID[/RWY]>");
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
        Say("Usage: .wsc msg <callsign> <message text>");
        return;
    }
    const std::string text = RemainderAfterTokens(line, 3);
    // ".msg <callsign> <text>" is EuroScope's own private-message command;
    // we type it into the command line on the controller's behalf.
    const auto r = ChatInjection::SendCommandLine(".msg " + tokens[2] + " " + text);
    Say(r.ok ? "Private message to " + tokens[2] + " handed to EuroScope."
             : "Private message FAILED: " + r.detail);
}

void ConnectorPlugin::CmdFreq(const std::vector<std::string>& tokens, const std::string& line)
{
    if (tokens.size() < 3)
    {
        Say("Usage: .wsc freq <message text>");
        return;
    }
    const std::string text = RemainderAfterTokens(line, 2);
    const auto r = ChatInjection::SendToPrimaryFrequency(text);
    Say(r.ok ? "Text handed to EuroScope for the primary frequency."
             : "Frequency text FAILED: " + r.detail);
}
