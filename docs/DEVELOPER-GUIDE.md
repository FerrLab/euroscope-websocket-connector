# Developer guide

For whoever works on this codebase next. Read
[euroscope-plugin-research.md](euroscope-plugin-research.md) first if you
have never written a EuroScope plugin — it explains the ecosystem, the SDK,
and where everything comes from.

## Architecture

```
EuroScope (32-bit MFC app, UI thread)                      ┊ socket thread
 │  loads DLL, calls EuroScopePlugInInit()   dllmain.cpp   ┊
 ▼                                                         ┊
ConnectorPlugin : CPlugIn          ConnectorPlugin.{h,cpp} ┊
 │  OnCompileCommand(".wsc ...") → parse → dispatch        ┊
 │  On...Update callbacks → event JSON → EmitEvent         ┊
 │  OnTimer (1 Hz) → Gateway::Tick → inbound commands      ┊
 │    → JsonApi::HandleMessage → responses back out        ┊
 ├──► JsonApi                            JsonApi.{h,cpp}   ┊
 │     │  the JSON contract (docs/PROTOCOL.md):            ┊
 │     │  "command" in → "response" out; event builders    ┊
 │     ▼                                                   ┊
 │   IActions (interface)                IActions.h        ┊
 │     ▲                                                   ┊
 ▼     │                                                   ┊
Actions                                  Actions.{h,cpp}   ┊
 │  reads → FlightInfo, writes → ActionResult              ┊
 ▼                                                         ┊
EuroScope plugin API                                       ┊
                                                           ┊
Gateway                                  Gateway.{h,cpp}   ┊
 │  reconnect/backoff, counters, snapshot trigger          ┊
 └─► Ws::WsClient                    ws/WsClient.{h,cpp} ══╪══ thread-safe
      connect/handshake/frame pump on its own thread       ┊   queues
      built on the pure codec:                             ┊
      ws/WsFrame.{h,cpp} + ws/WsHandshake.{h,cpp}          ┊

ChatInjection                            ChatInjection.{h,cpp}
    isolated workaround for the missing chat API — see below
```

**The separation matters:** `ConnectorPlugin` is the *command-line* front
end; `JsonApi` is the *contract* front end; the `Gateway`/`WsClient` pair
is pure transport and never interprets messages. All semantics live in
`Actions` (behind `IActions`), so behaviour is identical no matter where a
request comes from. Keep new functionality in `Actions` (or a sibling),
never inline in command handlers, and keep `docs/PROTOCOL.md` in sync with
`JsonApi.cpp`.

**Threading, as implemented:** the socket thread (inside `WsClient`) only
touches sockets and its own mutex-guarded queues. Everything EuroScope
lives on the main thread: events are serialised inside the callbacks and
handed to `Gateway::Send`; inbound commands are drained once per second in
`OnTimer` and executed there. Never call the EuroScope API (or `Actions`,
or `JsonApi::HandleMessage`) from any other thread. `WsClient` is one
connection attempt; `Gateway` recreates it for reconnects (2 s → 60 s
backoff) and triggers the `session_snapshot` on each connect.

### Adding a new operation (checklist)

1. Add a method to `Actions` returning `ActionResult` (or a struct for
   reads). Look up the flight with `Find()`; report EuroScope refusals
   honestly.
2. Add the action to `JsonApi::HandleMessage` (name it `snake_case`) and
   specify it in [PROTOCOL.md](PROTOCOL.md).
3. Add a `Cmd...` handler + dispatch line in
   `ConnectorPlugin::OnCompileCommand`, and a line in `CmdHelp`.
4. Document it in [COMMANDS.md](COMMANDS.md) (syntax, example, permissions,
   caveats).
5. Verify the API methods you call actually exist in
   [`sdk/EuroScopePlugIn.h`](../sdk/EuroScopePlugIn.h) — don't trust
   snippets from other plugins; API versions differ.

New *events* follow the same pattern: a builder in `JsonApi`, a callback
override in `ConnectorPlugin`, a row in PROTOCOL.md's event table.

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

### Unit tests (any platform)

The JSON contract layer is decoupled from the SDK behind the `IActions`
interface, so it has a real test suite that runs anywhere — no Windows, no
EuroScope:

```
cmake -S tests -B build-tests
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

- `tests/json_api_test.cpp` — the contract: envelope validation, action
  dispatch, payload validation, error model, event builders (mock
  `IActions`).
- `tests/ws_test.cpp` — the WebSocket codec against the RFC 6455 vectors:
  handshake key/accept, URL parsing, frame encode/decode, fragmentation,
  the message assembler.
- `tests/ws_client_test.cpp` (UNIX only) — the REAL `WsClient` code,
  end-to-end over TCP against an in-process scripted server: handshake,
  echo, server push, fragmented messages, ping/pong, close and error
  paths. (`WsClient.cpp` has a small POSIX `#ifdef` branch exactly so this
  test can exist; the production DLL uses the Win32 branch.)

**Add a test whenever you add or change an action, event, or codec
behaviour.** The tests are deliberately a standalone CMake project because
the root CMakeLists refuses to configure on non-Windows.

### In EuroScope (the parts that can't be mocked)

`Actions.cpp` and `ChatInjection.cpp` only run inside EuroScope. Manual
test procedure:

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

## Phase 2 status & what's next

The WebSocket transport is implemented as described above (own RFC 6455
client — zero dependencies, x86-safe, codec fully unit-tested; see
research doc §4 for why not IXWebSocket/Boost). Candidate next steps:

- **`wss://` (TLS)** — required before any gateway leaves the LAN.
  Options: Windows Schannel wrapped around the socket, or vendoring
  mbedTLS. Isolate it inside `WsClient` so nothing else changes.
- **Authentication** — the contract has no auth; a gateway token could
  ride as a header in `Ws::BuildRequest` or as a first `command`.
- **Controller events** — `controller_updated`/`controller_removed` are
  reserved in the spec; wire `OnControllerPositionUpdate`/`Disconnect`
  through the same EmitEvent path.
- **Outbound queueing policy** — currently events during a disconnect are
  dropped (snapshot restores consistency). If a gateway needs gapless
  history, add sequence numbers to events first.
