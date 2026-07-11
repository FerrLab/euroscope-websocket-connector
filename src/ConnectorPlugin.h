#pragma once

// ConnectorPlugin — the EuroScopePlugIn::CPlugIn subclass.
//
// Responsibilities:
//   * plugin registration (name/version/compatibility code)
//   * parsing of ".lpc ..." dot-commands typed in the EuroScope command line
//   * formatting results back to the user via DisplayUserMessage
//
// All EuroScope-facing operations are delegated to Actions (Actions.h) so the
// same operations can be driven by the HTTPS gateway as well as the
// command line. Keep this class free of business logic.

#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "EuroScopePlugIn.h"

#include "Actions.h"
#include "Gateway.h"
#include "JsonApi.h"

#define PLUGIN_NAME "EuroScope Long Polling Connector"
// Release builds stamp the real semantic version at compile time
// (cmake -DPLUGIN_VERSION=x.y.z, done by the release job in
// .github/workflows/ci.yml). The fallback makes local builds
// unmistakable in the load banner: "v0.0.0-dev".
#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "0.0.0-dev"
#endif
#define PLUGIN_AUTHOR "FerrLab"
#define PLUGIN_COPYRIGHT "MIT License - github.com/FerrLab/euroscope-longpolling-connector"

// Name of the chat handler (tab) our output appears under in EuroScope.
#define MESSAGE_HANDLER "LPC"

class ConnectorPlugin : public EuroScopePlugIn::CPlugIn
{
public:
    ConnectorPlugin();
    virtual ~ConnectorPlugin();

    // Called by EuroScope for every command typed in the command line that
    // EuroScope itself does not recognise. Return true when we handled it.
    bool OnCompileCommand(const char* sCommandLine) override;

    // Event sources for the JSON contract's "event" messages
    // (docs/PROTOCOL.md): printed to the LPC chat tab when enabled via
    // ".lpc events", and pushed to the gateway while connected.
    void OnFlightPlanFlightPlanDataUpdate(EuroScopePlugIn::CFlightPlan FlightPlan) override;
    void OnFlightPlanControllerAssignedDataUpdate(EuroScopePlugIn::CFlightPlan FlightPlan,
                                                  int DataType) override;
    void OnFlightPlanDisconnect(EuroScopePlugIn::CFlightPlan FlightPlan) override;
    void OnRadarTargetPositionUpdate(EuroScopePlugIn::CRadarTarget RadarTarget) override;
    void OnControllerPositionUpdate(EuroScopePlugIn::CController Controller) override;
    void OnControllerDisconnect(EuroScopePlugIn::CController Controller) override;

    // 1 Hz heartbeat: pumps the gateway (reconnects, inbound commands,
    // snapshot on connect). This is the ONLY place gateway traffic touches
    // the EuroScope API, keeping everything main-thread.
    void OnTimer(int Counter) override;

private:
    Actions m_actions;
    JsonApi m_jsonApi;
    Gateway m_gateway;

    // Event-stream toggles (".lpc events ...") - printing to the chat tab.
    // Position events are separate because they fire for every target
    // every few seconds.
    bool m_flightEvents = false;
    bool m_positionEvents = false;

    // Send position_updated events to the gateway (persisted setting;
    // flight events always go when connected).
    bool m_gatewayPositions = true;

    void LoadSettings();
    void SaveSetting(const char* name, const char* description,
                     const std::string& value);

    // Routes one event JSON to every active consumer (chat tab print,
    // gateway when connected).
    void EmitEvent(const std::string& eventJson, bool isPosition);

    // Writes one line to the "LPC" chat handler.
    void Say(const std::string& text);
    // Writes a multi-line text (splits on '\n').
    void SayLines(const std::string& text);

    // Sub-command handlers. `tokens` is the whitespace-split command line
    // (tokens[0] == ".lpc"), `line` the original string for commands that
    // need the raw remainder (message text, scratchpad content).
    void CmdHelp();
    void CmdList(const std::vector<std::string>& tokens);
    void CmdAtc(const std::vector<std::string>& tokens);
    void CmdShow(const std::vector<std::string>& tokens);
    void CmdTrack(const std::vector<std::string>& tokens, const std::string& verb);
    void CmdSet(const std::vector<std::string>& tokens);
    void CmdPad(const std::vector<std::string>& tokens, const std::string& line);
    void CmdState(const std::vector<std::string>& tokens);
    void CmdSid(const std::vector<std::string>& tokens, bool star);
    void CmdMsg(const std::vector<std::string>& tokens, const std::string& line);
    void CmdFreq(const std::vector<std::string>& tokens, const std::string& line);
    void CmdJson(const std::vector<std::string>& tokens, const std::string& line);
    void CmdEvents(const std::vector<std::string>& tokens);
    void CmdGateway(const std::vector<std::string>& tokens);
};
