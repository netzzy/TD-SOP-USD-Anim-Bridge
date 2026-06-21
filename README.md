<img src="docs/assets/td-to-usd.png" alt="TD-SOP-USD-Anim-Bridge" width="600">

# TD-SOP-USD-Anim-Bridge

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

USD export for animated TouchDesigner SOP geometry - one self-contained .usda/.usdc cache.

## Overview

Build procedural (or any) SOP animation in TouchDesigner and write it straight to USD - a single self-contained cache that every major 3D package reads, so you can render it in Blender, Houdini, or wherever you work. The drop-in component writes readable `.usda` out of the box, and compact `.usdc` after installing the optional USD sidecar.

## Features

- Animated `.usda` and `.usdc` export from a SOP input.
- Mesh and point output, including changing topology.
- Point, vertex, and primitive attributes, including custom attributes.
- Experimental native SOP/POP acceleration paths.
- Native TD Mesh primitive tessellation to polygon faces.
- Built-in mappings for normals, UVs, colors, ids, velocities, and extent.
- Half-precision export modes for smaller USD crate files.
- Playback-driven animated export for stateful SOP networks.
- Memory-bounded animated `.usda` streaming with a one-frame peak and cancel/progress.

## Requirements

- TouchDesigner (last tested on build 2025.32820).
- Windows or macOS for the component; `.usda` export has no extra Python package.
- Optional `usd-core` + `numpy` sidecar for `.usdc` export and validation.
- Optional Visual Studio 2022 Build Tools for experimental native plugins on
  Windows x64.

## Installation

Drop `TD_SOP_USD_Anim_Bridge.tox` into a TouchDesigner project and wire a SOP into the component input. The component reads the input through its internal `IN_FOR_EXPORT` SOP and exposes all controls as custom parameters.

For repository development, open `TD-SOP-USD-Anim-Bridge.toe`; the extension source is tracked at `src/ExportExt.py` and synced into the component's `ExportExt` DAT.

## Binary Export Setup

`.usda` works immediately. `.usdc` and `tools/validate_usd.py` need the sidecar packages:

```powershell
python tools/setup.py
```

Inside TouchDesigner, press `Setup Binary Support` on the component to run the same setup with TD's bundled Python. The setup needs internet access for `pip`.

Advanced override order for `.usdc` sidecar execution:

1. `USD Python Executable` custom parameter.
2. `TD_SOP_USD_ANIM_BRIDGE_PYTHON` environment variable.
3. Bundled `tools/.venv-usd` created by setup.

## Usage

| Parameter | Purpose |
| --- | --- |
| `Export Mode` | `Compatible SOP Python` is the default and preserves the full SOP contract. Experimental native modes are opt-in acceleration paths. |
| `File` | Output path. Relative paths are anchored to `project.folder`. |
| `Format` | `usda` for direct ASCII, `usdc` for sidecar-built crate. |
| `Native Status` | Availability/status for optional native plugins. |
| `Export` | Run the export. |
| `Setup Native Support` | Builds optional native plugins when supported by the local toolchain. |
| `Cancel` | Abort an active animated export and clean temporary files. |
| `Animate` | Export the frame range instead of the current frame. |
| `Output FPS` | FPS authored into animated USD metadata. Default expression follows the current TD timeline rate. |
| `Frame Step` | TD source-frame decimation. `1` writes every frame, `2` writes every second frame, etc. Playback still cooks every frame. |
| `Playback Start` | Timeline frame to start playback for pre-roll before writing. |
| `Frame Start` / `Frame End` | Inclusive animated export range. |
| `Progress` | Animated export progress from 0 to 1. |
| `Export Status` | Current animated export state or last result. |
| `Topology Changes` | Enable when point count or face topology can vary. |
| `Half Precision` | `Off`, `Safe Half`, or `All Half`. |
| `USD Python Executable` | Optional path to a `python.exe`/Python binary that has `usd-core` and `numpy` installed. Leave empty for the bundled sidecar. |
| `Binary Status` | Last setup status message. |
| `Setup Binary Support` | Installs/updates `tools/.venv-usd` for `.usdc`. |
| `Temp Folder` | Folder for setup logs, animated export chunks, and temporary `.usda` files. Default `_tdsopusd_temp`; relative paths are anchored to `project.folder`. |

When `Animate` is on, `Export` starts playback and returns immediately. Watch `Export Status` / `Progress`; the USD file is complete only after status becomes `Done`. Animated-only controls are disabled when `Animate` is off, and `Cancel` is enabled only while an animated export is running.

For a 60 FPS TouchDesigner simulation that should become a 30 FPS USD cache while preserving timing, set `Output FPS = 30` and `Frame Step = 2`. TD still plays every simulation frame; the exporter writes source frames `1, 3, 5...` as dense USD timeCodes `1, 2, 3...`.

## Export Modes

| Mode | Input | Attribute support | Status |
| --- | --- | --- | --- |
| `Compatible SOP Python` | SOP input 0 | Point, vertex, and primitive SOP attributes, including custom attrs | Default production path |
| `Experimental Native SOP` | SOP input 0 | Point custom attrs plus standard SOP `N`, `Cd`, and `uv`/texture attrs; generic vertex/primitive custom attrs are rejected before export | Opt-in experimental path for safe SOP inputs |
| `Experimental Native POP` | POP input 1 | POP point, vertex, and primitive float attributes through the C++ POP API | Opt-in experimental path |

Native modes are never allowed to silently drop attributes. If the selected native
path cannot preserve the input safely, export fails before writing the final USD.
If native plugins are missing or built for the wrong platform/TouchDesigner build,
`Compatible SOP Python` still works.

Native plugin parameters are set from `project.folder` at runtime, so the shipped
component is not tied to a developer machine path.

## Output And Size

`.usda` stays readable and is useful for debugging. Animated `.usdc` avoids ASCII formatting during playback by streaming binary numeric chunks, then building the crate out-of-process. Static `.usdc` still transcodes a temporary `.usda`.

`Half Precision` trades file size for accuracy: `Safe Half` shrinks the file by halving attributes where 16-bit loss is hard to see (normals, UVs, colors, velocities), while `All Half` shrinks it further by also halving point positions and widths - visible as jitter or banding on large or fine geometry. Pick `Safe Half` as a default and reach for `All Half` only when size matters more than positional precision.

Animated export chunks, setup logs, and temporary `.usda` files are written under the `Temp Folder` parameter (`_tdsopusd_temp/` by default) and removed on normal completion/cancel/failure.

## Limitations

- NURBS and Bezier input need a Convert SOP first.
- Experimental native plugins are currently Windows x64 / TouchDesigner
  2025.32820 development artifacts, not required for the default SOP exporter.
- The `.usdc` sidecar still authors the final USD layer out-of-process, so peak sidecar RAM can grow with cache size.
- A SOP containing loose points plus faces currently exports one mesh; multi-prim
  output is a known gap.

## Project Structure

- `TD_SOP_USD_Anim_Bridge.tox` - distributable component.
- `TD-SOP-USD-Anim-Bridge.toe` - development TouchDesigner project.
- `src/ExportExt.py` - canonical extension source synced to the DAT.
- `native/` - optional C++ native plugin source and build script.
- `tools/` - setup, validation, `.usdc` chunk-build, and transcode helpers.
- `docs/adr/` - architecture decisions.
- `docs/changelog.md` - shipped changes.

## Development

Edit `src/ExportExt.py`, reload/sync the `ExportExt` DAT, save the `.toe`, and export a fresh `.tox` when component behavior changes. Validate exported USD with:

```powershell
tools/.venv-usd/Scripts/python.exe tools/validate_usd.py export/sop_usd_export.usda
```

## License

MIT, see [LICENSE](LICENSE).
