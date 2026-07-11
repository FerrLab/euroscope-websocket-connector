// Unit tests for the JSON contract layer (src/JsonApi.cpp).
//
// JsonApi depends only on IActions, so these tests run on ANY platform —
// no Windows, no EuroScope. They cover the contract logic: envelope
// validation, action dispatch, payload validation, serialization, error
// model and the event builders. The EuroScope-facing implementation
// (src/Actions.cpp) can only be exercised inside EuroScope itself.
//
// Build & run (any OS with a C++17 compiler):
//   g++ -std=c++17 -I ../src -I ../third_party \
//       json_api_test.cpp mock_chat_injection.cpp ../src/JsonApi.cpp \
//       -o json_api_test && ./json_api_test
// or use tests/CMakeLists.txt (see docs/DEVELOPER-GUIDE.md).

#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "IActions.h"
#include "JsonApi.h"
#include "mock_chat_injection.h"

using nlohmann::json;

// ---------------------------------------------------------------------
// tiny assertion helper
// ---------------------------------------------------------------------

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                        \
    do                                                                     \
    {                                                                      \
        ++g_checks;                                                        \
        if (!(cond))                                                       \
        {                                                                  \
            ++g_failures;                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  "    \
                      << #cond << "\n";                                    \
        }                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do                                                                     \
    {                                                                      \
        ++g_checks;                                                        \
        if (!((a) == (b)))                                                 \
        {                                                                  \
            ++g_failures;                                                  \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  "    \
                      << #a << " == " << #b << "  (got: " << (a) << ")\n"; \
        }                                                                  \
    } while (0)

// ---------------------------------------------------------------------
// mock IActions — records calls, returns canned data
// ---------------------------------------------------------------------

class MockActions : public IActions
{
public:
    // last-call recording
    mutable std::string lastMethod;
    mutable std::string lastCallsign;
    mutable std::string lastString;
    mutable int lastInt = 0;

    bool failNext = false;  // make the next write action fail

    static FlightInfo SampleFlight(const std::string& callsign)
    {
        FlightInfo f;
        f.callsign = callsign;
        f.planType = "I";
        f.aircraftType = "A320";
        f.wtc = 'M';
        f.origin = "EDDM";
        f.destination = "EDDF";
        f.sid = "GIVMI1N";
        f.star = "ROKIL3A";
        f.route = "GIVMI1N GIVMI Y101 ERNAS T161 ROKIL3A";
        f.finalAltitude = 24000;
        f.clearedAltitude = 12000;
        f.assignedSquawk = "1000";
        f.groundState = "TAXI";
        f.clearanceFlag = true;
        f.trackingController = "EDDM_TWR";
        f.trackedByMe = true;
        f.communicationType = 'v';
        f.correlated = true;
        f.transponderSquawk = "1000";
        f.flightLevel = 1200;
        f.groundSpeed = 18;
        f.latitude = 48.3538;
        f.longitude = 11.7861;
        return f;
    }

    std::vector<FlightInfo> CollectFlights(const std::string& filter) const override
    {
        lastMethod = "CollectFlights";
        lastString = filter;
        return { SampleFlight("DLH4TX"), SampleFlight("BAW23K") };
    }

    bool GetFlight(const std::string& callsign, FlightInfo& out,
                   std::string& error) const override
    {
        lastMethod = "GetFlight";
        lastCallsign = callsign;
        if (callsign == "NOBODY")
        {
            error = "No flight plan found for 'NOBODY'";
            return false;
        }
        out = SampleFlight(callsign);
        return true;
    }

    static ControllerInfo SampleController(const std::string& callsign)
    {
        ControllerInfo c;
        c.callsign = callsign;
        c.positionId = "MT";
        c.fullName = "Jane Doe";
        c.frequency = 119.6;
        c.facility = 4;  // TWR
        c.rating = 5;    // C1
        c.isController = true;
        return c;
    }

    std::vector<ControllerInfo> CollectControllers(const std::string& filter) const override
    {
        lastMethod = "CollectControllers";
        lastString = filter;
        return { SampleController("EDDM_TWR"), SampleController("EDDF_APP") };
    }

private:
    ActionResult Record(const std::string& method, const std::string& callsign)
    {
        lastMethod = method;
        lastCallsign = callsign;
        if (failNext)
        {
            failNext = false;
            return ActionResult::Fail(callsign + ": EuroScope rejected " + method);
        }
        return ActionResult::Ok(callsign + ": " + method + " ok");
    }

public:
    ActionResult SetClearedAltitude(const std::string& cs, int feet) override
    {
        lastInt = feet;
        return Record("SetClearedAltitude", cs);
    }
    ActionResult SetFinalAltitude(const std::string& cs, int feet) override
    {
        lastInt = feet;
        return Record("SetFinalAltitude", cs);
    }
    ActionResult SetHeading(const std::string& cs, int degrees) override
    {
        lastInt = degrees;
        return Record("SetHeading", cs);
    }
    ActionResult SetSpeed(const std::string& cs, int knots) override
    {
        lastInt = knots;
        return Record("SetSpeed", cs);
    }
    ActionResult SetMach(const std::string& cs, int hundredths) override
    {
        lastInt = hundredths;
        return Record("SetMach", cs);
    }
    ActionResult SetRate(const std::string& cs, int fpm) override
    {
        lastInt = fpm;
        return Record("SetRate", cs);
    }
    ActionResult SetSquawk(const std::string& cs, const std::string& code) override
    {
        lastString = code;
        return Record("SetSquawk", cs);
    }
    ActionResult SetDirect(const std::string& cs, const std::string& point) override
    {
        lastString = point;
        return Record("SetDirect", cs);
    }
    ActionResult SetScratchPad(const std::string& cs, const std::string& text) override
    {
        lastString = text;
        return Record("SetScratchPad", cs);
    }
    ActionResult BroadcastScratchPadToken(const std::string& cs,
                                          const std::string& token) override
    {
        lastString = token;
        return Record("BroadcastScratchPadToken", cs);
    }
    ActionResult SetSid(const std::string& cs, const std::string& sid) override
    {
        lastString = sid;
        return Record("SetSid", cs);
    }
    ActionResult SetStar(const std::string& cs, const std::string& star) override
    {
        lastString = star;
        return Record("SetStar", cs);
    }
    ActionResult AssumeTrack(const std::string& cs) override
    {
        return Record("AssumeTrack", cs);
    }
    ActionResult ReleaseTrack(const std::string& cs) override
    {
        return Record("ReleaseTrack", cs);
    }
    ActionResult TransferTrack(const std::string& cs,
                               const std::string& controller) override
    {
        lastString = controller;
        return Record("TransferTrack", cs);
    }
};

// ---------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------

static json Run(JsonApi& api, const std::string& request)
{
    const std::string out = api.HandleMessage(request);
    json j = json::parse(out, nullptr, false);
    CHECK(!j.is_discarded());  // every response must be valid JSON
    return j;
}

// ---------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------

int main()
{
    MockActions mock;
    JsonApi api(mock);

    // --- envelope basics ---------------------------------------------

    {
        // ping: session-level, no callsign needed
        json r = Run(api, R"({"type":"command","action":"ping"})");
        CHECK_EQ(r.at("type").get<std::string>(), "response");
        CHECK(r.at("ok").get<bool>());
        CHECK_EQ(r.at("action").get<std::string>(), "ping");
        CHECK_EQ(r.at("payload").at("protocolVersion").get<int>(), JsonApi::kProtocolVersion);
    }
    {
        // malformed JSON
        json r = Run(api, "this is not json {{{");
        CHECK(!r.at("ok").get<bool>());
        CHECK(r.contains("error"));
    }
    {
        // not an object
        json r = Run(api, "[1,2,3]");
        CHECK(!r.at("ok").get<bool>());
    }
    {
        // missing type
        json r = Run(api, R"({"action":"ping"})");
        CHECK(!r.at("ok").get<bool>());
        CHECK(r.at("error").get<std::string>().find("type") != std::string::npos);
    }
    {
        // wrong type
        json r = Run(api, R"({"type":"event","action":"ping"})");
        CHECK(!r.at("ok").get<bool>());
    }
    {
        // unknown action
        json r = Run(api, R"({"type":"command","action":"fly_to_the_moon"})");
        CHECK(!r.at("ok").get<bool>());
        CHECK(r.at("error").get<std::string>().find("fly_to_the_moon") != std::string::npos);
    }
    {
        // missing action
        json r = Run(api, R"({"type":"command"})");
        CHECK(!r.at("ok").get<bool>());
    }
    {
        // id echo: string and number
        json r1 = Run(api, R"({"type":"command","id":"req-1","action":"ping"})");
        CHECK_EQ(r1.at("id").get<std::string>(), "req-1");
        json r2 = Run(api, R"({"type":"command","id":42,"action":"ping"})");
        CHECK_EQ(r2.at("id").get<int>(), 42);
        // id echoed even on errors
        json r3 = Run(api, R"({"type":"command","id":7,"action":"nope"})");
        CHECK_EQ(r3.at("id").get<int>(), 7);
        CHECK(!r3.at("ok").get<bool>());
    }

    // --- reads ---------------------------------------------------------

    {
        // get_flight happy path
        json r = Run(api, R"({"type":"command","callsign":"DLH4TX","action":"get_flight"})");
        CHECK(r.at("ok").get<bool>());
        CHECK_EQ(r.at("callsign").get<std::string>(), "DLH4TX");
        const json& f = r.at("payload");
        CHECK_EQ(f.at("origin").get<std::string>(), "EDDM");
        CHECK_EQ(f.at("sid").get<std::string>(), "GIVMI1N");
        CHECK_EQ(f.at("clearedAltitude").get<int>(), 12000);
        CHECK(f.at("clearanceFlag").get<bool>());
        CHECK_EQ(f.at("groundState").get<std::string>(), "TAXI");
        CHECK(f.at("correlated").get<bool>());
        CHECK_EQ(f.at("groundSpeed").get<int>(), 18);
        CHECK(f.contains("latitude") && f.contains("longitude"));
    }
    {
        // get_flight without callsign
        json r = Run(api, R"({"type":"command","action":"get_flight"})");
        CHECK(!r.at("ok").get<bool>());
        CHECK(r.at("error").get<std::string>().find("callsign") != std::string::npos);
    }
    {
        // get_flight unknown callsign: Actions error is passed through
        json r = Run(api, R"({"type":"command","callsign":"NOBODY","action":"get_flight"})");
        CHECK(!r.at("ok").get<bool>());
        CHECK(r.at("error").get<std::string>().find("NOBODY") != std::string::npos);
    }
    {
        // list_flights + filter passthrough
        json r = Run(api, R"({"type":"command","action":"list_flights","payload":{"filter":"EDDF"}})");
        CHECK(r.at("ok").get<bool>());
        CHECK_EQ(mock.lastString, "EDDF");
        CHECK_EQ(r.at("payload").at("count").get<int>(), 2);
        CHECK_EQ(r.at("payload").at("flights").size(), 2u);
    }

    // --- writes: parameter validation & dispatch ------------------------

    {
        // set_cleared_altitude with feet
        json r = Run(api, R"({"type":"command","callsign":"DLH4TX","action":"set_cleared_altitude","payload":{"feet":12000}})");
        CHECK(r.at("ok").get<bool>());
        CHECK_EQ(mock.lastMethod, "SetClearedAltitude");
        CHECK_EQ(mock.lastInt, 12000);
        CHECK(r.at("payload").contains("message"));
    }
    {
        // special values map to 0/1/2
        Run(api, R"({"type":"command","callsign":"A","action":"set_cleared_altitude","payload":{"special":"ils"}})");
        CHECK_EQ(mock.lastInt, 1);
        Run(api, R"({"type":"command","callsign":"A","action":"set_cleared_altitude","payload":{"special":"visual"}})");
        CHECK_EQ(mock.lastInt, 2);
        Run(api, R"({"type":"command","callsign":"A","action":"set_cleared_altitude","payload":{"special":"clear"}})");
        CHECK_EQ(mock.lastInt, 0);
        json bad = Run(api, R"({"type":"command","callsign":"A","action":"set_cleared_altitude","payload":{"special":"bogus"}})");
        CHECK(!bad.at("ok").get<bool>());
    }
    {
        // missing / mistyped payload field
        json r1 = Run(api, R"({"type":"command","callsign":"A","action":"set_heading","payload":{}})");
        CHECK(!r1.at("ok").get<bool>());
        CHECK(r1.at("error").get<std::string>().find("degrees") != std::string::npos);
        json r2 = Run(api, R"({"type":"command","callsign":"A","action":"set_heading","payload":{"degrees":"north"}})");
        CHECK(!r2.at("ok").get<bool>());
    }
    {
        // mach: decimal, hundredths and garbage
        Run(api, R"({"type":"command","callsign":"A","action":"set_mach","payload":{"mach":0.78}})");
        CHECK_EQ(mock.lastMethod, "SetMach");
        CHECK_EQ(mock.lastInt, 78);
        Run(api, R"({"type":"command","callsign":"A","action":"set_mach","payload":{"mach":78}})");
        CHECK_EQ(mock.lastInt, 78);
        json bad = Run(api, R"({"type":"command","callsign":"A","action":"set_mach","payload":{"mach":"fast"}})");
        CHECK(!bad.at("ok").get<bool>());
        json toobig = Run(api, R"({"type":"command","callsign":"A","action":"set_mach","payload":{"mach":1.2}})");
        CHECK(!toobig.at("ok").get<bool>());
    }
    {
        // remaining write actions dispatch to the right method
        Run(api, R"({"type":"command","callsign":"A","action":"set_final_altitude","payload":{"feet":34000}})");
        CHECK_EQ(mock.lastMethod, "SetFinalAltitude");
        Run(api, R"({"type":"command","callsign":"A","action":"set_speed","payload":{"knots":250}})");
        CHECK_EQ(mock.lastMethod, "SetSpeed");
        Run(api, R"({"type":"command","callsign":"A","action":"set_rate","payload":{"feetPerMinute":2000}})");
        CHECK_EQ(mock.lastMethod, "SetRate");
        Run(api, R"({"type":"command","callsign":"A","action":"set_squawk","payload":{"code":"2354"}})");
        CHECK_EQ(mock.lastMethod, "SetSquawk");
        CHECK_EQ(mock.lastString, "2354");
        Run(api, R"({"type":"command","callsign":"A","action":"set_direct","payload":{"point":"ERNAS"}})");
        CHECK_EQ(mock.lastMethod, "SetDirect");
        Run(api, R"({"type":"command","callsign":"A","action":"set_scratchpad","payload":{"text":"HI"}})");
        CHECK_EQ(mock.lastMethod, "SetScratchPad");
        Run(api, R"({"type":"command","callsign":"A","action":"set_ground_state","payload":{"state":"PUSH"}})");
        CHECK_EQ(mock.lastMethod, "BroadcastScratchPadToken");
        CHECK_EQ(mock.lastString, "PUSH");
        Run(api, R"({"type":"command","callsign":"A","action":"set_sid","payload":{"sid":"GIVMI1N/26L"}})");
        CHECK_EQ(mock.lastMethod, "SetSid");
        Run(api, R"({"type":"command","callsign":"A","action":"set_star","payload":{"star":"ROKIL3A"}})");
        CHECK_EQ(mock.lastMethod, "SetStar");
    }
    {
        // EuroScope refusal becomes ok:false with the Actions message
        mock.failNext = true;
        json r = Run(api, R"({"type":"command","callsign":"A","action":"set_speed","payload":{"knots":250}})");
        CHECK(!r.at("ok").get<bool>());
        CHECK(r.at("error").get<std::string>().find("rejected") != std::string::npos);
    }

    // --- chat injection actions ----------------------------------------

    {
        MockChatInjection::Reset();
        MockChatInjection::nextOk = true;
        json r = Run(api, R"({"type":"command","callsign":"DLH4TX","action":"send_private_message","payload":{"message":"hello there"}})");
        CHECK(r.at("ok").get<bool>());
        CHECK_EQ(MockChatInjection::lastCommandLine, ".msg DLH4TX hello there");

        json r2 = Run(api, R"({"type":"command","action":"send_frequency_message","payload":{"message":"all stations"}})");
        CHECK(r2.at("ok").get<bool>());
        CHECK_EQ(MockChatInjection::lastFrequencyText, "all stations");

        MockChatInjection::nextOk = false;
        MockChatInjection::nextDetail = "command line not found";
        json r3 = Run(api, R"({"type":"command","callsign":"X","action":"send_private_message","payload":{"message":"hi"}})");
        CHECK(!r3.at("ok").get<bool>());
        CHECK(r3.at("error").get<std::string>().find("command line not found") != std::string::npos);
    }

    // --- ATC list --------------------------------------------------------

    {
        json r = Run(api, R"({"type":"command","action":"list_controllers"})");
        CHECK(r.at("ok").get<bool>());
        CHECK_EQ(r.at("payload").at("count").get<int>(), 2);
        CHECK_EQ(r.at("payload").at("controllers")[0].at("callsign").get<std::string>(), "EDDM_TWR");
        CHECK_EQ(r.at("payload").at("controllers")[0].at("frequency").get<double>(), 119.6);
        CHECK_EQ(r.at("payload").at("controllers")[0].at("facility").get<int>(), 4);
        CHECK(r.at("payload").at("controllers")[0].at("isController").get<bool>());

        json rf = Run(api, R"({"type":"command","action":"list_controllers","payload":{"filter":"EDDM"}})");
        CHECK(rf.at("ok").get<bool>());
        CHECK_EQ(mock.lastString, "EDDM");
    }

    // --- track control (assume / release / transfer) -----------------------

    {
        json r = Run(api, R"({"type":"command","id":21,"callsign":"DLH4TX","action":"assume"})");
        CHECK(r.at("ok").get<bool>());
        CHECK_EQ(mock.lastMethod, "AssumeTrack");
        CHECK_EQ(mock.lastCallsign, "DLH4TX");
        CHECK_EQ(r.at("id").get<int>(), 21);

        json r2 = Run(api, R"({"type":"command","callsign":"DLH4TX","action":"release"})");
        CHECK(r2.at("ok").get<bool>());
        CHECK_EQ(mock.lastMethod, "ReleaseTrack");

        json r3 = Run(api, R"({"type":"command","callsign":"DLH4TX","action":"transfer","payload":{"controller":"EDDF_APP"}})");
        CHECK(r3.at("ok").get<bool>());
        CHECK_EQ(mock.lastMethod, "TransferTrack");
        CHECK_EQ(mock.lastString, "EDDF_APP");

        // callsign is mandatory for all three
        json r4 = Run(api, R"({"type":"command","action":"assume"})");
        CHECK(!r4.at("ok").get<bool>());

        // transfer requires the target controller in the payload
        json r5 = Run(api, R"({"type":"command","callsign":"DLH4TX","action":"transfer"})");
        CHECK(!r5.at("ok").get<bool>());
        CHECK(r5.at("error").get<std::string>().find("controller") != std::string::npos);

        // EuroScope refusal propagates as ok:false
        mock.failNext = true;
        json r6 = Run(api, R"({"type":"command","callsign":"DLH4TX","action":"assume"})");
        CHECK(!r6.at("ok").get<bool>());
    }

    // --- events ----------------------------------------------------------

    {
        json e = json::parse(api.EventFlightUpdated(MockActions::SampleFlight("DLH4TX")));
        CHECK_EQ(e.at("type").get<std::string>(), "event");
        CHECK_EQ(e.at("action").get<std::string>(), "flight_updated");
        CHECK_EQ(e.at("callsign").get<std::string>(), "DLH4TX");
        CHECK_EQ(e.at("payload").at("destination").get<std::string>(), "EDDF");
        CHECK(!e.contains("id"));

        json rm = json::parse(api.EventFlightRemoved("DLH4TX"));
        CHECK_EQ(rm.at("action").get<std::string>(), "flight_removed");
        CHECK(rm.at("payload").is_object() && rm.at("payload").empty());

        PositionUpdate p;
        p.callsign = "DLH4TX";
        p.latitude = 48.35;
        p.longitude = 11.78;
        p.flightLevel = 12000;
        p.groundSpeed = 250;
        p.squawk = "1000";
        json pe = json::parse(api.EventPositionUpdated(p));
        CHECK_EQ(pe.at("action").get<std::string>(), "position_updated");
        CHECK_EQ(pe.at("payload").at("flightLevel").get<int>(), 12000);
        CHECK_EQ(pe.at("payload").at("squawk").get<std::string>(), "1000");

        json cu = json::parse(api.EventControllerUpdated(MockActions::SampleController("EDDM_TWR")));
        CHECK_EQ(cu.at("action").get<std::string>(), "controller_updated");
        CHECK_EQ(cu.at("callsign").get<std::string>(), "EDDM_TWR");
        CHECK_EQ(cu.at("payload").at("frequency").get<double>(), 119.6);
        CHECK_EQ(cu.at("payload").at("rating").get<int>(), 5);

        json cr = json::parse(api.EventControllerRemoved("EDDM_TWR"));
        CHECK_EQ(cr.at("action").get<std::string>(), "controller_removed");
        CHECK(cr.at("payload").is_object() && cr.at("payload").empty());

        json snap = json::parse(api.EventSessionSnapshot(
            { MockActions::SampleFlight("DLH4TX") },
            { MockActions::SampleController("EDDM_TWR"),
              MockActions::SampleController("EDDF_APP") }));
        CHECK_EQ(snap.at("action").get<std::string>(), "session_snapshot");
        CHECK_EQ(snap.at("payload").at("count").get<int>(), 1);
        CHECK_EQ(snap.at("payload").at("flights")[0].at("callsign").get<std::string>(), "DLH4TX");
        CHECK_EQ(snap.at("payload").at("controllerCount").get<int>(), 2);
        CHECK_EQ(snap.at("payload").at("controllers")[1].at("callsign").get<std::string>(), "EDDF_APP");

        // FlightObject carries the pending-handoff target (additive field).
        FlightInfo handedOff = MockActions::SampleFlight("DLH4TX");
        handedOff.handoffTargetController = "EDDF_APP";
        json ho = json::parse(api.EventFlightUpdated(handedOff));
        CHECK_EQ(ho.at("payload").at("handoffTargetController").get<std::string>(), "EDDF_APP");
    }

    // ---------------------------------------------------------------------

    std::cout << (g_failures == 0 ? "PASS" : "FAIL") << ": " << g_checks
              << " checks, " << g_failures << " failure(s)\n";
    return g_failures == 0 ? 0 : 1;
}
