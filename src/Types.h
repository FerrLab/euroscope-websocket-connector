#pragma once

// Plain data types shared across the project. Deliberately free of any
// Windows / EuroScope dependency so the JSON contract layer (JsonApi) and
// its unit tests compile on any platform.

#include <string>

// Result of a single write action. `ok` says whether EuroScope accepted
// the operation; `message` is a human-readable outcome (also serialised
// into JSON responses).
struct ActionResult
{
    bool ok = false;
    std::string message;

    static ActionResult Ok(const std::string& msg) { return { true, msg }; }
    static ActionResult Fail(const std::string& msg) { return { false, msg }; }
};

// Snapshot of one flight, decoupled from EuroScope handle objects so it can
// be formatted, serialised or queued freely. Numeric fields use 0 for
// "unassigned" (EuroScope convention); clearedAltitude additionally uses
// the special values 1 = cleared ILS approach, 2 = cleared visual approach.
struct FlightInfo
{
    std::string callsign;
    std::string planType;      // "I", "V", ...
    std::string aircraftType;  // e.g. "A320"
    char wtc = ' ';            // wake turbulence category letter

    std::string origin;
    std::string destination;
    std::string alternate;
    std::string departureRunway;
    std::string arrivalRunway;
    std::string sid;
    std::string star;
    std::string route;
    std::string remarks;

    int finalAltitude = 0;    // RFL, feet
    int clearedAltitude = 0;  // CFL, feet (0/1/2 special, see above)

    int assignedHeading = 0;
    int assignedSpeed = 0;   // knots
    int assignedMach = 0;    // hundredths (78 = M0.78)
    int assignedRate = 0;    // feet/minute
    std::string directTo;
    std::string assignedSquawk;
    std::string scratchPad;

    std::string groundState;  // "", "ST-UP", "PUSH", "TAXI", "DEPA", ...
    bool clearanceFlag = false;

    std::string trackingController;  // callsign, empty if untracked
    bool trackedByMe = false;
    // Callsign of the controller a handoff is being offered to; empty when
    // no handoff is in progress.
    std::string handoffTargetController;
    char communicationType = ' ';  // 'v', 'r', 't' (or unassigned)

    // Radar-derived; valid only when correlated == true.
    bool correlated = false;
    std::string transponderSquawk;
    int flightLevel = 0;  // feet (pressure-corrected level from the target)
    int groundSpeed = 0;  // knots
    double latitude = 0.0;
    double longitude = 0.0;
};

// Snapshot of one online ATC position (or observer), decoupled from
// EuroScope handle objects. Facility and rating are EuroScope's numeric
// codes (documented in docs/PROTOCOL.md); labels are presentation.
struct ControllerInfo
{
    std::string callsign;    // e.g. "EDDM_TWR"
    std::string positionId;  // sector-file position ID, e.g. "MT"
    std::string fullName;
    double frequency = 0.0;  // primary frequency in MHz; 0 when none
    int facility = 0;        // 1 FSS, 2 DEL, 3 GND, 4 TWR, 5 APP/DEP, 6 CTR
    int rating = 0;          // 1 OBS ... 12 ADM (VATSIM rating number)
    // Accepted as a controller by the server (may track and modify
    // flights); false for observers.
    bool isController = false;
};

// Lightweight radar position sample, used for position_updated events —
// exists even for radar targets that have no (correlated) flight plan.
struct PositionUpdate
{
    std::string callsign;
    double latitude = 0.0;
    double longitude = 0.0;
    int flightLevel = 0;  // feet
    int groundSpeed = 0;  // knots
    std::string squawk;   // transponder code
};
