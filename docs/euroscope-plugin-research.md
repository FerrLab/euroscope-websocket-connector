# EuroScope & Plugin Development Research

Research notes gathered as groundwork for building a EuroScope → WebSocket
connector plugin. Last updated: 2026-07-10.

## 1. What EuroScope is

- EuroScope is the most widely used **air traffic controller (radar) client
  for the VATSIM network**, developed by Gergely Csernák
  ([euroscope.hu](https://www.euroscope.hu/wp/)).
- It is a **Windows-only, 32-bit, MFC/C++ desktop application**.
- Current stable release: **v3.2.13 (May 2026)**. Recent cadence:
  3.2.10 (Jun 2025), 3.2.11 (Dec 2025), 3.2.12 (Apr 2026), 3.2.13 (May 2026).
- It renders sector files (`.sct`/`.ese`), connects to VATSIM FSD servers,
  and manages flight plans, radar targets, controller coordination,
  METARs, and ATIS.

### ⚠️ 32-bit vs 64-bit — critical constraint

A plugin API upgrade plus a move to 64-bit was announced in March 2024
([forum thread](https://forum.vatsim.net/t/euroscope-plug-in-api-upgrade/4936)),
but as of **March 2026 the developer confirmed the 64-bit version is
suspended** (post #96: "Unfortunately that means that the 64 bits version is
suspended."). Consequences for this project:

- Plugins **must be compiled as 32-bit (Win32/x86) DLLs**.
- The current plugin API (`COMPATIBILITY_CODE = 16`) remains the target;
  no imminent breaking API change is expected.
- Any third-party libraries (WebSocket, JSON) must be available/buildable
  for x86 Windows (vcpkg triplet `x86-windows` / `x86-windows-static`).

## 2. Plugin architecture

Plugins are **C++ DLLs loaded into EuroScope's process**. The model is
event-driven: EuroScope invokes virtual callback methods on the plugin for
everything from flight-plan updates to controller logins.

### SDK

- The SDK is just two files: **`EuroScopePlugIn.h`** and
  **`EuroScopePlugInDll.lib`**.
- Since v3.2 the standalone "Plugin Environment" installer is gone; the
  files ship **inside the EuroScope installer** (install directory). The
  old standalone download (`euroscope.hu/install/EuroScopePlugIn.zip`) is
  now 404. Many plugin repos vendor the header/lib
  (e.g. [afv-euroscope-bridge](https://github.com/AndyTWF/afv-euroscope-bridge/blob/master/EuroScopePlugIn.h)).
- Official docs: "Plug-In development Guide" (`EuroScopePlugInDevelopment.doc`)
  from the [documentation page](https://www.euroscope.hu/wp/documentation/);
  a markdown mirror exists at
  [FeronFae/EuroScopePlugInSDK](https://github.com/FeronFae/EuroScopePlugInSDK/blob/main/Plug-in%20Developer's%20Guide.md).

### DLL entry points

Two exported functions are mandatory:

```cpp
void __declspec(dllexport) EuroScopePlugInInit(EuroScopePlugIn::CPlugIn** ppPlugInInstance);
// allocate your CPlugIn subclass, return it via ppPlugInInstance

void __declspec(dllexport) EuroScopePlugInExit(void);
// free the instance
```

### CPlugIn subclass

```cpp
class CWebSocketConnectorPlugin : public EuroScopePlugIn::CPlugIn {
public:
    CWebSocketConnectorPlugin() : CPlugIn(
        EuroScopePlugIn::COMPATIBILITY_CODE,   // currently 16
        "WebSocket Connector",                 // plugin name
        "0.1.0",                               // version
        "Author",                              // author
        "License / distribution note")         // copyright
    { ... }
};
```

The compatibility code is checked at load time: **only plug-ins compiled
against a matching API version are loaded** — a version bump in EuroScope
can require recompilation.

### Build requirements

- MSVC (modern Visual Studio works; UK Controller Plugin and others use
  VS2019/2022 toolchains with CMake), **platform Win32/x86**.
- **Multi-byte character set** (`_MBCS`), *not* Unicode — the VATSIM
  protocol/API uses `const char*` strings throughout.
- Link `EuroScopePlugInDll.lib`; include `EuroScopePlugIn.h`.
- MFC optional (shared MFC DLLs if used).
- CMake template with Conan exists:
  [LeoKle/euroscope-plugin-template](https://github.com/LeoKle/euroscope-plugin-template).

### Key API classes (from `EuroScopePlugIn.h`, compat code 16)

| Class | Purpose |
|---|---|
| `CPlugIn` | Base class; registration, iteration, event callbacks |
| `CFlightPlan` | A flight plan: callsign, tracking, handoff, coordination state |
| `CFlightPlanData` | Filed data: origin, destination, route, aircraft type, TAS |
| `CFlightPlanControllerAssignedData` | Controller-assigned: cleared altitude, heading, speed, direct, squawk, scratchpad |
| `CFlightPlanExtractedRoute` | Parsed route waypoints/airways |
| `CFlightPlanPositionPredictions` | Predicted future positions |
| `CRadarTarget` | A radar-visible aircraft; history of ~20 past positions |
| `CRadarTargetPositionData` | Position, pressure/flight level, squawk, ground speed, heading |
| `CController` | Online controller: callsign, frequency, rating, position ID |
| `CRadarScreen` | Custom radar display overlays (OnRefresh drawing, screen objects) |
| `CSectorElement` | Access to sector file elements (airports, runways, fixes…) |
| `CPosition` | Lat/lon with distance/bearing math |
| `CFlightPlanList` | Custom aircraft list windows |
| `CGrountToAirChannel` (sic) | Voice channel info |

### Key data-access / action methods on `CPlugIn`

- Iteration: `FlightPlanSelectFirst()` / `FlightPlanSelectNext(fp)`,
  `RadarTargetSelectFirst()` / `RadarTargetSelectNext(rt)`,
  `FlightPlanSelect(callsign)`, `RadarTargetSelect(callsign)`,
  `FlightPlanSelectASEL()`.
- Self/controllers: `ControllerMyself()`, controller iteration analogues.
- Output: `DisplayUserMessage(...)` writes to a chat handler/tab.
- Registration: `RegisterTagItemType(...)`, `RegisterTagItemFunction(...)`,
  `RegisterDisplayType(...)` (for `CRadarScreen`).

### Event callbacks on `CPlugIn` (the ones a data connector cares about)

| Callback | Fired when |
|---|---|
| `OnFlightPlanFlightPlanDataUpdate(CFlightPlan)` | Filed flight-plan data changes |
| `OnFlightPlanControllerAssignedDataUpdate(CFlightPlan, int DataType)` | Controller-assigned data changes (type flag says what) |
| `OnFlightPlanDisconnect(CFlightPlan)` | Pilot disconnects / FP removed |
| `OnFlightPlanFlightStripPushed(CFlightPlan, from, to)` | Strip pushed between controllers |
| `OnRadarTargetPositionUpdate(CRadarTarget)` | New position for a target (~5 s cadence from network) |
| `OnControllerPositionUpdate(CController)` | Controller login/update |
| `OnControllerDisconnect(CController)` | Controller logoff |
| `OnPlaneInformationUpdate(callsign, livery, type)` | Aircraft info update |
| `OnNewMetarReceived(station, metar)` | METAR update |
| `OnCompileCommand(commandline)` | User typed a dot-command (return `true` if handled) |
| `OnCompileFrequencyChat` / `OnCompilePrivateChat` | Text comms |
| `OnGetTagItem(...)` | EuroScope needs the value of a registered tag item |
| `OnFunctionCall(functionId, ...)` | User clicked a registered tag function |
| `OnAirportRunwayActivityChanged()` | Active runways changed |
| `OnTimer(int Counter)` | **Once per second** — the natural heartbeat/flush point |
| `OnVoiceTransmitStarted/Ended`, `OnVoiceReceiveStarted/Ended` | Voice activity |

`CRadarScreen` gets its own overlapping set (plus `OnRefresh`, ASR
load/save, mouse events on screen objects) — only needed if we draw on the
radar display; a headless data connector can skip it entirely.

### Threading model — the most important design constraint

- **All callbacks run on EuroScope's main (UI) thread.** The developer's
  guide warns that blocking it stalls the whole client and can drop the
  VATSIM connection.
- The EuroScope API is **not documented as thread-safe**; the safe pattern
  used by networked plugins is:
  1. Run the WebSocket server/client on a **background thread** owned by
     the plugin.
  2. In callbacks, **serialize the needed data immediately** (into plain
     structs/JSON) and hand it to the background thread through a
     thread-safe queue — never hold `CFlightPlan`/`CRadarTarget` handles
     across threads.
  3. For inbound commands (WebSocket → EuroScope), queue them and apply on
     the main thread from `OnTimer` (1 Hz) — or keep the connector
     read-only and avoid the problem.

### Loading / distribution

- Users load the DLL via `Other Settings → Plug-Ins…` dialog; the path is
  remembered in the settings/profile files.
- Plugins can be restricted to specific radar displays (ASR types) via the
  same dialog.
- Debugging: never debug against live VATSIM (a breakpoint freezes the
  client and times out the network connection) — use EuroScope's built-in
  playback/simulator sessions.

## 3. Prior art

| Project | Relevance |
|---|---|
| [LeoChen98/\_Euroscope\_DataBridge](https://github.com/LeoChen98/_Euroscope_DataBridge) | "A plugin provides websocket data bridge between Euroscope and your software" — closest existing concept (2023, 1 ⭐, appears stale) |
| [Simplezes/StripCol-Euroscope](https://github.com/Simplezes/StripCol-Euroscope) | Recent (May 2026); syncs aircraft + controller state to a gateway over WebSocket, session pairing codes; plugin acts as WS *client* to a gateway on port 3000 |
| [AndyTWF/afv-euroscope-bridge](https://github.com/AndyTWF/afv-euroscope-bridge) | Bridges the Audio-for-VATSIM standalone client into EuroScope; good minimal example of a networked plugin + vendored SDK header |
| [vACDM plugin](https://github.com/vACDM/vacdm-plugin) | Talks HTTP/REST to a backend from inside EuroScope |
| [ECFMP/plugin-sdk](https://github.com/ECFMP/plugin-sdk) | Modern, tested plugin-SDK layer with an `HttpClient` abstraction — good architectural reference |
| [VATSIM-UK/uk-controller-plugin](https://github.com/VATSIM-UK/uk-controller-plugin) | The largest actively maintained plugin; CMake build, CI, good patterns for testable wrappers around the ES API |
| [EvenAR/euroscope-aman](https://github.com/EvenAR/euroscope-aman) | Arrival manager using ES position predictions; native window UI |
| [LeoKle/euroscope-plugin-template](https://github.com/LeoKle/euroscope-plugin-template) | CMake + Conan plugin template |

Takeaway: no widely adopted, maintained, general-purpose WebSocket
connector exists — the niche this repo targets is real.

## 4. Implications for this project (researched under its original name, euroscope-websocket-connector)

1. **Target**: C++ DLL, Win32 (x86), `_MBCS`, `COMPATIBILITY_CODE = 16`,
   built with MSVC (CMake recommended for CI-friendliness).
2. **WebSocket library**: must build for 32-bit Windows and run happily on
   a background thread inside someone else's process. Candidates:
   IXWebSocket (simple, no external event loop), Boost.Beast, or
   websocketpp/standalone-Asio. Header-only or statically linked
   (`x86-windows-static`) avoids DLL-hell in users' installs.
3. **Server vs client**: running a local **WS server inside the plugin**
   (e.g. `ws://127.0.0.1:<port>`) lets arbitrary local consumers
   (web dashboards, Electron apps, OBS overlays) connect without extra
   infrastructure; StripCol instead dials out to a gateway. Decision
   depends on the intended consumers.
4. **Data flow**: serialize on the main thread in the `On…Update`
   callbacks → lock-free/mutex queue → broadcast from the WS thread.
   Full-state snapshot on client connect (via `FlightPlanSelectFirst/Next`
   + `RadarTargetSelectFirst/Next`), incremental events afterwards.
5. **Heartbeat**: `OnTimer` (1 Hz) for flushes, reconnect logic, and
   applying any queued inbound actions on the main thread.
6. **Recompilation risk**: EuroScope point releases can bump the
   compatibility code; vendor the exact `EuroScopePlugIn.h`/`.lib` used
   and document which EuroScope version each release targets.

## Sources

- [EuroScope homepage](https://www.euroscope.hu/wp/) · [documentation](https://www.euroscope.hu/wp/documentation/) · [installation](https://www.euroscope.hu/wp/installation/) · [public releases](https://www.euroscope.hu/wp/category/public-release/) · [v3.2.10 release post](https://www.euroscope.hu/wp/2025/06/22/v3-2-10/)
- [Plug-in API upgrade thread (forum.vatsim.net)](https://forum.vatsim.net/t/euroscope-plug-in-api-upgrade/4936) — 64-bit plan (2024) and suspension (post #96, Mar 2026)
- [Plugin Environment for v3.2+ thread](https://forum.vatsim.net/t/plugin-environment-euroscope-v3-2/4144) — SDK distribution
- [Plug-in Developer's Guide (markdown mirror)](https://github.com/FeronFae/EuroScopePlugInSDK/blob/main/Plug-in%20Developer's%20Guide.md)
- [`EuroScopePlugIn.h` (vendored copy)](https://github.com/AndyTWF/afv-euroscope-bridge/blob/master/EuroScopePlugIn.h) — API surface inspected directly
- Prior-art repos linked in §3
