#pragma once

// IActions — the operations the JSON contract (and any other front end)
// can invoke, as an abstract interface.
//
// Implementations:
//   * Actions (src/Actions.h)      — the real, EuroScope-backed one
//   * MockActions (tests/)         — canned data for unit tests
//
// JsonApi depends only on this header (no Windows, no EuroScope SDK), so
// the contract layer compiles and is testable on any platform.

#include <string>
#include <vector>

#include "Types.h"

class IActions
{
public:
    virtual ~IActions() = default;

    // --- reads ---------------------------------------------------------

    virtual std::vector<FlightInfo> CollectFlights(const std::string& filter) const = 0;
    virtual bool GetFlight(const std::string& callsign, FlightInfo& out,
                           std::string& error) const = 0;

    // --- writes --------------------------------------------------------

    virtual ActionResult SetClearedAltitude(const std::string& callsign, int feet) = 0;
    virtual ActionResult SetFinalAltitude(const std::string& callsign, int feet) = 0;
    virtual ActionResult SetHeading(const std::string& callsign, int degrees) = 0;
    virtual ActionResult SetSpeed(const std::string& callsign, int knots) = 0;
    virtual ActionResult SetMach(const std::string& callsign, int hundredths) = 0;
    virtual ActionResult SetRate(const std::string& callsign, int feetPerMinute) = 0;
    virtual ActionResult SetSquawk(const std::string& callsign, const std::string& code) = 0;
    virtual ActionResult SetDirect(const std::string& callsign, const std::string& point) = 0;
    virtual ActionResult SetScratchPad(const std::string& callsign, const std::string& text) = 0;
    virtual ActionResult BroadcastScratchPadToken(const std::string& callsign,
                                                  const std::string& token) = 0;
    virtual ActionResult SetSid(const std::string& callsign, const std::string& sid) = 0;
    virtual ActionResult SetStar(const std::string& callsign, const std::string& star) = 0;
};
