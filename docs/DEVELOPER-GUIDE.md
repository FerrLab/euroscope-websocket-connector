# Developer guide

For whoever works on this codebase next. Read
[euroscope-plugin-research.md](euroscope-plugin-research.md) first if you
have never written a EuroScope plugin — it explains the ecosystem, the SDK,
and where everything comes from.

## Architecture

```
EuroScope (32-bit MFC app, UI thread)                      ┊ worker threads
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
 │  poll thread + send thread, backoff, counters,          ┊
 │  snapshot trigger; batches messages into POSTs and ═════╪══ thread-safe
 │  long-polls for commands (docs/PROTOCOL.md Transport)   ┊   queues
 └─► Http::Client (interface)        http/HttpClient.h     ┊
      production: WinHTTP        http/WinHttpClient.cpp    ┊
        (OS handles TLS, certificates, proxies)            ┊
      tests: plain-HTTP POSIX client (tests/)              ┊
      URL parsing (pure):            http/HttpUrl.{h,cpp}  ┊

ChatInjection                            ChatInjection.{h,cpp}
    isolated workaround for the missing chat API — see below
```

**The separation matters:** `ConnectorPlugin` is the *command-line* front
end; `JsonApi` is the *contract* front end; `Gateway` is pure transport
and never interprets messages beyond batching/unbatching them. All semantics live in
`Actions` (behind `IActions`), so behaviour is identical no matter where a
request comes from. Keep new functionality in `Actions` (or a sibling),
never inline in command handlers, and keep `docs/PROTOCOL.md` in sync with
`JsonApi.cpp`.

**Threading, as implemented:** `Gateway` owns two worker threads — one
keeps a long poll open (`GET {base}/poll`), one drains the outbound queue
into batched `POST {base}/messages` requests. Workers only touch HTTP and
the mutex-guarded queues. Everything EuroScope lives on the main thread:
events are serialised inside the callbacks and handed to `Gateway::Send`;
inbound commands are drained once per second in `OnTimer` and executed
there. Never call the EuroScope API (or `Actions`, or
`JsonApi::HandleMessage`) from any other thread. Failures back off
exponentially (2 s → 60 s; 401/403 park at the maximum); every
unhealthy → healthy transition re-triggers the `session_snapshot`.
`Disable()` aborts an in-flight long poll via `Http::Client::Abort()` and
joins the workers — that is why one client instance exists per request.

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
   no sleeps). All network I/O runs on the Gateway's worker threads,
   which exchange plain data (JSON strings) with the main thread via
   locked queues; inbound commands are applied on the main thread from
   `OnTimer` (called once per second).

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
   control (`WM_SETTEXT`) and **posts** the ENTER / FREQ key through the
   thread message queue — it must be `PostMessage`, never `SendMessage`:
   EuroScope handles command-line keys in its MFC message pump
   (`PreTranslateMessage`), which never sees sent messages (verified live —
   `SendMessage`'d keys were silently ignored). Because posted keys are
   processed only after the plugin callback returns, the consumed-check is
   deferred to the next `OnTimer` tick (`ChatInjection::Pump()`), which
   also serializes queued sends. Undocumented UI behaviour that may break
   with any EuroScope release; deliberately isolated in its own module —
   if it breaks, nothing else is affected. Re-verify after every EuroScope
   upgrade.

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
- `tests/http_gateway_test.cpp` (UNIX only) — URL parsing plus the REAL
  `Gateway` (both worker threads) end-to-end over live TCP against a
  scripted HTTP backend: bearer-token auth, long-poll command delivery,
  204 empty polls, batched POSTs, snapshot gating and counters, 401 →
  clear error and no health, backend-down error path, and `Disable()`
  promptly aborting a held long poll. Production swaps in WinHTTP behind
  the same `Http::Client` interface.

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
   chat, which is expected and documented behaviour). Note the verdict
   ("sent" / "FAILED") arrives in the WSC tab up to a second after the
   command — the keystroke is posted and verified on the next timer tick.

## Transport status & what's next

The transport is plain HTTPS (long poll + POST, see PROTOCOL.md). An
earlier hand-rolled WebSocket/Pusher transport was replaced by this
design — it lives in git history at commit `ed8ef6c` if ever needed.
Candidate next steps:

- **Windows CI** — a GitHub Actions workflow (windows runner, `cmake -A
  Win32`) would compile `WinHttpClient.cpp` against the real Windows
  headers and build the DLL artifact; a Linux job runs the test suites.
- **Controller events** — `controller_updated`/`controller_removed` are
  reserved in the spec; wire `OnControllerPositionUpdate`/`Disconnect`
  through the same EmitEvent path.
- **Outbound queueing policy** — messages produced while unhealthy are
  dropped (the snapshot restores consistency). If the backend needs
  gapless history, add sequence numbers to events first.
- **Poll efficiency** — if command latency matters more than request
  volume, shorten the poll `timeout`; if volume matters, lengthen it
  (the server hold hint is `Gateway::kPollHoldSeconds`).
