# Building the plugin

## Prerequisites

- **Windows** (EuroScope plugins are Windows DLLs; there is no way around
  MSVC — the SDK import library uses the MSVC C++ ABI).
- **Visual Studio 2019 or 2022** with the *Desktop development with C++*
  workload. The free Community edition is fine.
- **CMake ≥ 3.20** (bundled with recent Visual Studio, or from cmake.org).

No third-party dependencies. The EuroScope SDK (header + import library) is
vendored in [`sdk/`](../sdk/).

## Build

From a *Developer Command Prompt* (or any shell where `cmake` is on PATH):

```bat
cmake -B build -A Win32
cmake --build build --config Release
```

The result is `build\Release\WebSocketConnector.dll`.

### Why `-A Win32` is mandatory

EuroScope is a **32-bit** application, and DLLs must match the host
process's architecture — a 64-bit build compiles fine but EuroScope will
silently fail to load it. The CMakeLists refuses to configure for 64-bit to
make this impossible to get wrong. (EuroScope's announced 64-bit transition
was suspended by its developer in March 2026; if it ever ships, this
project needs a rebuild and possibly a new SDK — see
[euroscope-plugin-research.md](euroscope-plugin-research.md).)

### Build settings that matter (already configured in CMakeLists.txt)

| Setting | Why |
|---------|-----|
| `_MBCS`, never `UNICODE` | The EuroScope API and the VATSIM protocol use multi-byte `const char*` strings |
| Static CRT (`/MT`) | Controllers shouldn't need a matching VC++ redistributable for the plugin to load |
| Links `sdk/EuroScopePlugInDll.lib` | Import library for the API exported by EuroScope's own `EuroScopePlugInDll.dll` |
| No `/permissive-` | `EuroScopePlugIn.h` itself is not standards-conformant (`= NULL` pure-specifiers, types used before declaration). It compiles in MSVC's default permissive mode — do **not** enable conformance mode (`/permissive-`) on this target |

## Loading the plugin in EuroScope

1. Start EuroScope (≥ 3.2.x; developed against the 3.2.13-era SDK,
   compatibility code 16).
2. `Other Set` (OTHER SET button) → `Plug-Ins…` → `Load` → select
   `WebSocketConnector.dll`.
3. The plugin greets you in a chat tab named **WSC**. Type `.wsc help`.

If EuroScope refuses to load the DLL:

- **Wrong architecture** — you built 64-bit. Rebuild with `-A Win32`.
- **Compatibility code mismatch** — your EuroScope version expects a
  different SDK. Replace the files in `sdk/` with the ones shipped by your
  EuroScope installation (see [`sdk/README.md`](../sdk/README.md)) and
  rebuild.

## Debugging

- **Never debug against the live VATSIM network.** A breakpoint freezes all
  of EuroScope (plugins run on its UI thread) and your ATC connection times
  out. Use EuroScope's built-in simulator / SweatBox or a session playback.
- Attach: *Debug → Attach to Process → EuroScope.exe* from Visual Studio,
  with the `Debug` build of the DLL loaded.
- Quick print-debugging: `DisplayUserMessage(...)` into the WSC tab (see
  `ConnectorPlugin::Say`).

## Verification status

What has and hasn't been verified, honestly:

- **Verified by executed tests** (`tests/`): the JSON contract layer
  (118 checks) and the real `Gateway` transport — both worker threads,
  long polling, batched POSTs, bearer-token auth, backoff, snapshot
  gating and abort behaviour — end-to-end over live TCP against a
  scripted HTTP backend (40 checks; plain-HTTP test client on POSIX).
- **Verified by compilation only**: the EuroScope-facing code
  (`Actions`, `ConnectorPlugin`, `ChatInjection`) and `WinHttpClient` —
  compiled by the Windows CI job with MSVC against the real SDK header
  and real Windows headers, but never run inside EuroScope.
- **Unverified until someone runs it in EuroScope**: plugin load, command
  behaviour on a live session, settings persistence, the chat-injection
  workaround, TLS against your real backend, and gateway behaviour under
  a real controller workload. Test in a SweatBox session first (see
  below).

## CI

CI ([`.github/workflows/ci.yml`](../.github/workflows/ci.yml)) runs on
every pull request and push to `main`:

- **Linux job** — builds the standalone test project and runs the full
  suite, including the `http_gateway` transport test (POSIX-only: its
  scripted HTTP client/server use POSIX sockets, so Linux is the only
  place it runs).
- **Windows job** — the real `cmake -A Win32` MSVC build of the DLL —
  the only compile of the EuroScope-facing code against the real Windows
  headers — plus the `json_api` test under MSVC. The built DLL is
  uploaded as a workflow artifact.
- **Release job** (pushes to `main` only, after both jobs pass) — bumps
  the semantic version from the latest `vX.Y.Z` git tag (patch by
  default; put `[minor]` or `[major]` in the commit message for bigger
  bumps), rebuilds the DLL with the version compiled in
  (`-DPLUGIN_VERSION=X.Y.Z`), and publishes a GitHub Release with the
  DLL attached. **Grab release DLLs from the Releases page**, not from
  local build trees.

Version identification: a locally built plugin announces itself as
**`v0.0.0-dev`** in the WSC tab; only release builds carry a real
version. If EuroScope greets you with a version that doesn't match what
you expect, you loaded a stale DLL.

The *plugin* itself still deliberately has no non-Windows build path:
don't add Linux-only tooling to the core sources (`src/`); only `tests/`
builds everywhere.
