# Developer guide

For whoever works on this codebase next. Read
[euroscope-plugin-research.md](euroscope-plugin-research.md) first if you
have never written a EuroScope plugin — it explains the ecosystem, the SDK,
and where everything comes from.

## Architecture

```
EuroScope (32-bit MFC app, UI thread)
 │  loads DLL, calls EuroScopePlugInInit()          src/dllmain.cpp
 ▼
ConnectorPlugin : EuroScopePlugIn::CPlugIn          src/ConnectorPlugin.{h,cpp}
 │  OnCompileCommand(".wsc ...") → parse → dispatch
 │  formats ActionResult → DisplayUserMessage (chat tab "WSC")
 ▼
Actions                                             src/Actions.{h,cpp}
 │  one method per supported operation
 │  string in → ActionResult out; no UI, no parsing
 ▼
EuroScope plugin API (CFlightPlan, CFlightPlanData,
CFlightPlanControllerAssignedData, CRadarTarget, ...)

ChatInjection                                       src/ChatInjection.{h,cpp}
    isolated workaround for the missing chat API — see below
```

**The separation matters:** `ConnectorPlugin` is the *command-line* front
end. The planned WebSocket layer becomes a *second* front end that calls
the same `Actions` methods, giving identical semantics over the wire. Keep
new functionality in `Actions` (or a sibling), never inline in command
handlers.

### Adding a new operation (checklist)

1. Add a method to `Actions` returning `ActionResult`. Look up the flight
   with `Find()`; report EuroScope refusals honestly.
2. Add a `Cmd...` handler + dispatch line in
   `ConnectorPlugin::OnCompileCommand`, and a line in `CmdHelp`.
3. Document it in [COMMANDS.md](COMMANDS.md) (syntax, example, permissions,
   caveats).
4. Verify the API methods you call actually exist in
   [`sdk/EuroScopePlugIn.h`](../sdk/EuroScopePlugIn.h) — don't trust
   snippets from other plugins; API versions differ.

## EuroScope API rules & gotchas (hard-won, do not skip)

1. **Single thread.** Every `CPlugIn` callback runs on EuroScope's UI
   thread, and the API is not thread-safe. Never block (no network calls,
   no sleeps). When the WebSocket layer lands, it must run its socket on a
   background thread, exchange plain data (JSON/structs) with the main
   thread via a locked queue, and apply inbound requests on the main thread
   — `OnTimer` (called once per second) is the natural drain point.

2. **Copy strings immediately.** API getters return `const char*` into
   internal buffers that the next API call may overwrite, and they can be
   NULL. `Actions.cpp` wraps every getter in `S()` (NULL-safe copy). Never
   store handle objects (`CFlightPlan`, `CRadarTarget`) across callbacks
   either — re-select by callsign each time.

3. **Setters fail silently by design.** Every `Set...` returns `bool`.
   `false` usually means: not connected, or not permitted to modify that
   flight (most controller-assigned data requires you to be tracking the
   aircraft for the change to distribute). Always check and surface it.

4. **Cleared altitude special values:** `0` = no cleared level (final
   applies), `1` = cleared ILS approach, `2` = cleared visual approach,
   otherwise feet.

5. **Ground state has no setter.** `GetGroundState()` exists;
   `SetGroundState()` does not. Write the magic tokens (`STUP`, `PUSH`,
   `TAXI`, `DEPA`, `TXIN`, `PARK`, `NSTS`) — and `CLEA`/`NOTC` for the
   clearance flag — into the scratch pad, then restore the previous
   content. This is the community-standard pattern (PushbackFlorian, vACDM
   do the same); official token list:
   [Non-Standard Extensions](https://www.euroscope.hu/wp/non-standard-extensions/).
   `Actions::BroadcastScratchPadToken` implements it.

6. **SID/STAR have getters but no setters.** Assignment = rewrite the route
   string so it starts with the SID / ends with the STAR, then
   `AmendFlightPlan()`. The procedure name must exist in the loaded sector
   file to be re-extracted. `Actions::SetSid/SetStar` implement it
   conservatively (they only remove a route token that matches the
   currently extracted procedure).

7. **There is NO chat-send API** (verified against the full compat-16
   header: the chat callbacks are receive-only; `DisplayUserMessage` is
   local-only). `ChatInjection` types into EuroScope's command-line edit
   control (`WM_SETTEXT` + synthesized ENTER / FREQ key) — undocumented UI
   behaviour that may break with any EuroScope release. It is deliberately
   isolated in its own module with rich failure diagnostics; if it breaks,
   nothing else is affected. Re-verify after every EuroScope upgrade.

8. **Compatibility code.** The `CPlugIn` constructor passes
   `COMPATIBILITY_CODE` (16) from the vendored header; EuroScope refuses
   plugins with a mismatched code. If a EuroScope release bumps it, refresh
   `sdk/` from that release (see [sdk/README.md](../sdk/README.md)) and
   rebuild — expect this to be the *only* change needed.

9. **"All flights" = session-visible flights.** `FlightPlanSelectFirst/Next`
   iterates what EuroScope knows: traffic within your visibility range.
   There is no API to see the whole network.

10. **Multi-byte strings everywhere.** Build with `_MBCS`, never `UNICODE`.
    Non-ASCII text will not survive the VATSIM protocol.

## Testing

There is no automated test rig (the API only exists inside a running
EuroScope). Manual test procedure:

1. Build Debug, load into EuroScope.
2. Open a **SweatBox/simulator session** (never the live network) with a
   scenario file that has a few departures.
3. Walk through every command in [COMMANDS.md](COMMANDS.md); for state
   changes, verify the effect in the departure list ground-state column and
   in a second EuroScope instance if available (sync check).
4. For `ChatInjection`, verify both the success path and the failure path
   (e.g. `.wsc msg NOSUCHUSER hi` — EuroScope accepts the command, so the
   *plugin* reports success; delivery failure shows in EuroScope's own
   chat, which is expected and documented behaviour).

## Roadmap to the WebSocket connector (phase 2)

The intended shape (see research doc §4 for the reasoning):

- Plugin runs a WebSocket **client** dialing out to a configurable gateway
  (StripCol-style), reconnecting with backoff; socket lives on its own
  thread.
- Outbound: serialize on the main thread in `OnFlightPlanFlightPlanDataUpdate`,
  `OnFlightPlanControllerAssignedDataUpdate`, `OnRadarTargetPositionUpdate`,
  `OnControllerPositionUpdate`, the disconnect callbacks → queue → socket
  thread sends. Full-state snapshot on (re)connect via the same iteration
  `Actions::ListFlights` uses.
- Inbound: gateway messages map 1:1 onto `Actions` methods; queue them and
  apply from `OnTimer`. `ActionResult` serializes into the response.
- Library candidate: IXWebSocket (builds x86, no external event loop) —
  statically linked; or a bare WinSock client since the gateway protocol is
  ours to define.
