# EuroScope plug-in SDK (vendored)

| File | Purpose |
|------|---------|
| `EuroScopePlugIn.h` | The complete plugin API: classes, callbacks, constants (`COMPATIBILITY_CODE = 16`) |
| `EuroScopePlugInDll.lib` | 32-bit (x86) MSVC import library for `EuroScopePlugInDll.dll`, which ships with EuroScope |

These files are part of the EuroScope plug-in development kit,
**© Gergely Csernák**, and are redistributed here solely for building
EuroScope plugins — the standard practice in the plugin community (see
UK Controller Plugin, afv-euroscope-bridge, etc.). They are *not* covered
by this repository's MIT license.

## Provenance

Obtained from the community-vendored copy in
[AndyTWF/afv-euroscope-bridge](https://github.com/AndyTWF/afv-euroscope-bridge)
(July 2026), byte-identical to the files distributed with EuroScope 3.2.x.
The import library was verified to be x86 (COFF machine type I386).

## Updating the SDK

The authoritative source is your **EuroScope installation directory** — the
installer ships the current `EuroScopePlugIn.h` and `EuroScopePlugInDll.lib`
(the old standalone `EuroScopePlugIn.zip` download no longer exists). If a
new EuroScope release bumps `COMPATIBILITY_CODE` (grep for it in the
header), copy both files from the new installation over these, rebuild, and
note the supported EuroScope version in the release notes.
