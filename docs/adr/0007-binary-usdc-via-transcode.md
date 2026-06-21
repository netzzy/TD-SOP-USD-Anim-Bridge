# ADR 0007 - Static binary `.usdc` via sidecar transcode

Status: Accepted

## Context

The exporter writes `.usda` directly from TouchDesigner using the Python standard
library. That keeps the TD process free of `pxr` bindings and lets animated export
stay memory-bounded by streaming per-frame samples through temp files.

Production caches also need `.usdc` for smaller files and faster downstream loads.
TouchDesigner cannot write USD crate files, and `usd-core` should not be imported
inside the TD process because of runtime/DLL conflict risk.

## Decision

Add a `Format` custom parameter with `usda` and `usdc` choices. For static export,
the existing writer produces ASCII USD first:

- `usda`: write the existing `.usda` output directly.
- static `usdc`: write the same `.usda` to a temporary path, then run
  `tools/transcode_usd.py` in `tools/.venv-usd` to export the layer to crate via
  `Sdf.Layer.FindOrOpen(input).Export(output)`.

The final file extension is forced from `Format`, so `export/sop_usd_export.usda` with
`Format = usdc` writes `export/sop_usd_export.usdc`.

## Consequences

- The static `.usdc` path still reuses the existing `.usda` writer.
- TouchDesigner still never imports `pxr`; all usd-core work is out-of-process.
- Static `.usdc` export is not memory-bounded. The sidecar materializes the full
  layer in usd-core before writing crate, so peak sidecar RAM is O(total exported USD).
- The temporary `.usda` is deleted after transcode.
- `tools/.venv-usd` is now required for `.usdc` export, not only validation.
- ADR 0010 replaces this path for animated `.usdc` with binary chunk files and a
  direct crate-building sidecar.
