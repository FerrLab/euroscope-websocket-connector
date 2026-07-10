#pragma once

// ConnectorPlugin — the EuroScopePlugIn::CPlugIn subclass.
//
// Responsibilities:
//   * plugin registration (name/version/compatibility code)
//   * parsing of ".wsc ..." dot-commands typed in the EuroScope command line
//   * formatting results back to the user via DisplayUserMessage
//
// All EuroScope-facing operations are delegated to Actions (Actions.h) so the
// same operations can later be driven by a WebSocket endpoint instead of the
// command line. Keep this class free of business logic.

#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "EuroScopePlugIn.h"

#include "Actions.h"
#include "Gateway.h"
#include "JsonApi.h"

#define PLUGIN_NAME "WebSocket Connector"
#define PLUGIN_VERSION "0.3.0"
#define PLUGIN_AUTHOR "FerrLab"
#define PLUGIN_COPYRIGHT "MIT License - github.com/FerrLab/euroscope-websocket-connector"

// Name of the chat handler (tab) our output appears under in EuroScope.
#define MESSAGE_HANDLER "WSC"

class ConnectorPlugin : public EuroScopePlugIn::CPlugIn
{
public:
    ConnectorPlugin();
    virtual ~ConnectorPlugin();

    // Called by EuroScope for every command typed in the command line that
    // EuroScope itself does not recognise. Return true when we handled it.
    bool OnCompileCommand(const char* sCommandLine) override;

    // Event sources for the JSON contract's "event" messages
    // (docs/PROTOCOL.md). Today the events are printed to the WSC chat tab
    // when enabled via ".wsc events"; phase 2 pushes them to the gateway.
    void OnFlightPlanFlightPlanDataUpdate(EuroScopePlugIn::CFlightPlan FlightPlan) override;
    void OnFlightPlanControllerAssignedDataUpdate(EuroScopePlugIn::CFlightPlan FlightPlan,
                                                  int DataType) override;
    void OnFlightPlanDisconnect(EuroScopePlugIn::CFlightPlan FlightPlan) override;
    void OnRadarTargetPositionUpdate(EuroScopePlugIn::CRadarTarget RadarTarget) override;

    // 1 Hz heartbeat: pumps the gateway (reconnects, inbound commands,
    // snapshot on connect). This is the ONLY place gateway traffic touches
    // the EuroScope API, keeping everything main-thread.
    void OnTimer(int Counter) override;

private:
    Actions m_actions;
    JsonApi m_jsonApi;
    Gateway m_gateway;

    // Event-stream toggles (".wsc events ...") - printing to the chat tab.
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

    // Writes one line to the "WSC" chat handler.
    void Say(const std::string& text);
    // Writes a multi-line text (splits on '\n').
    void SayLines(const std::string& text);

    // Sub-command handlers. `tokens` is the whitespace-split command line
    // (tokens[0] == ".wsc"), `line` the original string for commands that
    // need the raw remainder (message text, scratchpad content).
    void CmdHelp();
    void CmdList(const std::vector<std::string>& tokens);
    void CmdShow(const std::vector<std::string>& tokens);
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
