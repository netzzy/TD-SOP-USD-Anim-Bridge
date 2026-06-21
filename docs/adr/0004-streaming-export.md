# ADR 0004 — Streaming, memory-bounded export to a single `.usda`

Status: Accepted (supersedes the original in-memory animated writer)

## Context

The first animated writer sampled the **whole** frame range into a Python list of
per-frame IR dicts, then built the **whole** `.usda` as one string before writing.
Peak RAM was O(total animation) — heavy Python tuples per point per frame, plus a
duplicate giant string — capping exportable size by available RAM. Unacceptable.

A `.usda` time-sampled attribute is written attribute-major
(`points.timeSamples = { 1: ..., 2: ... }`), so a single self-contained ASCII file
cannot be produced in pure frame-major streaming in one shot.

Two ways to break the RAM ceiling were considered:

- **Value clips** (per-frame files + a stitch file): truly unbounded, but a
  multi-file asset. Rejected — the user requires a single normal USD file.
- **Per-attribute temp files** (chosen).

## Decision

Stream with a **one-frame memory ceiling**, producing a single `.usda`:

1. One sequential playback pass (see [0009](0009-playback-driven-animated-export.md)).
   For each frame in the write window, sample one IR and append that frame's
   `<frame>: <array>,` line to a **per-attribute temp file**; discard the IR.
2. Assemble the final `.usda` by streaming each temp file into its attribute's
   `.timeSamples { }` block (`shutil.copyfileobj`, chunked).

Whether topology changes is **declared by the user** (`Topology Changes` toggle),
not detected by pre-scanning (streaming cannot pre-scan cheaply). A constant
declaration writes topology once; a varying declaration time-samples it. A cheap
in-pass safety net (`_topoKey`) raises if the user declared constant but topology
actually changes.

## Consequences

- Exportable size is bounded by **disk, not RAM**; peak RAM ≈ one frame.
- Output stays a single self-contained `.usda` (no value clips).
- Cost: transient per-attribute temp files on disk (cleaned up via `try/finally`).
- Animated `.usdc` uses binary chunk files and a direct crate-building sidecar (see
  [0010](0010-animated-usdc-binary-chunks.md)). The TD-side writer stays
  memory-bounded; final crate authoring still happens out-of-process.
- Static (single-frame) export keeps the simple in-memory `_build`.
- ADR 0009 replaces the original scrub/force-cook driver. This ADR still defines the
  memory-bounded section/temp-file layout used by the playback callback.
