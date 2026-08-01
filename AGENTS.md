# SKRTG agent working contract

This repository is source, tests, schemas, and documentation. Keep character
FBX, animation FBX, Unreal projects/assets, exported production definitions,
Golden data, `.skrtgprofile`, SKRV output, binaries, and release archives out
of Git.

## Start here

1. Read `README.md`, `docs/PROJECT_LAYOUT.md`, and the contract document for
   the subsystem being changed.
2. Use `skrtg capabilities --json` to discover the installed command surface.
3. Use `skrtg doctor --json` before a runtime job.
4. Validate before run: `profile probe`, `batch validate`, or
   `bridge validate`.
5. Treat non-zero exit codes and `ok=false` as failures. Use `error.code`,
   `error.stage`, and `error.context`; do not scrape prose from stderr.

## Non-negotiable behavior

- Never parse `.uasset` at runtime. Unreal configuration enters through an
  exported JSON adapter.
- Never infer a runnable skeleton mapping from bone names alone.
- Unknown definition formats, coordinate contracts, hashes, roles, or paths
  fail closed.
- A Rest FBX is a skeleton/rest-pose source, not a complete retarget profile.
- No partial Profile or retarget output is committed after validation failure.
- Viewer code consumes SKRV; it does not own or recompute the retarget solver.
- An implementation or passing experiment does not automatically select or
  adopt an algorithm route.
- Preserve unrelated work and do not use destructive Git cleanup.

## Build and test

Use an out-of-source build and provide Autodesk FBX SDK 2020.3.9 through
`SKRTG_FBX_SDK_ROOT` or the CMake cache variable of the same name.

```powershell
cmake -S . -B build -A x64 `
  -DSKRTG_FBX_SDK_ROOT="C:/Program Files/Autodesk/FBX/FBX SDK/2020.3.9"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Every change to a machine-facing JSON shape needs a versioned file under
`schemas/`, a contract test, and updated documentation. Keep old command-line
executables working unless a deliberate compatibility break is approved.

## Adding a Character Definition importer

An importer parses one declared source format and emits
`skrtg.character_definition.v1`. It must:

- identify itself with a stable id and version;
- bind the input SHA-256;
- enforce file size, nesting, node, and inventory limits;
- normalize coordinates explicitly or reject them;
- report missing runtime requirements without fabricating them;
- reuse the shared semantic validator and package writer;
- add positive, negative, and no-output-on-failure tests.

See `docs/CHARACTER_PROFILE_AUTHORING_V2.md`.

## Debug evidence

Use `skrtg debug ... --json` for resolved Profile, Batch, and Bridge evidence.
Use `--trace <events.jsonl>` only when a durable start/finish timeline is
needed. Traces record arguments, so do not put credentials in command lines.
