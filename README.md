# euroscope-longpolling-connector

A [EuroScope](https://www.euroscope.hu/wp/) plugin that connects the
controller's session to a web backend over plain **HTTPS**: it streams the
session as JSON messages (flights, positions, snapshots) via `POST` and
receives JSON commands back (modify flight plans, ground states, SID/STAR,
messages) via **long polling**. The backend — e.g. a Laravel app — fans the
data out to browsers however it likes (typically Soketi/Reverb).
Everything is also driven manually via `.lpc` dot-commands for testing.

> **Status: HTTPS transport (long poll + POST) with bearer-token auth.**
> TLS, certificate validation and proxies are handled by Windows (WinHTTP)
> — no third-party network stack. The contract is specified in
> [docs/PROTOCOL.md](docs/PROTOCOL.md). An earlier WebSocket/Pusher
> transport was replaced by this design (git history, `ed8ef6c`).
> Needs real-world testing inside EuroScope — see *Verification status*
> in [docs/BUILDING.md](docs/BUILDING.md).

## What it can do

| # | Capability | Command |
|---|------------|---------|
| 1 | List all flights known to the session | `.lpc list [filter]`, `.lpc show <cs>` |
| 2 | Modify flight-plan / controller-assigned parameters | `.lpc set <cs> <field> <value>` |
| 3 | Send a text message to the primary frequency *(experimental)* | `.lpc freq <text>` |
| 4 | Send a private message to a user *(experimental)* | `.lpc msg <cs> <text>` |
| 5 | Set scratch-pad content, incl. ground states (PUSH/TAXI/…) | `.lpc pad <cs> <text>`, `.lpc state <cs> <token>` |
| 6 | Read & set SID/STAR | `.lpc show <cs>`, `.lpc sid <cs> <SID[/RWY]>`, `.lpc star <cs> <STAR>` |
| 7 | Track control: assume, release, transfer (handoffs) | `.lpc assume <cs>`, `.lpc release <cs>`, `.lpc transfer <cs> <controller>` |
| 8 | List online controllers (ATC positions) | `.lpc atc [filter]` |
| — | **Standardized JSON contract** — all of the above as bidirectional `{type, callsign, action, payload}` messages | `.lpc json <message>`, `.lpc events on` |

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
   `build/Release/EuroscopeLongPollingConnector.dll`.

3. Type `.lpc help` in the command line.

4. Connect to your backend:

   ```
   .lpc gateway config <base64 of url:token>
   .lpc gateway connect
   ```

   (One base64 argument because EuroScope's command line does not pass
   `:` characters through — see [docs/COMMANDS.md](docs/COMMANDS.md) §8.)

   The plugin POSTs a `session_snapshot` + live events to
   `{base}/messages` and long-polls `{base}/poll` for commands, per
   [docs/PROTOCOL.md](docs/PROTOCOL.md).

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
  block. The HTTP transport runs on its own worker threads and talks to the
  main thread through queues (see the developer guide).
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
