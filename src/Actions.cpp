#include "Actions.h"

#include <algorithm>
#include <cctype>
#include <sstream>

using EuroScopePlugIn::CFlightPlan;
using EuroScopePlugIn::CFlightPlanControllerAssignedData;
using EuroScopePlugIn::CFlightPlanData;
using EuroScopePlugIn::CRadarTarget;

namespace
{
    // The EuroScope API returns const char* pointing into internal buffers
    // that may be reused by the next API call. Copy immediately, and guard
    // against NULL.
    std::string S(const char* p) { return p ? std::string(p) : std::string(); }

    std::string Upper(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return s;
    }

    bool StartsWith(const std::string& s, const std::string& prefix)
    {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }

    std::vector<std::string> SplitWords(const std::string& s)
    {
        std::vector<std::string> out;
        std::istringstream in(s);
        std::string w;
        while (in >> w)
            out.push_back(w);
        return out;
    }

    std::string JoinWords(const std::vector<std::string>& words)
    {
        std::string out;
        for (const auto& w : words)
        {
            if (!out.empty())
                out += ' ';
            out += w;
        }
        return out;
    }

    // "DKB1A/25R" -> "DKB1A" (procedure name without runway suffix)
    std::string BaseName(const std::string& token)
    {
        const size_t slash = token.find('/');
        return slash == std::string::npos ? token : token.substr(0, slash);
    }
}

// ---------------------------------------------------------------------
// lookup & snapshot
// ---------------------------------------------------------------------

CFlightPlan Actions::Find(const std::string& callsign, std::string& error) const
{
    CFlightPlan fp = m_es->FlightPlanSelect(Upper(callsign).c_str());
    if (!fp.IsValid())
        error = "No flight plan found for '" + Upper(callsign) +
                "' (flight not in range of this session, or callsign misspelled)";
    return fp;
}

FlightInfo Actions::Snapshot(CFlightPlan fp)
{
    CFlightPlanData fpd = fp.GetFlightPlanData();
    CFlightPlanControllerAssignedData cad = fp.GetControllerAssignedData();

    FlightInfo info;
    info.callsign = S(fp.GetCallsign());
    info.planType = S(fpd.GetPlanType());
    info.aircraftType = S(fpd.GetAircraftFPType());
    info.wtc = fpd.GetAircraftWtc();

    info.origin = S(fpd.GetOrigin());
    info.destination = S(fpd.GetDestination());
    info.alternate = S(fpd.GetAlternate());
    info.departureRunway = S(fpd.GetDepartureRwy());
    info.arrivalRunway = S(fpd.GetArrivalRwy());
    info.sid = S(fpd.GetSidName());
    info.star = S(fpd.GetStarName());
    info.route = S(fpd.GetRoute());
    info.remarks = S(fpd.GetRemarks());

    info.finalAltitude = fp.GetFinalAltitude();
    info.clearedAltitude = fp.GetClearedAltitude();

    info.assignedHeading = cad.GetAssignedHeading();
    info.assignedSpeed = cad.GetAssignedSpeed();
    info.assignedMach = cad.GetAssignedMach();
    info.assignedRate = cad.GetAssignedRate();
    info.directTo = S(cad.GetDirectToPointName());
    info.assignedSquawk = S(cad.GetSquawk());
    info.scratchPad = S(cad.GetScratchPadString());

    info.groundState = S(fp.GetGroundState());
    info.clearanceFlag = fp.GetClearenceFlag();

    info.trackingController = S(fp.GetTrackingControllerCallsign());
    info.trackedByMe = fp.GetTrackingControllerIsMe();
    info.handoffTargetController = S(fp.GetHandoffTargetControllerCallsign());
    info.communicationType = fpd.GetCommunicationType();

    CRadarTarget rt = fp.GetCorrelatedRadarTarget();
    if (rt.IsValid())
    {
        info.correlated = true;
        info.transponderSquawk = S(rt.GetPosition().GetSquawk());
        info.flightLevel = rt.GetPosition().GetFlightLevel();
        info.groundSpeed = rt.GetPosition().GetReportedGS();
        info.latitude = rt.GetPosition().GetPosition().m_Latitude;
        info.longitude = rt.GetPosition().GetPosition().m_Longitude;
    }
    return info;
}

std::vector<FlightInfo> Actions::CollectFlights(const std::string& filter) const
{
    const std::string f = Upper(filter);
    std::vector<FlightInfo> out;

    for (CFlightPlan fp = m_es->FlightPlanSelectFirst(); fp.IsValid();
         fp = m_es->FlightPlanSelectNext(fp))
    {
        if (!f.empty())
        {
            CFlightPlanData fpd = fp.GetFlightPlanData();
            if (!StartsWith(Upper(S(fp.GetCallsign())), f) &&
                Upper(S(fpd.GetOrigin())) != f && Upper(S(fpd.GetDestination())) != f)
                continue;
        }
        out.push_back(Snapshot(fp));
    }
    return out;
}

bool Actions::GetFlight(const std::string& callsign, FlightInfo& out,
                        std::string& error) const
{
    CFlightPlan fp = Find(callsign, error);
    if (!fp.IsValid())
        return false;
    out = Snapshot(fp);
    return true;
}

// ---------------------------------------------------------------------
// capability 2: controller-assigned data
// ---------------------------------------------------------------------
// All these setters return false when EuroScope refuses the change - most
// commonly because we are not connected, or not allowed to modify this
// flight (on VATSIM you generally need to be tracking the aircraft for the
// change to be accepted and distributed).

namespace
{
    ActionResult ReportSet(bool ok, const std::string& what, const std::string& callsign)
    {
        if (ok)
            return ActionResult::Ok(callsign + ": " + what);
        return ActionResult::Fail(callsign + ": EuroScope rejected " + what +
                                  " (not connected, or you are not allowed to modify this flight - try tracking it first)");
    }
}

ActionResult Actions::SetClearedAltitude(const std::string& callsign, int feet)
{
    std::string error;
    CFlightPlan fp = Find(callsign, error);
    if (!fp.IsValid())
        return ActionResult::Fail(error);
    std::string what;
    if (feet == 0)
        what = "cleared altitude cleared (falls back to RFL)";
    else if (feet == 1)
        what = "cleared for ILS approach";
    else if (feet == 2)
        what = "cleared for visual approach";
    else
        what = "cleared altitude set to " + std::to_string(feet) + " ft";
    return ReportSet(fp.GetControllerAssignedData().SetClearedAltitude(feet), what, Upper(callsign));
}

ActionResult Actions::SetFinalAltitude(const std::string& callsign, int feet)
{
    std::string error;
    CFlightPlan fp = Find(callsign, error);
    if (!fp.IsValid())
        return ActionResult::Fail(error);
    return ReportSet(fp.GetControllerAssignedData().SetFinalAltitude(feet),
                     "final (requested) altitude set to " + std::to_string(feet) + " ft",
                     Upper(callsign));
}

ActionResult Actions::SetHeading(const std::string& callsign, int degrees)
{
    std::string error;
    CFlightPlan fp = Find(callsign, error);
    if (!fp.IsValid())
        return ActionResult::Fail(error);
    const std::string what = degrees == 0 ? "assigned heading cleared"
                                          : "heading set to " + std::to_string(degrees);
    return ReportSet(fp.GetControllerAssignedData().SetAssignedHeading(degrees), what, Upper(callsign));
}

ActionResult Actions::SetSpeed(const std::string& callsign, int knots)
{
    std::string error;
    CFlightPlan fp = Find(callsign, error);
    if (!fp.IsValid())
        return ActionResult::Fail(error);
    const std::string what = knots == 0 ? "assigned speed cleared"
                                        : "speed set to " + std::to_string(knots) + " kts";
    return ReportSet(fp.GetControllerAssignedData().SetAssignedSpeed(knots), what, Upper(callsign));
}

ActionResult Actions::SetMach(const std::string& callsign, int hundredths)
{
    std::string error;
    CFlightPlan fp = Find(callsign, error);
    if (!fp.IsValid())
        return ActionResult::Fail(error);
    const std::string what = hundredths == 0
                                 ? "assigned mach cleared"
                                 : "mach set to 0." + std::to_string(hundredths);
    return ReportSet(fp.GetControllerAssignedData().SetAssignedMach(hundredths), what, Upper(callsign));
}

ActionResult Actions::SetRate(const std::string& callsign, int feetPerMinute)
{
    std::string error;
    CFlightPlan fp = Find(callsign, error);
    if (!fp.IsValid())
        return ActionResult::Fail(error);
    const std::string what = feetPerMinute == 0
                                 ? "assigned rate cleared"
                                 : "rate set to " + std::to_string(feetPerMinute) + " fpm";
    return ReportSet(fp.GetControllerAssignedData().SetAssignedRate(feetPerMinute), what, Upper(callsign));
}

ActionResult Actions::SetSquawk(const std::string& callsign, const std::string& code)
{
    if (code.size() != 4 ||
        code.find_first_not_of("01234567") != std::string::npos)
        return ActionResult::Fail("'" + code + "' is not a valid squawk (4 octal digits, e.g. 2354)");

    std::string error;
    CFlightPlan fp = Find(callsign, error);
    if (!fp.IsValid())
        return ActionResult::Fail(error);
    return ReportSet(fp.GetControllerAssignedData().SetSquawk(code.c_str()),
                     "squawk set to " + code, Upper(callsign));
}

ActionResult Actions::SetDirect(const std::string& callsign, const std::string& point)
{
    std::string error;
    CFlightPlan fp = Find(callsign, error);
    if (!fp.IsValid())
        return ActionResult::Fail(error);
    const std::string p = Upper(point);
    return ReportSet(fp.GetControllerAssignedData().SetDirectToPointName(p.c_str()),
                     "direct to " + p, Upper(callsign));
}

// ---------------------------------------------------------------------
// capability 5: scratch pad
// ---------------------------------------------------------------------

ActionResult Actions::SetScratchPad(const std::string& callsign, const std::string& text)
{
    std::string error;
    CFlightPlan fp = Find(callsign, error);
    if (!fp.IsValid())
        return ActionResult::Fail(error);
    const std::string what = text.empty() ? "scratch pad cleared"
                                          : "scratch pad set to '" + text + "'";
    return ReportSet(fp.GetControllerAssignedData().SetScratchPadString(text.c_str()),
                     what, Upper(callsign));
}

bool Actions::IsKnownScratchPadToken(const std::string& token)
{
    // Official list: https://www.euroscope.hu/wp/non-standard-extensions/
    static const char* kTokens[] = {
        "NSTS", "STUP", "PUSH", "TAXI", "DEPA", // departure ground states
        "TXIN", "PARK",                          // arrival ground states
        "CLEA", "NOTC",                          // clearance received flag
        "ARR",                                   // mark as arrival traffic
    };
    for (const char* t : kTokens)
        if (token == t)
            return true;
    return false;
}

ActionResult Actions::BroadcastScratchPadToken(const std::string& callsign,
                                               const std::string& tokenIn)
{
    const std::string token = Upper(tokenIn);
    if (!IsKnownScratchPadToken(token))
        return ActionResult::Fail(
            "'" + tokenIn + "' is not a known token. Valid: NSTS STUP PUSH TAXI DEPA TXIN PARK CLEA NOTC ARR");

    std::string error;
    CFlightPlan fp = Find(callsign, error);
    if (!fp.IsValid())
        return ActionResult::Fail(error);

    CFlightPlanControllerAssignedData cad = fp.GetControllerAssignedData();

    // Standard community pattern (see PushbackFlorian, vACDM): write the
    // magic token so EuroScope interprets and distributes it, then restore
    // whatever the controller had in the scratch pad before.
    const std::string previous = S(cad.GetScratchPadString());
    if (!cad.SetScratchPadString(token.c_str()))
        return ActionResult::Fail(Upper(callsign) +
                                  ": EuroScope rejected the scratch pad update (not connected, or no permission)");
    cad.SetScratchPadString(previous.c_str());

    return ActionResult::Ok(Upper(callsign) + ": broadcast '" + token +
                            "' (scratch pad restored to '" + previous + "')");
}

// ---------------------------------------------------------------------
// capability 6: SID / STAR
// ---------------------------------------------------------------------
// The API exposes GetSidName()/GetStarName() but no setters. The working
// technique (used e.g. by StripCol and several vACC plugins) is to rewrite
// the route so it starts with the SID (or ends with the STAR) and then call
// AmendFlightPlan() to publish the change. EuroScope re-extracts the
// procedure from the route; the name must exist in the loaded sector file
// for the assignment to display properly.

ActionResult Actions::SetSid(const std::string& callsign, const std::string& sidIn)
{
    std::string error;
    CFlightPlan fp = Find(callsign, error);
    if (!fp.IsValid())
        return ActionResult::Fail(error);

    CFlightPlanData fpd = fp.GetFlightPlanData();
    const std::string sid = Upper(sidIn);
    const std::string currentSid = S(fpd.GetSidName());

    std::vector<std::string> words = SplitWords(S(fpd.GetRoute()));

    // Drop the leading token when it is the currently assigned SID
    // (possibly carrying a "/RWY" suffix).
    if (!words.empty() && !currentSid.empty() &&
        BaseName(Upper(words.front())) == Upper(currentSid))
        words.erase(words.begin());

    const std::string newRoute = words.empty() ? sid : sid + " " + JoinWords(words);

    if (!fpd.SetRoute(newRoute.c_str()))
        return ActionResult::Fail(Upper(callsign) + ": EuroScope rejected the route change");
    if (!fpd.AmendFlightPlan())
        return ActionResult::Fail(Upper(callsign) +
                                  ": route changed locally but AmendFlightPlan failed (not connected?)");

    return ActionResult::Ok(Upper(callsign) + ": SID " + sid + " - new route: " + newRoute);
}

ActionResult Actions::SetStar(const std::string& callsign, const std::string& starIn)
{
    std::string error;
    CFlightPlan fp = Find(callsign, error);
    if (!fp.IsValid())
        return ActionResult::Fail(error);

    CFlightPlanData fpd = fp.GetFlightPlanData();
    const std::string star = Upper(starIn);
    const std::string currentStar = S(fpd.GetStarName());

    std::vector<std::string> words = SplitWords(S(fpd.GetRoute()));

    // Drop the trailing token when it is the currently assigned STAR.
    if (!words.empty() && !currentStar.empty() &&
        BaseName(Upper(words.back())) == Upper(currentStar))
        words.pop_back();

    const std::string newRoute = words.empty() ? star : JoinWords(words) + " " + star;

    if (!fpd.SetRoute(newRoute.c_str()))
        return ActionResult::Fail(Upper(callsign) + ": EuroScope rejected the route change");
    if (!fpd.AmendFlightPlan())
        return ActionResult::Fail(Upper(callsign) +
                                  ": route changed locally but AmendFlightPlan failed (not connected?)");

    return ActionResult::Ok(Upper(callsign) + ": STAR " + star + " - new route: " + newRoute);
}

// ---------------------------------------------------------------------
// capability 7: track control (assume / release / transfer)
// ---------------------------------------------------------------------
// "Assume" and "release" double as the handoff accept/refuse when a
// handoff is currently being offered to me - mirroring what the ASSUME
// button does in EuroScope's tag.

namespace
{
    bool HandoffOfferedToMe(EuroScopePlugIn::CPlugIn* es,
                            EuroScopePlugIn::CFlightPlan fp)
    {
        const std::string myself = S(es->ControllerMyself().GetCallsign());
        return !myself.empty() &&
               S(fp.GetHandoffTargetControllerCallsign()) == myself;
    }
}

ActionResult Actions::AssumeTrack(const std::string& callsign)
{
    std::string error;
    CFlightPlan fp = Find(callsign, error);
    if (!fp.IsValid())
        return ActionResult::Fail(error);
    const std::string cs = Upper(callsign);

    if (HandoffOfferedToMe(m_es, fp))
    {
        fp.AcceptHandoff();
        return ActionResult::Ok(cs + ": handoff accepted");
    }
    if (fp.GetTrackingControllerIsMe())
        return ActionResult::Ok(cs + ": already tracked by you");
    if (!fp.StartTracking())
        return ActionResult::Fail(
            cs + ": EuroScope refused to assume (not connected, or the "
                 "flight is tracked by another controller)");
    return ActionResult::Ok(cs + ": track assumed");
}

ActionResult Actions::ReleaseTrack(const std::string& callsign)
{
    std::string error;
    CFlightPlan fp = Find(callsign, error);
    if (!fp.IsValid())
        return ActionResult::Fail(error);
    const std::string cs = Upper(callsign);

    if (HandoffOfferedToMe(m_es, fp))
    {
        fp.RefuseHandoff();
        return ActionResult::Ok(cs + ": handoff refused");
    }
    if (!fp.GetTrackingControllerIsMe())
        return ActionResult::Fail(cs + ": you are not tracking this flight");
    if (!fp.EndTracking())
        return ActionResult::Fail(
            cs + ": EuroScope refused to release the track (not connected?)");
    return ActionResult::Ok(cs + ": track released");
}

ActionResult Actions::TransferTrack(const std::string& callsign,
                                    const std::string& controller)
{
    std::string error;
    CFlightPlan fp = Find(callsign, error);
    if (!fp.IsValid())
        return ActionResult::Fail(error);
    const std::string cs = Upper(callsign);
    const std::string target = Upper(controller);

    EuroScopePlugIn::CController ctrl = m_es->ControllerSelect(target.c_str());
    if (!ctrl.IsValid())
        ctrl = m_es->ControllerSelectByPositionId(target.c_str());
    if (!ctrl.IsValid())
        return ActionResult::Fail(
            cs + ": no controller '" + target +
            "' online (use the callsign or the position ID - see '.lpc atc')");
    if (!ctrl.IsController())
        return ActionResult::Fail(cs + ": '" + S(ctrl.GetCallsign()) +
                                  "' is an observer and cannot receive a handoff");
    if (!fp.GetTrackingControllerIsMe())
        return ActionResult::Fail(
            cs + ": you can only transfer a flight you are tracking - assume it first");
    if (!fp.InitiateHandoff(S(ctrl.GetCallsign()).c_str()))
        return ActionResult::Fail(cs + ": EuroScope refused to initiate the handoff");
    return ActionResult::Ok(cs + ": handoff initiated to " + S(ctrl.GetCallsign()));
}

// ---------------------------------------------------------------------
// capability 8: ATC list
// ---------------------------------------------------------------------

ControllerInfo Actions::SnapshotController(EuroScopePlugIn::CController c)
{
    ControllerInfo info;
    info.callsign = S(c.GetCallsign());
    info.positionId = S(c.GetPositionId());
    info.fullName = S(c.GetFullName());
    // The API reports 199.980 when no primary frequency is selected.
    const double frequency = c.GetPrimaryFrequency();
    info.frequency = frequency >= 199.0 ? 0.0 : frequency;
    info.facility = c.GetFacility();
    info.rating = c.GetRating();
    info.isController = c.IsController();
    return info;
}

std::vector<ControllerInfo> Actions::CollectControllers(const std::string& filter) const
{
    const std::string f = Upper(filter);
    std::vector<ControllerInfo> out;

    for (EuroScopePlugIn::CController c = m_es->ControllerSelectFirst();
         c.IsValid(); c = m_es->ControllerSelectNext(c))
    {
        if (!f.empty() && !StartsWith(Upper(S(c.GetCallsign())), f))
            continue;
        out.push_back(SnapshotController(c));
    }
    return out;
}
