# ADR 0009 - Playback-driven animated export

Status: Accepted

## Context

The previous animated driver scrubbed the timeline in a blocking loop:
`time.frame = f`, then `cook(force=True)` on the input SOP. That is acceptable only
for geometry that is a pure function of the current frame. It is wrong for stateful
TouchDesigner networks such as Particle SOPs, Bullet/spring simulations, feedback
loops, CHOP exports, and expression networks that depend on sequential cooks.

The exporter still needs the memory profile from ADR 0004: one sampled frame in RAM,
with per-attribute temp files streamed into a single final USD file.

## Decision

Animated `Export()` starts a playback state machine instead of scrubbing frames:

- It saves timeline state (`realTime`, play state, frame, loop, and main start/end
  where available), sets `project.realTime = False`, disables looping, aligns TD's
  visible working range (`rangeStart`/`rangeEnd`) to the write window, enables an
  `exec_playback` Execute DAT, and starts playback from `Playback Start`.
- `exec_playback.onFrameEnd(frame)` calls `PlaybackFrame(frame)`. Pre-roll frames only
  advance TD state. Playback still advances through every TD frame in order, but
  `Frame Step` decides which source frames inside `[Frame Start, Frame End]` are
  written. Written source frames are mapped to dense output timeCodes beginning at
  `Frame Start`, so a 60 FPS simulation can be exported as a 30 FPS cache with
  `Output FPS = 30` and `Frame Step = 2` without stretching time.
- `Output FPS` is authored into `framesPerSecond` and `timeCodesPerSecond`. It has a
  default expression that follows the current TD timeline rate, but may be overridden
  by the user.
- Written frames sample `IN_FOR_EXPORT` and append one frame of data to the existing
  ADR 0004 section temp files.
- The first written frame creates the section plan and schema/topology guards. Later
  frames reuse the same guards and `_streamFrameStep`.
- Finalization closes temp files, then either assembles the `.usda` or builds
  animated `.usdc` from binary chunks, deletes temps, disables the Execute DAT, and
  restores the saved timeline state including the visible working range. Cancel and
  failure follow the same cleanup path.
- Static export remains synchronous and unchanged.

New UI parameters are `Playback Start`, `Output FPS`, `Frame Step`, `Cancel`,
`Progress`, and `Export Status`.
`Export()` for animated exports now returns when the run has started; completion is
reported through `Export Status` and the textport.

## Consequences

- Stateful SOP networks export according to TouchDesigner's real sequential cook
  order, provided `Playback Start` includes the needed simulation reset/pre-roll.
- The UI stays responsive during animated export, and cancel/progress are natural.
- Animated export duration now depends on real playback through every frame; this is
  the cost of correctness.
- The one-frame RAM ceiling from ADR 0004 remains intact. Animated `.usdc` avoids
  ASCII formatting in the playback callback; final crate authoring remains a
  sidecar step that can grow with cache size.
