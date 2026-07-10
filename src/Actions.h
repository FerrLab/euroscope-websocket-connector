#pragma once

// Actions — the EuroScope-backed implementation of IActions: a thin facade
// over the EuroScope plugin API for every operation this project supports.
//
// Design intent: this is the RPC surface of the connector. Both front ends
// call into it (through IActions), so behaviour stays identical no matter
// where a request comes from:
//
//   * ConnectorPlugin  — ".wsc ..." dot-commands (manual/testing)
//   * JsonApi          — the standardized JSON contract (docs/PROTOCOL.md),
//                        later carried over WebSocket
//
// Reads return plain structs (FlightInfo); writes return ActionResult.
// No formatting and no JSON in here — presentation belongs to the callers.
//
// Threading: every method here MUST be called on the EuroScope main thread
// (i.e. from inside a CPlugIn callback). The EuroScope API is not
// thread-safe. See docs/DEVELOPER-GUIDE.md.

#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "EuroScopePlugIn.h"

#include "IActions.h"

class Actions : public IActions
{
public:
    // The plugin instance is borrowed, not owned. It outlives this object
    // (both are destroyed together in EuroScopePlugInExit).
    explicit Actions(EuroScopePlugIn::CPlugIn* plugin) : m_es(plugin) {}

    // --- capability 1: read flights ------------------------------------

    // Snapshot of every flight plan known to this EuroScope session.
    // `filter` (optional, case-insensitive) keeps flights whose callsign
    // starts with it, or whose origin/destination equals it.
    std::vector<FlightInfo> CollectFlights(const std::string& filter) const override;

    // Snapshot of one flight; returns false (with `error` set) when the
    // callsign is unknown to the session.
    bool GetFlight(const std::string& callsign, FlightInfo& out,
                   std::string& error) const override;

    // --- capability 2: modify flight plan parameters ------------------

    ActionResult SetClearedAltitude(const std::string& callsign, int feet) override;
    ActionResult SetFinalAltitude(const std::string& callsign, int feet) override;
    ActionResult SetHeading(const std::string& callsign, int degrees) override;
    ActionResult SetSpeed(const std::string& callsign, int knots) override;
    ActionResult SetMach(const std::string& callsign, int hundredths) override;
    ActionResult SetRate(const std::string& callsign, int feetPerMinute) override;
    ActionResult SetSquawk(const std::string& callsign, const std::string& code) override;
    ActionResult SetDirect(const std::string& callsign, const std::string& point) override;

    // --- capability 5: scratch pad ------------------------------------

    // Persistently replaces the scratch pad content.
    ActionResult SetScratchPad(const std::string& callsign, const std::string& text) override;

    // Broadcasts one of EuroScope's magic scratch-pad tokens (STUP, PUSH,
    // TAXI, DEPA, TXIN, PARK, NSTS, CLEA, NOTC, ARR) and restores the
    // previous scratch pad content afterwards. This is the community-
    // standard way to set ground states / the clearance flag, as the API
    // has no SetGroundState(). See docs/DEVELOPER-GUIDE.md.
    ActionResult BroadcastScratchPadToken(const std::string& callsign,
                                          const std::string& token) override;

    // --- capability 6: SID / STAR -------------------------------------

    // Assigns a SID/STAR by rewriting the route field and amending the
    // flight plan (there is no SetSid()/SetStar() in the API). The SID may
    // carry a runway suffix ("DKB1A/25R"). Reading SID/STAR is part of
    // FlightInfo.
    ActionResult SetSid(const std::string& callsign, const std::string& sid) override;
    ActionResult SetStar(const std::string& callsign, const std::string& star) override;

    // Tokens accepted by BroadcastScratchPadToken.
    static bool IsKnownScratchPadToken(const std::string& token);

private:
    EuroScopePlugIn::CPlugIn* m_es;

    // Looks up a flight plan; on failure `error` explains why.
    EuroScopePlugIn::CFlightPlan Find(const std::string& callsign,
                                      std::string& error) const;

    static FlightInfo Snapshot(EuroScopePlugIn::CFlightPlan fp);
};
