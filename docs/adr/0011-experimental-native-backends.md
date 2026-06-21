# ADR 0011 - Experimental native backends are opt-in acceleration paths

Status: Accepted

## Context

The Python SOP exporter is complete but slow on large animated geometry. The
TouchDesigner C++ SOP input API can read point custom attributes and standard SOP
channels such as `N`, `Cd`, and texture coordinates, but does not expose generic
vertex/primitive custom attributes. The C++ POP input API does expose point,
vertex, and primitive attribute classes.

## Decision

Keep `Compatible SOP Python` as the default public contract. Add native modes only
as explicit experimental choices:

- `Experimental Native SOP` uses a CPlusPlus SOP writer for safe SOP inputs. It
  must fail before export when generic vertex/primitive custom attributes are
  present rather than dropping them.
- `Experimental Native POP` uses a CPlusPlus POP writer to stream binary chunks for
  the existing out-of-process USD sidecar builder.
- Native plugins are optional. Missing DLLs or unsupported platforms must not break
  the default SOP Python exporter.
- Native plugin paths are authored as `project.folder` expressions inside the
  component, not as absolute developer-machine paths.

## Consequences

- Public users get a reliable default path and clear status for native acceleration.
- Native work can evolve without changing the SOP-to-USD product promise.
- Documentation and validation must describe backend compatibility, not just output
  file format support.
- Production SOP networks that require generic vertex/primitive custom attributes
  should use `Compatible SOP Python` or convert through the explicit POP native
  path.
