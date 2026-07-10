# euroscope-websocket-connector

A [EuroScope](https://www.euroscope.hu/wp/) plugin that connects the
controller's session to external software over **WebSocket**: it dials out
to a gateway, streams the session as JSON events (flights, positions,
snapshots), and accepts JSON commands back (modify flight plans, ground
states, SID/STAR, messages). Everything is also driven manually via
`.wsc` dot-commands for testing.

> **Status: phase 2 — WebSocket transport implemented.**
> The plugin is a reconnecting WebSocket client (`.wsc gateway ...`,
> zero-dependency RFC 6455 implementation, `ws://` only for now) carrying
> the standardized JSON contract ([docs/PROTOCOL.md](docs/PROTOCOL.md)).
> Needs real-world testing inside EuroScope — see *Verification status*
> in [docs/BUILDING.md](docs/BUILDING.md).

## What it can do

| # | Capability | Command |
|---|------------|---------|
| 1 | List all flights known to the session | `.wsc list [filter]`, `.wsc show <cs>` |
| 2 | Modify flight-plan / controller-assigned parameters | `.wsc set <cs> <field> <value>` |
| 3 | Send a text message to the primary frequency *(experimental)* | `.wsc freq <text>` |
| 4 | Send a private message to a user *(experimental)* | `.wsc msg <cs> <text>` |
| 5 | Set scratch-pad content, incl. ground states (PUSH/TAXI/…) | `.wsc pad <cs> <text>`, `.wsc state <cs> <token>` |
| 6 | Read & set SID/STAR | `.wsc show <cs>`, `.wsc sid <cs> <SID[/RWY]>`, `.wsc star <cs> <STAR>` |
| — | **Standardized JSON contract** — all of the above as bidirectional `{type, callsign, action, payload}` messages | `.wsc json <message>`, `.wsc events on` |

Full command reference with examples and caveats: **[docs/COMMANDS.md](docs/COMMANDS.md)**.
The JSON contract for external systems: **[docs/PROTOCOL.md](docs/PROTOCOL.md)**.

```json
{ "type": "command", "callsign": "ABC1234", "action": "set_ground_state", "payload": { "state": "PUSH" } }
{ "type": "response", "ok": true, "callsign": "ABC1234", "action": "set_ground_state", "payload": { "message": "..." } }
{ "type": "event", "callsign": "ABC1234", "action": "flight_updated", "payload": { "...": "FlightObject" } }
```

## Quick start

1. Build the DLL (Windows, Visual Studio 2019/2022, **32-bit**):

   ```
   cmake -B build -A Win32
   cmake --build build --config Release
   ```

   Details and troubleshooting: [docs/BUILDING.md](docs/BUILDING.md).

2. In EuroScope: `Other Set` → `Plug-Ins…` → `Load` →
   `build/Release/WebSocketConnector.dll`.

3. Type `.wsc help` in the command line.

4. Connect to your gateway:

   ```
   .wsc gateway url ws://127.0.0.1:3000/session
   .wsc gateway connect
   ```

   The plugin sends a `session_snapshot`, streams events, and answers
   `command` messages per [docs/PROTOCOL.md](docs/PROTOCOL.md).

Test on a [SweatBox/playback session](https://www.euroscope.hu/wp/), not on
the live network, until you are comfortable with what each command does.

## Documentation

| Document | Contents |
|----------|----------|
| [docs/PROTOCOL.md](docs/PROTOCOL.md) | **The JSON contract** — envelope, every action, FlightObject schema, error model |
| [docs/COMMANDS.md](docs/COMMANDS.md) | Every command: syntax, examples, permissions, caveats |
| [docs/BUILDING.md](docs/BUILDING.md) | Toolchain, build steps, loading into EuroScope, debugging |
| [docs/DEVELOPER-GUIDE.md](docs/DEVELOPER-GUIDE.md) | Architecture, EuroScope API gotchas, how to extend |
| [docs/euroscope-plugin-research.md](docs/euroscope-plugin-research.md) | Background research: EuroScope, plugin ecosystem, prior art |
| [sdk/README.md](sdk/README.md) | Provenance of the vendored EuroScope SDK files |

## Important constraints (the short version)

- EuroScope is **32-bit**; the plugin must be built for **Win32/x86**
  (the announced 64-bit transition is suspended as of March 2026).
- All plugin callbacks run on **EuroScope's UI thread** — nothing here may
  block. The future WebSocket layer runs on its own thread and talks to the
  main thread through a queue (see the developer guide).
- The plugin API has **no function to send chat messages**; capabilities
  3 & 4 are implemented by injecting into EuroScope's command line, which is
  undocumented behaviour — treat them as experimental.
- Data modifications generally require you to be **connected and allowed to
  modify the flight** (usually: tracking it); EuroScope refuses otherwise
  and the plugin reports it.

## License

MIT — see [LICENSE](LICENSE). The files in `sdk/` are part of the EuroScope
plug-in development kit © Gergely Csernák and are redistributed here, as is
common practice in the plugin community, solely for building EuroScope
plugins. `third_party/nlohmann/json.hpp` is
[nlohmann/json](https://github.com/nlohmann/json) (MIT, © Niels Lohmann).
