# TD-SOP-USD-Anim-Bridge

## Project Idea

This project is about saving and exporting animation from TouchDesigner into USD files that support frame sequences and animated geometry.

The main focus is direct export from the SOP / Surface Operators context into USD (`.usd`, `.usda`, `.usdc`) so the resulting files can be imported into Houdini and other 3D tools.

## Motivation

The current workflow is too indirect:

1. TouchDesigner geometry has to be saved frame by frame.
2. A temporary format or workaround such as `bhclassic` is often used.
3. Houdini is then used to combine those frames into one animated `.usd` file.
4. Only after that can the file be imported cleanly into another DCC.

The goal is to remove this intermediate step and build a more direct bridge from TouchDesigner SOP animation to standard animated USD files.

## Agent Context

- This is not just a static geometry exporter.
- Account for frame sequences, topology changes, vertex attributes, point attributes, normals, UVs, colors, and transform/geometry animation.
- USD is the only target format. Keep the project positioned and implemented as a SOP-to-USD exporter.
- Houdini and downstream DCC import workflows are important validation targets.
- TouchDesigner SOP context is the primary data source.
- Solutions should fit real production workflows, not only demo proof-of-concepts.

## TouchDesigner Access and Native USD Limits

The agent interacts with TouchDesigner through the `twozero_td` MCP server. Always work with the live TD instance through MCP tools (read operators, run Python, inspect geometry, capture screenshots) instead of guessing project state.

- Call `td_list_instances` at the start of work to discover the running instance and its `instanceId`; use that `instanceId` as `target_instance` for all other MCP calls.
- TouchDesigner has no native USD writer. USD is read-only (the USD COMP imports; TD does not write USD).
- Therefore the exporter must write USD itself. TD is the data source and orchestrator; file writing happens outside TD's native operators.

## Save the TouchDesigner Project Around Risky Changes

TouchDesigner work lives in the in-memory session until the `.toe` is saved. Save through Python so progress is not lost.

- Before any important or dangerous change to the live project, save first: `project.save()` (saves to the current `.toe`; pass a path to save elsewhere).
- After completing such a change, save again so the new state is persisted to disk.
- `project.save()` returns `True` when a file was written, `False` otherwise; check it when the save matters.

## Module Structure and Encapsulation

The exporter lives entirely inside a single Base COMP, `TD_SOP_USD_Anim_Bridge`. Treat this COMP as the module boundary; do not depend on or reach into anything outside it.

- Input geometry arrives through the container's input connector and is read from the `IN_FOR_EXPORT` (In SOP) inside it. The SOP wired from outside is not the module's concern.
- All logic lives in a Python extension on the container COMP (a DAT inside, referenced by `par.extension1`), not in scattered DATs. Promote the public methods so the module is driven by calling them on the COMP.
- All UI is exposed as custom parameters on the container COMP. Do not build panel UIs unless asked.
- Use relative references inside the module (`op('IN_FOR_EXPORT')`, `./child`, `me.parent()`); never hardcode `/project1/...` so the COMP stays copy- and `.tox`-export safe.

## Current Architecture and Status

The exporter is implemented as the `ExportExt` Python extension on the
`TD_SOP_USD_Anim_Bridge` Base COMP (persisted in `TD-SOP-USD-Anim-Bridge.toe` and shipped
as `TD_SOP_USD_Anim_Bridge.tox`). The canonical reviewable source is
`src/ExportExt.py`, synced to the `ExportExt` DAT. It exports animated SOP geometry
to a single USD file: `.usda` directly, animated `.usdc` via binary chunk files plus
a sidecar crate builder, and static `.usdc` by sidecar transcoding a temporary
`.usda`. Key decisions are recorded in `docs/adr/` (see
`docs/adr/README.md`); the change log is `docs/changelog.md`. This section is the
fast orientation for a new agent.

**Data flow**
- Input: `IN_FOR_EXPORT` (In SOP) inside the COMP, fed from outside via the COMP
  input connector.
- Optional native input: `IN_POP_FOR_EXPORT` (In POP) is connector 1 for
  `Experimental Native POP`. `Compatible SOP Python` remains the default public
  contract; native modes are opt-in acceleration paths (see ADR 0011).
- `_sampleSop(sop)` → intermediate representation (IR) dict for one frame:
  `numPoints/numPrims/numVertices`, `P` (xyz list), and `pointAttribs` /
  `vertexAttribs` / `primAttribs` as `{name: {size, values}}`. For meshes it also
  builds `faceVertexCounts` / `faceVertexIndices` via `_faces` → `_meshFaces` →
  `_dedupeRing` (Poly passes through; TD `Mesh` prims are tessellated grid→quads,
  poles→triangles; prim attribs replicated per output face).
- Geometry kind from data (`_isMeshGeo`): no prims or particle/point prims
  (class `Prim`/`Part`/`Particle`) → `Points`; `Poly`/`Mesh` prims → `Mesh`. Stored
  as `ir['isMesh']`. So a Particle SOP exports as a `UsdGeomPoints` cloud.
- `Export()` branches on the `Animate` toggle:
  - **Static** (off): `_build(ir)` writes one frame in memory (values inline).
  - **Animated** (on): `_startPlaybackExport(...)` starts a playback-driven state
    machine and returns immediately. It saves timeline state, sets
    `project.realTime = False`, disables looping, aligns TD's visible working range
    (`rangeStart`/`rangeEnd`) to `[Frame Start, Frame End]`, enables `exec_playback`,
    and plays from `Playback Start`. `exec_playback.onFrameEnd(frame)` calls
    `PlaybackFrame`. Playback still cooks every TD frame, but `Frame Step` controls
    which source frames are written. Written source frames are mapped to dense output
    timeCodes starting at `Frame Start`, and `Output FPS` is authored as
    `framesPerSecond`/`timeCodesPerSecond`. Each written frame appends a
    `<frame>: <array>,` line to per-attribute temp files (`_buildSections`) for
    `.usda`, or raw numeric chunk files plus a manifest (`_buildBinarySections`) for
    animated `.usdc`. On completion `_finishPlaybackExport` assembles the final
    `.usda` or runs the chunk builder for `.usdc`, cleans temps, disables the Execute
    DAT, and restores the previous timeline state including the visible working
    range. Peak TD memory is one frame (see ADR 0004, ADR 0009, and ADR 0010).
    `topoVaries` comes from the user `Topology Changes` toggle; `_topoKey` is a
    cheap safety net if the user mis-declares constant.

**Attribute mapping** (`_resolveAttr`): class → interpolation is point→`vertex`,
vertex→`faceVarying`, prim→`uniform`. Role mappings: point `N`(3)→`normal3f normals`;
point `v`/`PartVel`(3)→`vector3f velocities`; vertex `uv`(≥2)→`texCoord2f primvars:st`
(components u,v); `Cd`(3)→`color3f primvars:Cd`; point `id`/`ids`→`int64[] ids`
(Points only); everything else → `primvars:<name>` typed by component size. The
`Half Precision` mode is applied after role resolution: `Off` keeps float32,
`Safe Half` halves only normals, `st`, velocities, and `Cd`, and `All Half` also
halves positions, widths, and generic float primvars. Integer arrays are never half.

**Stage**: `defaultPrim = "Exported"`, `metersPerUnit = 1`, `upAxis = "Y"`; animated
adds `framesPerSecond`/`timeCodesPerSecond` (from `Output FPS`) and
`startTimeCode`/`endTimeCode`. Meshes set `subdivisionScheme = "none"`. All exports
author `float3[] extent` from the sampled point bbox. Animated binary `.usdc` export
writes numeric chunk files and a manifest, then runs `tools/build_usdc_from_chunks.py`
with an interpreter resolved from the `USD Python Executable` parameter,
`TD_SOP_USD_ANIM_BRIDGE_PYTHON`, or `tools/.venv-usd`. Static `.usdc` still writes a
temporary `.usda` and transcodes it with `tools/transcode_usd.py`. Sidecar USD
authoring is out-of-process and can grow in RAM with cache size.

**UI** (custom pars on the COMP): `File` (output path, relative→anchored to
`project.folder`; extension forced by `Format`), `Format` (`usda`/`usdc`),
`Temp Folder` (relative path defaults to `_tdsopusd_temp`, used for setup logs,
animated export chunks, and temporary `.usda` files),
`USD Python Executable` (optional path to a Python executable with `usd-core` and
`numpy` installed), `Setup Binary Support` (pulse
that runs `tools/setup.py` with TD's bundled Python), `Binary Status`, `Half
Precision` (`off`/`safe`/`all`), `Animate`, `Output FPS`, `Frame Step`,
`Frame Start`, `Frame End`, `Topology Changes`, `Playback Start`, `Cancel`,
`Progress`, `Export Status`, `Export`
(pulses are dispatched by the `parexec_export` DAT). `RefreshUiState()` keeps
animated-only parameters disabled when `Animate` is off, enables `Cancel` only for an
active playback export, and locks `Export`/`Animate` while such an export is running.

**Validation**: `tools/validate_usd.py` run with `tools/.venv-usd` (usd-core) checks
per-time element-count coherence for `.usda` and `.usdc`, including points, normals,
velocities, extent, topology arrays, ids, and primvars. usd-core and numpy are also
used by the `.usdc` sidecar builder/transcode; they are never imported in TD.

**Distribution**: `.usda` export works from the `.tox` with no Python package setup.
`.usdc` export and validation require `tools/setup.py` to create `tools/.venv-usd`
from `tools/requirements.txt` (`usd-core` and `numpy` pinned). Keep
`src/ExportExt.py`, the DAT, the `.toe`, and `TD_SOP_USD_Anim_Bridge.tox` in sync
when exporter code changes.

Optional native plugins live under `native/` and are built with `native/build.ps1`;
generated DLLs are local/release artifacts, not normal source history.

**Known limitations**: Changing-topology animation is authored as time-sampled USD
topology and verified with usd-core/usdview; per-DCC import behavior is not verified
(some USD importers read topology only once). NURBS/Bezier need a Convert SOP.
`.usdc` sidecar authoring may be memory-heavy for very large caches.
Winding/orientation (`rightHanded` default) is not yet DCC-verified. Multi-prim
output remains a backlog item.

## Possible Directions

- Investigate TouchDesigner Python API support for reading SOP geometry per frame.
- Build an exporter that samples a SOP across a frame range and writes one animated file.
- Write USD directly: `.usda` (ASCII) for readable/debuggable output, and `.usdc` (crate) via `usd-core` for production-size caches.
- Treat a Houdini bridge as a fallback, not the main goal.
- Document limitations: changing topology, large caches, attributes, FPS, frame range, and scale/orientation differences between TD, Houdini, and downstream DCCs.

## Language

Documentation and external project communication are English-only.

- Keep repository docs, changelog entries, issues, release notes, public comments, and user-facing project text in English.
- Translate existing non-English project documentation when touching it.
- Internal reasoning can happen silently in any language, but the output committed to the project should be English.

## Changelog - Document All Changes

ALWAYS update `docs/changelog.md` when making changes.

- Bug fixes, new features, refactors, documentation updates, project rule changes, and other repository changes all go in the changelog.
- Format: `## [YYYY-MM-DD] Brief Title` followed by bullet points.
- Include affected files and migrations. If there are no migrations, write `Migrations: none`.

## Investigate Before Answering

ALWAYS read and understand relevant files before proposing or making code edits.

- If the user references a specific file or path, MUST open and inspect it before explaining it or proposing fixes.
- Be rigorous in searching the codebase for key facts before making claims about behavior.
- Thoroughly review the style, conventions, and existing abstractions of the codebase before implementing new features.
- Never speculate about code, project behavior, or implementation details that have not been inspected.
- If the relevant files are not obvious, search the repository first and inspect the files that define the behavior being changed.
- State assumptions explicitly when local evidence is incomplete.

## Mandatory Local Documentation

Use local documentation before guessing TouchDesigner or OpenUSD APIs.

- TouchDesigner docs may be available in `Docs_TD/` or `Docs_MD/` with roughly 1950 Markdown files. Operators use names like `Noise_CHOP.md`, `Blur_TOP.md`, and `Text_DAT.md`; Python classes use names like `Par_Class.md` and `OP_Class.md`; concepts use names like `Extensions.md`, `Custom_Parameters.md`, `Python.md`, and `GLSL.md`.
- OpenUSD docs/source may be available in `Docs_USD/OpenUSD/`. Use it for USD concepts, schemas, file format behavior, time-sampled data, `UsdGeom`, `Sdf`, and Python/C++ API details before making USD claims.
- These folders are local reference dumps and should not be committed unless explicitly requested.

## TouchDesigner Scripts

Follow TouchDesigner DAT script conventions.

- Do not use `if __name__ == '__main__'` or `if hasattr(op, 'create')`.
- Call the target function directly at the end of the script.
- Use tabs for indentation in TouchDesigner DAT scripts.
- `op()` is a global function; avoid shadowing it in loops.
- PLAYER_COPY_MANAGER copy-safety principle: anything inside `PLAYER_1` that will be copied to `PLAYER_2..4` must not hardcode `/project1/PLAYER_1/...`. Use relative references such as `../slot_swipes_left`, `../slot_swipes_right`, `./child`, or expressions relative to the copied COMP. After any copy-manager propagation, verify raw parameter values (`par.val`) and resolved targets for `PLAYER_1..4`; resolved paths alone can hide bad absolute references.

## Challenge Before Agreeing

When the user proposes a change in strategy, positioning, architecture, or UX, DO NOT agree immediately.

1. Defend the current solution first. Recall why it was chosen, what problem it solved, and what is lost by abandoning it.
2. Attack the proposal. Identify weak points, risks, and non-obvious consequences.
3. Only then give a position: which side is stronger and why. `I do not know, need data` is a valid answer.

If you catch yourself simply repackaging the user's words into an argument, say so directly.

## No Hardcoding - 100% Data-Driven

For model/provider-specific behavior, prefer data-driven configuration over hardcoded branches.

- NEVER encode model or provider behavior with checks like `if model.startswith('midjourney')` or `if provider == 'kie_ai'`.
- ALWAYS represent model/provider capabilities and parsing rules in YAML config, using fields such as `response_parser`, `requires_image`, and `asset_type`.
- Test every model/provider decision with this question: `Will this break when we add 100 new models?`
- If the answer is yes, refactor the behavior into YAML-driven configuration.

## Brevity - Hard Requirement

The user's attention context window is narrow. Long walls of text are pain, not service.

- Default response length: 3-7 lines.
- Use long breakdowns only when the user explicitly asks for detailed analysis, for example `ULTRATHINK`, `detailed`, or `break it down`.
- Do not use tables or numbered lists for routine communication. Add structure only when comparing options or when it materially improves clarity.
- Do not use preambles like `I will investigate`, `let's think`, or `so`. Go straight to the point.
- Do not add final summaries just to restate visible diffs.
- When asking for a decision, ask one question. If there is a recommendation, state it in one line and wait for ack/nack.
- If the answer is getting long, reduce it to the core thesis. Length is not depth.
