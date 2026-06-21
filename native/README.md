# Native Plugins

Native plugins are optional acceleration paths. The default public exporter is
still `Compatible SOP Python` and does not require anything in this folder.

## Build

On Windows x64 with Visual Studio 2022 Build Tools and TouchDesigner installed:

```powershell
powershell -ExecutionPolicy Bypass -File native/build.ps1
```

The build writes local artifacts under `native/**/build/`, which are ignored by
git. Prebuilt DLLs should be attached to releases for a specific TouchDesigner
build instead of committed to source history.

The TouchDesigner component assigns plugin paths with `project.folder`
expressions at runtime. Do not save absolute local DLL paths into the component.

## Contents

- `td_sop_usd_writer/` - experimental CPlusPlus SOP writer used by
  `Experimental Native SOP` for safe SOP inputs. It preserves point custom
  attributes plus standard SOP `N`, `Cd`, and texture coordinates, and rejects
  unsupported generic vertex/primitive custom attributes before export.
- `td_pop_usd_writer/` - experimental CPlusPlus POP writer used by
  `Experimental Native POP` for point, vertex, and primitive float attributes.
- `td_sop_probe/` - diagnostic probe that documents the public C++ SOP input API
  limitation: point custom attributes are visible, generic vertex/primitive
  custom attributes are not.
- `td_pop_probe/` - diagnostic POP attribute access benchmark.
