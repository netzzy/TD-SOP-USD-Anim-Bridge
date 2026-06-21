# Changelog

## [2026-06-21] Add Experimental Native Backend Modes

- Added `Export Mode`, `Native Status`, and `Setup Native Support` to separate the
  default compatible SOP Python exporter from optional native acceleration paths.
- Added experimental CPlusPlus SOP and POP chunk writers plus `native/build.ps1`;
  native build artifacts are ignored and should be distributed as release
  artifacts, not committed source.
- Experimental Native SOP now exports safe SOP inputs with point custom attrs plus
  standard `N`, `Cd`, and texture coordinates, and rejects unsupported generic
  vertex/primitive custom attributes before writing output.
- Experimental Native POP uses a second POP input and preserves point, vertex, and
  primitive float attributes through the existing chunk sidecar path.
- Native plugin parameters are authored from `project.folder` expressions so the
  component does not save absolute developer-machine DLL paths.
- Native command parameters are reset after native exports so temporary folders and
  status-file paths are not persisted into saved `.toe`/`.tox` files.
- Validated Compatible SOP Python, Experimental Native SOP, and Experimental
  Native POP in static/animated `.usda` and `.usdc` smoke exports; Native SOP also
  verifies fail-fast behavior on the current `vertex:T` SOP case.
- Updated README, native README, and ADRs with the compatibility matrix and opt-in
  native backend policy.
- Affected files: `src/ExportExt.py`, `native/*`, `.gitignore`, `README.md`,
  `docs/adr/*`, `docs/changelog.md`, `TD-SOP-USD-Anim-Bridge.toe`,
  `TD_SOP_USD_Anim_Bridge.tox`.
- Migrations: existing users keep `Compatible SOP Python`; native users must build
  native plugins with `native/build.ps1` or press `Setup Native Support`.

## [2026-06-21] Add Animated USDC Binary Chunk Builder

- Animated `Format = usdc` now writes binary numeric chunks during playback and
  builds the final crate with `tools/build_usdc_from_chunks.py`, avoiding the old
  hot-path ASCII formatting/transcode route.
- Added `numpy` to the optional USD sidecar requirements and updated setup/status,
  README, tools docs, ADRs, and agent architecture notes to describe the new split:
  animated `.usdc` uses chunks; static `.usdc` still transcodes a temporary `.usda`.
- Affected files: `src/ExportExt.py`, `tools/build_usdc_from_chunks.py`,
  `tools/requirements.txt`, `tools/setup.py`, `README.md`, `tools/README.md`,
  `AGENTS.md`, `docs/adr/*`, `docs/changelog.md`,
  `TD-SOP-USD-Anim-Bridge.toe`, `TD_SOP_USD_Anim_Bridge.tox`.
- Migrations: rerun `python tools/setup.py` or press `Setup Binary Support` if the
  existing sidecar environment does not have `numpy`.

## [2026-06-21] Preserve Mesh Schema on Empty Animated Frames

- Fixed animated exports where a mesh sequence contains fully empty frames. The
  playback writer now defers schema creation through leading empty frames and
  coerces later empty frames to the established Mesh/Points schema instead of
  misclassifying `0 prims` as `UsdGeomPoints`.
- This keeps “objects exist / no objects exist” changes in the topology/counts
  bucket, so they work with the existing `Topology Changes` toggle instead of
  failing as a geometry-kind schema change.
- Affected files: `src/ExportExt.py`, `docs/changelog.md`,
  `TD-SOP-USD-Anim-Bridge.toe`, `TD_SOP_USD_Anim_Bridge.tox`.
- Migrations: none.

## [2026-06-15] Add Project Logo to README

- Added the project logo (`docs/assets/td-to-usd.png`) to the top of the README and
  normalized README punctuation to plain ASCII.
- Affected files: `README.md`, `docs/assets/td-to-usd.png`, `docs/changelog.md`.
- Migrations: none.

## [2026-06-15] Remove Legacy Agent Instruction Shim

- Removed the legacy root-level agent instruction shim; `AGENTS.md` remains the
  canonical project instruction file.
- Affected files: `docs/changelog.md`; removed legacy agent instruction shim.
- Migrations: none.

## [2026-06-15] Rename Project and Clarify USD-Only Scope

- Renamed the project to `TD-SOP-USD-Anim-Bridge` and the TouchDesigner component /
  distributable to `TD_SOP_USD_Anim_Bridge`.
- Repositioned documentation and architecture notes around a single purpose:
  exporting animated TouchDesigner SOP geometry to `.usda` / `.usdc` USD caches.
- Renamed the default sample output path to `export/sop_usd_export.*`.
- Affected files: `README.md`, `AGENTS.md`, `src/ExportExt.py`,
  `tools/README.md`, `tools/validate_usd.py`, `docs/adr/*`,
  `docs/backlog.md`, `docs/changelog.md`, `TD-SOP-USD-Anim-Bridge.toe`,
  `TD_SOP_USD_Anim_Bridge.tox`.
- Migrations: update local component and `.tox` references to
  `TD_SOP_USD_Anim_Bridge`.

## [2026-06-15] README Positioning Cleanup

- Reworded the README overview to position the tool plainly as a TouchDesigner
  SOP-to-USD cache exporter, dropping the "per-frame export plus Houdini stitch"
  framing (that was a personal workflow, not a common one).
- Moved the "Screenshot/GIF preview" note out of README Limitations into
  `docs/backlog.md` as an open work item.
- Replaced the unverified `TouchDesigner 2025.x` requirement with the specific last
  tested build (`2025.32820`).
- Replaced the unrepresentative byte-size table in "Output And Size" with a
  qualitative precision/size trade-off description for the `Half Precision` modes.
- Removed the unverified changing-topology limitation (per-DCC claims about Blender /
  Houdini) from README, and softened the same note in AGENTS to drop named DCCs.
- Removed the unverified winding/orientation limitation from README (the exporter
  authors explicit normals, so shading is unaffected; the technical note stays only in
  the internal backlog).
- Made `docs/backlog.md` private: dropped it from the README project structure and
  added it to `.gitignore` so the internal development backlog is not published.
- Reordered the README Usage table to match the manually reordered custom-parameter
  order on the live component (File, Format, Export, Cancel, Animate, Output FPS,
  Frame Step, Playback Start, Frame Start/End, Progress, Export Status, Topology
  Changes, Half Precision, USD Python Executable, Binary Status, Setup Binary Support,
  Temp Folder).
- Affected files: `README.md`, `AGENTS.md`, `.gitignore`, `docs/changelog.md`.
- Migrations: none.

## [2026-06-15] Recover Orphaned Playback Export State

- `CancelExport()` now detects a stored active playback-export flag even when the
  in-memory extension state was lost by extension reinitialization. In that orphaned
  state, Cancel disables `exec_playback`, stops playback, clears
  `_tdsopusd_playback_export`, reports how many frames had been written, and refreshes
  the parameter enable states.
- This prevents `Export`, `Animate`, and animated-range parameters from staying
  disabled after a reinit interrupted an active export.
- Affected files: `src/ExportExt.py`, `docs/changelog.md`,
  `TD-SOP-USD-Anim-Bridge.toe`, `TD_SOP_USD_Anim_Bridge.tox`.
- Migrations: none.

## [2026-06-15] Restore Timeline Working Range

- Animated export now snapshots and restores TouchDesigner's visible working range
  (`rangeStart`/`rangeEnd`) along with playback state. `REnd` is still updated during
  export so the playback window is correct, but it no longer stays changed after
  done/cancel/failure while `End` returns to its original value.
- Updated ADR 0009 and AGENTS to state that export restores the full timeline state
  including the visible working range.
- Affected files: `src/ExportExt.py`, `docs/adr/0009-playback-driven-animated-export.md`,
  `AGENTS.md`, `docs/changelog.md`, `TD-SOP-USD-Anim-Bridge.toe`,
  `TD_SOP_USD_Anim_Bridge.tox`.
- Migrations: none.

## [2026-06-14] Sync Timeline Working Range End

- Animated export now explicitly sets TouchDesigner's visible working range
  (`rangeStart`/`rangeEnd`) to `[Frame Start, Frame End]` before playback. This fixes
  the case where decreasing `Frame End` visibly moved REnd through TD's clamp, but
  increasing `Frame End` did not expand REnd again.
- The exporter still restores playback state after done/cancel/failure, but leaves
  the visible working range aligned to the last export window for user feedback.
- Affected files: `src/ExportExt.py`, `docs/adr/0009-playback-driven-animated-export.md`,
  `AGENTS.md`, `docs/changelog.md`, `TD-SOP-USD-Anim-Bridge.toe`,
  `TD_SOP_USD_Anim_Bridge.tox`.
- Migrations: none.

## [2026-06-14] Output FPS and Frame Step

- Added animated-export parameters `Output FPS` and `Frame Step`. `Output FPS`
  authors `framesPerSecond`/`timeCodesPerSecond` and defaults to an expression that
  follows the current TouchDesigner timeline rate; `Frame Step` controls source-frame
  decimation.
- Playback still cooks every TD frame for stateful simulations, but only source
  frames matching the step are written. Written source frames are mapped to dense USD
  timeCodes starting at `Frame Start`, so a 60 FPS sim can export a 30 FPS cache with
  `Output FPS = 30` and `Frame Step = 2` without stretching time.
- Updated ADR 0009, README, and AGENTS to document the explicit FPS/step contract.
- Affected files: `src/ExportExt.py`, `README.md`,
  `docs/adr/0009-playback-driven-animated-export.md`, `AGENTS.md`,
  `docs/changelog.md`, `TD-SOP-USD-Anim-Bridge.toe`, `TD_SOP_USD_Anim_Bridge.tox`.
- Migrations: none.

## [2026-06-14] Configurable Local Temp Folder

- Added a `Temp Folder` component parameter for setup logs, animated export chunks,
  and temporary `.usda` files. It defaults to project-local `_tdsopusd_temp`; relative
  paths are anchored to `project.folder`.
- Export and setup temp directories now use the configured folder instead of the OS
  temp directory, so crash leftovers are easy to find next to the project.
- Stale cleanup only removes marker-bearing `export_*` / `setup_*` directories plus
  stale `.tmp.usda` files created by this component, reducing the risk of deleting
  unrelated user folders if the temp folder is changed.
- Affected files: `src/ExportExt.py`, `.gitignore`, `README.md`, `AGENTS.md`,
  `docs/changelog.md`, `TD-SOP-USD-Anim-Bridge.toe`, `TD_SOP_USD_Anim_Bridge.tox`.
- Migrations: none.

## [2026-06-14] Clarify USD Python Executable Parameter

- Renamed the visible `USD Python` label to `USD Python Executable` and changed its
  TouchDesigner custom parameter style from plain string to `File`, because the value
  is a Python executable path such as `tools/.venv-usd/Scripts/python.exe`, not a
  library path or folder.
- Kept the internal parameter name `Usdpython` for compatibility with existing code
  and `.tox` users.
- Updated README, tools docs, AGENTS, and setup error hints to describe the value as
  an optional interpreter override; leaving it empty uses `TD_SOP_USD_ANIM_BRIDGE_PYTHON` or the
  bundled sidecar.
- Affected files: `src/ExportExt.py`, `README.md`, `tools/README.md`, `AGENTS.md`,
  `docs/changelog.md`, `TD-SOP-USD-Anim-Bridge.toe`, `TD_SOP_USD_Anim_Bridge.tox`.
- Migrations: none.

## [2026-06-14] Setup Binary Support Robustness

- `Setup Binary Support` now first checks whether the bundled `tools/.venv-usd`
  Python can import `pxr.Usd`; if it can, the component reports `Binary support
  ready` without launching another `pip install` subprocess.
- `SetupBinaryPoll` now prints the setup log when a setup process exits without a
  status file, and treats the sidecar as ready if `usd-core` is importable despite
  the missing status.
- `tools/setup.py` now writes failure status for `KeyboardInterrupt`/other
  `BaseException` interruptions, so TouchDesigner does not lose the real cause and
  fall back to an opaque "process exited without status".
- Verified from the live component: the existing sidecar imports `pxr.Usd`,
  `Setup Binary Support` reports `Binary support ready`, and no setup process remains
  stored.
- Affected files: `src/ExportExt.py`, `tools/setup.py`, `docs/changelog.md`,
  `TD-SOP-USD-Anim-Bridge.toe`, `TD_SOP_USD_Anim_Bridge.tox`.
- Migrations: none.

## [2026-06-14] Playback-Driven Animated Export

- Replaced the animated scrub/force-cook driver with playback-driven export.
  Animated `Export()` now starts a state machine, runs TD playback with
  `project.realTime = False`, samples frames from `exec_playback.onFrameEnd`, streams
  them through the existing per-attribute temp-file writer, and restores prior
  timeline state on done/cancel/failure.
- Added `Playback Start`, `Cancel`, `Progress`, and `Export Status` custom
  parameters. `parexec_export` now dispatches `Cancel`; `exec_playback` stays disabled
  except during an active animated export.
- Added `RefreshUiState()` so animated-only controls are disabled when `Animate` is
  off, `Cancel` is enabled only during an active export, and `Export`/`Animate` are
  locked while playback export is running.
- Fixed boolean custom-parameter reads to use explicit evaluated values instead of
  `bool(par)`, preventing disabled toggles such as `Animate` from being treated as
  enabled.
- Hardened playback finalization for long exports: `PlaybackFrame` now writes only
  the exact next expected frame, fails immediately if TD skips past it, finalizes as
  soon as the expected frame count is written, and clamps the temporary playback end
  to `Frame End` until the saved timeline state is restored.
- Added ADR 0009 and updated ADR 0004, `AGENTS.md`, `README.md`, and
  `docs/backlog.md` to reflect the new animated export contract.
- Verification: `python -m py_compile src/ExportExt.py`; scoped TouchDesigner errors
  for `/project1/TD_SOP_USD_Anim_Bridge` report 0 errors / 0 warnings; live UI-state
  checks passed for `Animate` off/on and simulated active export; dry state-machine
  tests verified finish-on-final-count and skipped-frame failure. Full USD export was
  not run in this pass because export validation is being done manually in TD.
- Affected files: `src/ExportExt.py`, `AGENTS.md`, `README.md`,
  `docs/adr/0004-streaming-export.md`,
  `docs/adr/0009-playback-driven-animated-export.md`, `docs/adr/README.md`,
  `docs/backlog.md`, `docs/changelog.md`, `TD-SOP-USD-Anim-Bridge.toe`,
  `TD_SOP_USD_Anim_Bridge.tox`.
- Migrations: none.

## [2026-06-14] Particle / Point-Primitive Support

- Geometry-kind detection now routes particle/point primitives to the point-cloud
  path. `_isMeshGeo` classifies prims: `Poly`/`Mesh` → mesh, `Prim`/`Part`/`Particle`
  (a Particle SOP wraps its points in one `Prim`) → `UsdGeomPoints`; mixed/unknown
  raises. The kind is stored as `ir['isMesh']` and read everywhere instead of
  recomputing `numPrims > 0`, so `_build`, `_exportStreaming`, `_schema`, and
  `_topoKey` treat particles as points (point count may vary per frame → handled by
  the `Topology Changes` toggle + `ids`).
- `_resolveAttr` maps `PartVel`(3) as well as `v`(3) to the built-in `velocities`.
- Verified on a Particle SOP (prim class `Prim`): exports as `def Points` with
  positions, `extent`, `widths`, `velocities` (from `v`), and remaining particle
  attributes as primvars; `validate_usd.py` passes (exit 0). Re-exported
  `TD_SOP_USD_Anim_Bridge.tox`.
- Affected files: `src/ExportExt.py`, `AGENTS.md`, `docs/changelog.md`,
  `TD-SOP-USD-Anim-Bridge.toe`, `TD_SOP_USD_Anim_Bridge.tox`.
- Migrations: none.

## [2026-06-14] GitHub-Ready Distribution

- Added a root `README.md`, MIT `LICENSE`, `tools/README.md`,
  `tools/requirements.txt`, and `tools/setup.py` so `.usda` works out of the box and
  `.usdc`/validation sidecar support is reproducible.
- Externalized the exporter source to `src/ExportExt.py`, synced the live
  `ExportExt` DAT to that file, and exported `TD_SOP_USD_Anim_Bridge.tox` as the
  distributable component.
- Added `USD Python`, `Setup Binary Support`, and `Binary Status` custom parameters.
  `.usdc` transcode now resolves Python from the parameter, `TD_SOP_USD_ANIM_BRIDGE_PYTHON`, or
  the bundled `tools/.venv-usd`; setup runs with TD's bundled Python and reports
  status through the component.
- Added ADR 0008 for distribution/source/sidecar decisions and updated `AGENTS.md`,
  `docs/backlog.md`, and `.gitignore` for the new project shape.
- Affected files: `README.md`, `LICENSE`, `.gitignore`, `AGENTS.md`,
  `TD-SOP-USD-Anim-Bridge.toe`, `TD_SOP_USD_Anim_Bridge.tox`, `src/ExportExt.py`,
  `tools/README.md`, `tools/requirements.txt`, `tools/setup.py`,
  `docs/adr/0008-distribution-tox-external-source-sidecar.md`,
  `docs/adr/README.md`, `docs/backlog.md`, `docs/changelog.md`.
- Migrations: none.

## [2026-06-14] Serialization Layer Upgrade

- Added `Half Precision` export mode to `TD_SOP_USD_Anim_Bridge` (`off`/`safe`/`all`).
  Safe mode halves normals, UV/st, velocities, and `Cd`; all mode also halves points,
  widths, and generic float primvars.
- The streaming writer now deduplicates constant samplers and emits defaults instead
  of repeated identical timeSamples, including constant topology when `Topology
  Changes` is enabled but the topology does not actually vary.
- Added built-in point `v` -> USD `velocities` mapping and authored `float3[] extent`
  from sampled bounds for static and animated exports.
- Extended `tools/validate_usd.py` to gather time codes from all authored geometry
  attributes and to validate `velocities` length plus `extent` length.
- Affected files: `AGENTS.md`, `docs/backlog.md`, `docs/changelog.md`,
  `tools/validate_usd.py`, `TD-SOP-USD-Anim-Bridge.toe` (live project: `ExportExt`,
  new `Halfprecision` parameter).
- Migrations: none.

## [2026-06-14] Binary USD Export via Sidecar Transcode

- Added a `Format` menu to `TD_SOP_USD_Anim_Bridge` (`usda`/`usdc`). `.usda` export keeps
  the existing direct writer; `.usdc` export writes a temporary `.usda`, transcodes it
  out-of-process with `tools/.venv-usd` + `tools/transcode_usd.py`, and deletes the
  temp file.
- Added ADR 0007 documenting binary crate export and its RAM trade-off, and updated
  ADR 0002/0004 plus `AGENTS.md` so `.usdc` is no longer described as out of scope.
- Updated `tools/validate_usd.py` help text to cover `.usda` and `.usdc`.
- Affected files: `AGENTS.md`, `docs/adr/0002-self-written-usda.md`,
  `docs/adr/0004-streaming-export.md`,
  `docs/adr/0007-binary-usdc-via-transcode.md`, `docs/adr/README.md`,
  `docs/changelog.md`, `tools/transcode_usd.py`, `tools/validate_usd.py`,
  `TD-SOP-USD-Anim-Bridge.toe` (live project: `ExportExt`, new `Format` parameter).
- Migrations: none.

## [2026-06-14] Internal Documentation: ADRs and Architecture Summary

- Added an Architecture Decision Records folder `docs/adr/` with an index
  (`README.md`) and six records: USD-only scope, self-written `.usda`,
  module encapsulation, streaming memory-bounded export, native Mesh tessellation,
  and the usd-core validation venv.
- Added a "Current Architecture and Status" section to `AGENTS.md` (data flow,
  attribute mapping, stage metadata, UI, validation, known limitations) so a new
  agent or a fresh context can orient quickly, with links to the ADRs and changelog.
- Affected files: `AGENTS.md`, `docs/adr/*`, `docs/changelog.md`.
- Migrations: none.

## [2026-06-14] Streaming Animated Export (memory-bounded)

- Rewrote the animated export path to stream: `Export()` now calls `_exportStreaming`, which makes one timeline pass and appends each frame's `<frame>: <array>,` line to a per-attribute temp file, then assembles the final `.usda` by streaming those temp files into each attribute's `.timeSamples` block (`shutil.copyfileobj`). Peak memory is one frame, so exportable size is bounded by disk, not RAM. Removed the in-memory whole-range list (`_sampleRange`), the whole-file string build, and the live `_animationProfile` topology scan.
- Whether topology changes is now declared by the user via a new `Topology Changes` toggle (passed as `topoVaries`) instead of being detected by pre-scanning. A cheap safety net still runs during the single pass: if the toggle is off but topology actually changes, the export raises and asks the user to enable it (`_topoKey`).
- Static (Animate off) export keeps the simple in-memory single-frame `_build`.
- Verified on a 722-point / 760-face TD-Mesh sphere over 100 frames: toggle on (topology time-sampled) and toggle off (topology authored once) both pass `validate_usd.py` (all 100 time codes coherent, exit 0).
- Affected files: `docs/changelog.md`, `TD-SOP-USD-Anim-Bridge.toe` (live project: `ExportExt`, new `Topologychanges` parameter).
- Migrations: none.

## [2026-06-14] Native TD Mesh-Primitive Tessellation

- The sampler now accepts the TD `Mesh` primitive type natively instead of rejecting it. `_faces` resolves each prim to output faces: `Poly` passes through, `Mesh` is tessellated from its `numRows`/`numCols` grid (honoring `closedU`/`closedV` wrap) into quads via `_meshFaces`. `_dedupeRing` drops duplicate ring points so collapsed pole rows become triangles and fully degenerate cells are dropped. Unsupported types (NURBS/Bezier) still raise with a Convert-SOP hint.
- Prim attributes are now replicated per output face (a single Mesh prim produces many faces), keeping `uniform` interpolation counts correct.
- Verified against a TD `Mesh` sphere: tessellation matches a Convert SOP exactly (760 faces = 80 pole triangles + 680 quads, 722 points, no degenerate/out-of-range faces), and the built mesh passes `validate_usd.py` (points/faces/face-verts coherent, exit 0).
- Affected files: `docs/changelog.md`, `TD-SOP-USD-Anim-Bridge.toe` (live project: `ExportExt`).
- Migrations: none.

## [2026-06-14] Set USD-Only Scope

- Set the project scope to USD output and updated the Agent Context and Possible
  Directions in `AGENTS.md` accordingly. The direction is `.usda` for readable/debug
  output plus `.usdc` for production-size USD caches.
- Affected files: `AGENTS.md`, `docs/changelog.md`.
- Migrations: none.

## [2026-06-14] M4 Changing-Topology Export and usd-core Validation

- The animated path no longer assumes constant topology. `_animationProfile` replaces the old hard guard: it allows point count and face topology to vary across the range, while still requiring a constant geometry kind (Points vs Mesh) and a constant attribute schema (names/sizes), raising clear errors otherwise. When topology varies, `faceVertexCounts`/`faceVertexIndices` are time-sampled too (`>1` sample marks the mesh topologically varying); per-frame value arrays already tolerate varying lengths and fall back to held interpolation in USD.
- Added a `UsdGeomPoints` `ids` (int64) mapping from a TD `id`/`ids` point attribute, and a guard that rejects non-`Poly` input prims (e.g. the TD "Mesh"/NURBS primitive types) with a message to switch the SOP to Polygon or add a Convert SOP.
- Added an isolated USD validation tool: `tools/validate_usd.py` run from a `tools/.venv-usd` virtualenv with `usd-core` (TD ships USD as C++ only). It opens the `.usda` and asserts per-time-code element-count coherence (positions/normals/ids/primvars vs the count each interpolation implies; `sum(faceVertexCounts) == len(faceVertexIndices)`; indices in range).
- Verified on a Polygon sphere with animated Frequency over 100 frames: topology alternates 52pt/80face and 22pt/20face per frame, and `validate_usd.py` reports all 100 time codes coherent (exit 0). Known consumer limitation: Blender's USD importer typically reads topology once and will not animate changing topology; usdview/Houdini handle it.
- Affected files: `docs/changelog.md`, `.gitignore` (ignore `tools/.venv-usd/`), `tools/validate_usd.py` (new), `TD-SOP-USD-Anim-Bridge.toe` (live project: `ExportExt`).
- Migrations: none.

## [2026-06-14] M3 Polygon Mesh Export (UsdGeomMesh)

- Extended the sampler to read polygon topology and all attribute classes: `faceVertexCounts`/`faceVertexIndices` from prim vertices, point attributes, vertex attributes in face-vertex order, and prim attributes.
- The writer now picks the geometry kind from the data (`numPrims == 0` -> `Points`, otherwise `Mesh`) and shares one code path for static and animated output. Meshes emit `faceVertexCounts`/`faceVertexIndices` once (constant topology) plus `subdivisionScheme = "none"`, and time-sample only the per-frame values.
- Mapped TD attribute classes to USD interpolations: point -> `vertex`, vertex -> `faceVarying`, prim -> `uniform`. Role mappings: point `N` -> `normals`, vertex `uv` -> `texCoord2f primvars:st` (components u, v); everything else -> `primvars:<name>` typed by component size.
- Verified on an 80-triangle, 42-point sphere: static and animated (frames 1-3) exports are well-formed, topology stays constant, and positions/normals/UVs carry the expected interpolation. Winding/orientation correctness is still to be confirmed by DCC import (USD defaults to `rightHanded`).
- Affected files: `docs/changelog.md`, `TD-SOP-USD-Anim-Bridge.toe` (live project: `ExportExt`).
- Migrations: none.

## [2026-06-14] M2 Animated USDA Point Cloud (Time Samples)

- Added the M2 animated writer to `ExportExt`: `Export()` now branches on an `Animate` toggle. When on, it scrubs the timeline across `[Framestart, Frameend]`, force-cooks the input each frame (playback paused, so no frame is dropped regardless of the Real-Time toggle), and writes USD `timeSamples` for positions, normals, and every point primvar, plus stage time metadata (`framesPerSecond`/`timeCodesPerSecond` from the live timeline rate, `startTimeCode`/`endTimeCode`).
- Added a constant-topology guard: export raises if the point count or attribute set changes across the range (changing topology is deferred to M4).
- Exposed `Animate`, `Frame Start`, and `Frame End` custom parameters on the container COMP.
- Verified per-frame motion of positions and normals against direct SOP scrubbing for the 10-point cloud; a non-animated custom attribute (`test`) correctly serializes as identical samples across frames.
- Affected files: `docs/changelog.md`, `TD-SOP-USD-Anim-Bridge.toe` (live project: `ExportExt`, `Animate`/`Framestart`/`Frameend` parameters).
- Migrations: none.

## [2026-06-14] M1 Static USDA Point Cloud Writer

- Added the M1 writer to `ExportExt`: `Export()` serializes the IR to a static `.usda` `UsdGeomPoints` prim with positions, constant `widths`, point-class `N` emitted as native `normals`, and any other point attribute emitted generically as a `primvars:<name>` array typed by component size, all at `interpolation = "vertex"` (`upAxis = "Y"`).
- Exposed module UI as custom parameters on the container COMP: `File` (default `export/sop_usd_export.usda`, stored relative and anchored to `project.folder` at write time) and an `Export` pulse, dispatched through a `parexec_export` parameterexecute DAT (extension pulse callbacks do not fire on their own).
- Verified the written file against the IR for the 10-point test cloud (positions and normals match); button-pulse and direct `Export()` both produce the file. System Python lacks `usd-core`, so parse validation is by inspection and DCC import for now.
- Affected files: `docs/changelog.md`, `TD-SOP-USD-Anim-Bridge.toe` (live project: `ExportExt`, custom parameters, `parexec_export` DAT). Generated output `export/sop_usd_export.usda` is a build artifact, not committed.
- Migrations: none.

## [2026-06-14] M0 Sampler and Project-Save Rule

- Implemented the M0 sampler inside the `TD_SOP_USD_Anim_Bridge` module: an `ExportExt` Python extension on the container COMP reads `IN_FOR_EXPORT` into a plain-Python intermediate representation (point count, primitive count, `P`, and all point attributes read generically). Verified the IR matches the SOP exactly for the 10-point test cloud (positions and randomized normals, 0 mismatches).
- Added an `AGENTS.md` rule to save the TouchDesigner project with `project.save()` before and after any important or dangerous change.
- Affected files: `AGENTS.md`, `docs/changelog.md`, `TD-SOP-USD-Anim-Bridge.toe` (live project: `ExportExt` DAT and container extension parameters).
- Migrations: none.

## [2026-06-14] Document Module Structure and Encapsulation

- Added an `AGENTS.md` section defining the `TD_SOP_USD_Anim_Bridge` Base COMP as the module boundary: geometry is read from the inner `IN_FOR_EXPORT` In SOP, all logic lives in a promoted Python extension on the COMP, all UI is exposed as custom parameters, and references stay relative for copy/`.tox` safety.
- Affected files: `AGENTS.md`, `docs/changelog.md`.
- Migrations: none.

## [2026-06-14] Document TouchDesigner MCP Access and Native USD Limits

- Added an `AGENTS.md` section stating the agent works with TouchDesigner through the `twozero_td` MCP server and should use the live instance via MCP rather than guessing project state.
- Recorded that TouchDesigner has no native USD writer, so the exporter must write
  USD outside TD's native operators.
- Affected files: `AGENTS.md`, `docs/changelog.md`.
- Migrations: none.

## [2026-06-14] Add Local Documentation Rules

- Added project rules requiring local TouchDesigner and OpenUSD documentation to be inspected before making API claims.
- Added `Docs_USD/` as an ignored local documentation dump location.
- Added TouchDesigner DAT script style rules and PLAYER_COPY_MANAGER copy-safety guidance.
- Affected files: `AGENTS.md`, `.gitignore`, `docs/changelog.md`.
- Migrations: none.

## [2026-06-14] Enforce English Project Documentation

- Translated `AGENTS.md` to English and added an English-only rule for documentation and external project communication.
- Affected files: `AGENTS.md`, `docs/changelog.md`.
- Migrations: none.

## [2026-06-14] Add Brevity Rule

- Added a project rule requiring concise default responses and limiting routine structure, preambles, and final summaries.
- Affected files: `AGENTS.md`, `docs/changelog.md`.
- Migrations: none.

## [2026-06-14] Add Data-Driven Configuration Rule

- Added a project rule banning hardcoded model/provider behavior in favor of YAML-driven configuration.
- Affected files: `AGENTS.md`, `docs/changelog.md`.
- Migrations: none.

## [2026-06-14] Add Agent Review Discipline Rules

- Added a project rule requiring agents to challenge strategy, positioning, architecture, and UX changes before agreeing.
- Expanded the investigation rule to require inspecting referenced files or paths before explaining or proposing fixes.
- Expanded the investigation rule to require rigorous code search and review of local style, conventions, and abstractions before new feature work.
- Affected files: `AGENTS.md`, `docs/changelog.md`.
- Migrations: none.

## [2026-06-14] Add Agent Documentation Rules

- Added a project rule requiring `docs/changelog.md` to be updated for all repository changes.
- Added a project rule requiring relevant files to be read and understood before proposing or making code edits.
- Affected files: `AGENTS.md`, `docs/changelog.md`.
- Migrations: none.
