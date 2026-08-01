# SKRTG Engine

SKRTG is an offline skeletal retargeting engine and native review toolchain.
The current public source route consumes configuration exported from Unreal
Engine as JSON; it never reads `.uasset` files.

This repository intentionally contains source code only. Character files,
animation clips, Unreal projects, exported rig definitions, Golden pose data,
review packages, and compiled releases are not included.

## Components

- `skrtg_ueik_retarget_worker` — hash-bound FBX retargeting worker driven by
  exported IK Rig and IK Retargeter JSON.
- `skrtg_ueik_route_probe` — validates a source/target JSON route without
  running a full clip.
- `skrtg_viewer` — native GLFW/OpenGL/ImGui four-lane review Viewer.
- `skrtg_retarget_bridge` — serial process bridge between the Worker and the
  read-only SKRV review boundary.
- `skrtg_batch_retarget` — low-memory batch runner with one active Worker.
- `skrv_pack` and `skrv_inspect` — SKRV v1 packaging and inspection tools.

## Input contract

The production-facing route is explicit and fail-closed:

1. Export IK Rig, IK Retargeter, rest-pose, and animation Golden data from
   Unreal Engine to JSON.
2. Provide source-rest, source-animation, and target-rest FBX files.
3. Bind every input by SHA-256 in an external asset catalog.
4. Let the Worker validate coordinate conversion, hierarchy, rest pose,
   animation keys, and route compatibility before it commits output.
5. Review the generated SKRV package in the native Viewer.

No skeleton-name inference, direct Unreal asset parsing, or silent fallback
route is enabled.

## Build

Requirements:

- CMake 3.23 or newer
- A C++20-capable compiler (Visual Studio 2022 is the tested Windows toolchain)
- Autodesk FBX SDK 2020.3.9
- OpenGL 3.3-capable graphics driver

Example:

```powershell
cmake -S . -B build -A x64 `
  -DSKRTG_FBX_SDK_ROOT="C:/Program Files/Autodesk/FBX/FBX SDK/2020.3.9"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The Autodesk SDK and its license are not redistributed by this repository.

## External catalog

The Viewer builds and starts without any private asset data. To create a local
package with your own catalog, pass a directory outside the repository:

```powershell
cmake -S . -B build -A x64 `
  -DSKRTG_FBX_SDK_ROOT="C:/Program Files/Autodesk/FBX/FBX SDK/2020.3.9" `
  -DSKRTG_RETARGET_ASSET_CATALOG_DIR="D:/private/skrtg_catalog"
```

That directory must contain `retarget_asset_catalog.json`. CMake copies it
only into the local build output; it is ignored by Git.

## Current boundary

- Windows x64 is the validated product platform.
- UE configuration is JSON-export based, not direct engine-asset access.
- The exact animation path is currently tied to the UE 5.8 import contract and
  checks every exported key against Golden data.
- The Viewer consumes SKRV and does not recompute retargeting.
- Batch execution is serial by design.
- Material, texture, morph-target, portable runtime, and cross-platform
  release gates remain open work.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the runtime flow and
[docs/ASSET_POLICY.md](docs/ASSET_POLICY.md) for repository boundaries.
