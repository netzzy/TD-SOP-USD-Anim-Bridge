# ADR 0010 - Animated `.usdc` via binary chunk sidecar

Status: Accepted

## Context

Animated `.usdc` previously reused the streaming `.usda` writer, then transcoded
the generated ASCII layer. That kept TouchDesigner free of `pxr`, but it made the
playback callback spend most of its time formatting very large Python strings.
Large frame ranges could drop TouchDesigner to single-digit FPS before the sidecar
transcode even started.

## Decision

Keep `.usda` export unchanged. For animated `Format = usdc`, the playback callback
writes raw numeric arrays into per-section `.bin` chunk files and records offsets in
a JSON manifest. After playback, `tools/build_usdc_from_chunks.py` runs out of
process with `usd-core` and `numpy`, reads those chunks, and authors the final crate
layer directly.

Static `.usdc` keeps the older temporary `.usda` plus `tools/transcode_usd.py` path.

## Consequences

- TouchDesigner still never imports `pxr` or `numpy`.
- Animated `.usdc` avoids ASCII array formatting and USD ASCII parsing in the hot
  playback path.
- The sidecar Python environment now needs both `usd-core` and `numpy`.
- The TD-side memory ceiling remains one sampled frame plus binary file buffers.
- The sidecar still owns final USD authoring, so sidecar RAM can grow with the size
  of the authored cache.
