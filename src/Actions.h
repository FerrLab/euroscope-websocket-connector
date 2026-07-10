#pragma once

// Actions — a thin, string-in/string-out facade over the EuroScope plugin
// API for every operation this project supports.
//
// Design intent: this is the future RPC surface of the WebSocket connector.
// The command-line front end (ConnectorPlugin) and, later, the WebSocket
// message handlers both call into this class, so behaviour stays identical
// no matter where a request comes from.
//
// Threading: every method here MUST be called on the EuroScope main thread
// (i.e. from inside a CPlugIn callback). The EuroScope API is not
// thread-safe. See docs/DEVELOPER-GUIDE.md.

#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "EuroScopePlugIn.h"

// Result of a single action. `ok` says whether EuroScope accepted the
// operation; `message` is a human-readable outcome (also suitable to be
// serialised into a WebSocket response later).
struct ActionResult
{
    bool ok = false;
    std::string message;

    static ActionResult Ok(const std::string& msg) { return { true, msg }; }
    static ActionResult Fail(const std::string& msg) { return { false, msg }; }
};

class Actions
{
public:
    // The plugin instance is borrowed, not owned. It outlives this object
    // (both are destroyed together in EuroScopePlugInExit).
    explicit Actions(EuroScopePlugIn::CPlugIn* plugin) : m_es(plugin) {}

    // --- capability 1: list flights -----------------------------------

    // One formatted line per flight plan known to this EuroScope session.
    // `filter` (optional, case-insensitive) keeps flights whose callsign
    // starts with it, or whose origin/destination equals it.
    // At most `maxLines` lines are returned; `totalMatched` receives the
    // real match count so callers can print "(+N more)".
    std::vector<std::string> ListFlights(const std::string& filter,
                                         size_t maxLines,
                                         size_t& totalMatched) const;

    // Multi-line ('\n'-separated) detail dump of one flight plan.
    ActionResult DescribeFlight(const std::string& callsign) const;

    // --- capability 2: modify flight plan parameters ------------------

    ActionResult SetClearedAltitude(const std::string& callsign, int feet);
    ActionResult SetFinalAltitude(const std::string& callsign, int feet);
    ActionResult SetHeading(const std::string& callsign, int degrees);
    ActionResult SetSpeed(const std::string& callsign, int knots);
    ActionResult SetMach(const std::string& callsign, int hundredths);
    ActionResult SetRate(const std::string& callsign, int feetPerMinute);
    ActionResult SetSquawk(const std::string& callsign, const std::string& code);
    ActionResult SetDirect(const std::string& callsign, const std::string& point);

    // --- capability 5: scratch pad ------------------------------------

    // Persistently replaces the scratch pad content.
    ActionResult SetScratchPad(const std::string& callsign, const std::string& text);

    // Broadcasts one of EuroScope's magic scratch-pad tokens (STUP, PUSH,
    // TAXI, DEPA, TXIN, PARK, NSTS, CLEA, NOTC, ARR) and restores the
    // previous scratch pad content afterwards. This is the community-
    // standard way to set ground states / the clearance flag, as the API
    // has no SetGroundState(). See docs/DEVELOPER-GUIDE.md.
    ActionResult BroadcastScratchPadToken(const std::string& callsign,
                                          const std::string& token);

    // --- capability 6: SID / STAR -------------------------------------

    // Assigns a SID/STAR by rewriting the route field and amending the
    // flight plan (there is no SetSid()/SetStar() in the API). The SID may
    // carry a runway suffix ("DKB1A/25R"). Reading SID/STAR is part of
    // DescribeFlight().
    ActionResult SetSid(const std::string& callsign, const std::string& sid);
    ActionResult SetStar(const std::string& callsign, const std::string& star);

    // Tokens accepted by BroadcastScratchPadToken.
    static bool IsKnownScratchPadToken(const std::string& token);

private:
    EuroScopePlugIn::CPlugIn* m_es;

    // Looks up a flight plan; on failure `error` explains why.
    EuroScopePlugIn::CFlightPlan Find(const std::string& callsign,
                                      std::string& error) const;
};
