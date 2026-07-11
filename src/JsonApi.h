#pragma once

// JsonApi — the standardized JSON contract for systems interacting with
// the plugin. The full contract (envelope, actions, events, examples,
// error model) is specified in docs/PROTOCOL.md — keep code and spec in
// sync.
//
// Every message, in BOTH directions, uses the same envelope:
//
//   {
//     "type":     "command" | "response" | "event",
//     "id":       <any, optional>,      // command→response correlation
//     "callsign": "ABC1234",            // omitted on session-level messages
//     "action":   "set_squawk",         // the verb (commands/responses)
//                                       // or the event name (events)
//     "payload":  { ... }               // action/event-specific data
//   }
//
//   backend/frontend  --command-->  plugin
//   plugin            --response->  backend/frontend  (one per command)
//   plugin            --event---->  backend/frontend  (unsolicited)
//
// HandleMessage never throws and always returns valid JSON, so a transport
// (the ".lpc json" command or the HTTPS gateway) can pass data through
// blindly. The Event* builders produce the plugin→outside messages; they
// go to the gateway while connected, and ".lpc events on" prints them so
// the stream can be inspected.
//
// Threading: must be called on the EuroScope main thread, because it calls
// straight into the real Actions implementation.
//
// This class depends only on IActions (no Windows/EuroScope headers), so
// the contract logic compiles anywhere and is unit-tested in tests/.

#include <string>

#include "IActions.h"

class JsonApi
{
public:
    // Bump when the contract in docs/PROTOCOL.md changes incompatibly.
    static constexpr int kProtocolVersion = 1;

    explicit JsonApi(IActions& actions) : m_actions(actions) {}

    // Processes one "command" message and returns the "response" message
    // (compact single-line JSON).
    std::string HandleMessage(const std::string& messageJson);

    // --- event builders (plugin -> outside) ---------------------------

    // Flight plan created or changed (filed data or controller-assigned).
    std::string EventFlightUpdated(const FlightInfo& flight) const;
    // Flight plan disappeared from the session.
    std::string EventFlightRemoved(const std::string& callsign) const;
    // New radar position for a target (also for uncorrelated targets).
    std::string EventPositionUpdated(const PositionUpdate& position) const;
    // Full session state; sent right after the gateway transport becomes
    // healthy so the backend can rebuild its world before incremental
    // events resume.
    std::string EventSessionSnapshot(const std::vector<FlightInfo>& flights) const;

private:
    IActions& m_actions;
};
