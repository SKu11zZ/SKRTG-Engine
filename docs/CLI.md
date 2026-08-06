# SKRTG unified CLI

`skrtg` is the stable entry point for people, scripts, CI jobs, and AI agents.
The older single-purpose executables remain available as compatibility tools,
but new automation should begin with this command.

Run `skrtg --help`, `skrtg <group> help`, or
`skrtg <group> <command> --help` for the relevant command surface.

```text
skrtg
├── version
├── capabilities
├── doctor
├── profile
│   ├── adapters
│   ├── probe
│   ├── normalize
│   ├── create
│   ├── inspect
│   ├── install
│   └── list
├── batch
│   ├── validate
│   └── run
├── bridge
│   ├── validate
│   └── run
├── skrv inspect
└── debug
    ├── profile
    ├── batch
    └── bridge
```

## Machine contract

Add `--json` anywhere in a command to make stdout contain exactly one JSON
object. Success uses `skrtg.cli.result.v1`; failure uses
`skrtg.cli.error.v1`. Warnings and ordinary diagnostics do not contaminate
machine-readable stdout.

| Exit code | Meaning |
| ---: | --- |
| `0` | command completed |
| `1` | validation or execution failed |
| `2` | command usage or request schema is invalid |
| `3` | a required runtime dependency is unavailable |

The schemas live in [`schemas/`](../schemas). Agents should check `ok` and
`error.code`; they should not parse English prose from stderr.

```powershell
skrtg capabilities --json
skrtg doctor --json
```

## Character Profile workflow

Start by probing the input. Probe is read-only and succeeds for a valid draft
even when it is not ready to package.

```powershell
skrtg profile probe .\character.fbx --rest-pose t_pose --json
skrtg profile probe .\IK_Character.json --rest-pose a_pose --json
skrtg profile probe .\character.xml --json
```

`runtimeDefinitionComplete=false` is not hidden. The response contains the
exact `missingRequirements` list. A rest-pose FBX normally reports missing
root, pelvis, chain, and rig identity semantics; geometry alone cannot define
a safe retarget mapping.

Normalize a supported source to the neutral Character Definition JSON:

```powershell
skrtg profile normalize .\character.xml `
  --out .\character.normalized.json `
  --json
```

Create a package either with direct options or a request file. Request files
are preferable for CI and agents because relative paths resolve against the
request file, not the caller's current directory.

```powershell
skrtg profile create --request .\profile-create.json --json
```

See [`examples/profile-create.example.json`](../examples/profile-create.example.json)
and [Character Profile authoring](CHARACTER_PROFILE_AUTHORING_V2.md).

## Retarget preflight and execution

Validation never writes retarget output:

```powershell
skrtg batch validate --request .\batch-request.json --json
skrtg bridge validate --request .\bridge-request.json --json
```

Run only after preflight passes:

```powershell
skrtg batch run --request .\batch-request.json --json
skrtg bridge run --request .\bridge-request.json --json
```

Batch remains intentionally serial (`maximumConcurrentJobs=1`) until a later
memory and determinism gate explicitly changes that contract.

An optional Operation System v2 program can be selected in the native Viewer
or supplied through the request's `operationStack` object. Configured
profile-backed jobs use Bridge request v6/status v7 and Batch request/status
v4; jobs without it retain v5/v6-status and v3 compatibility contracts. The
config is SHA-256-bound during preflight and remains candidate-only:

```json
"operationStack": {
  "configJson": "./op-stack.json",
  "expectedSha256": "",
  "candidate": true,
  "selected": false,
  "adopted": false
}
```

See [Operation System v2](OPERATION_SYSTEM_V2.md), the
[example program](../examples/op-stack-v2.example.json), and these schemas:

- [`skrtg.op_stack.v2`](../schemas/skrtg.op_stack.v2.schema.json)
- [`retarget_bridge_request.v6`](../schemas/skrtg.native_viewer.retarget_bridge_request.v6.schema.json)
- [`retarget_bridge_status.v7`](../schemas/skrtg.native_viewer.retarget_bridge_status.v7.schema.json)
- [`batch_retarget_request.v4`](../schemas/skrtg.native_viewer.batch_retarget_request.v4.schema.json)
- [`batch_retarget_status.v4`](../schemas/skrtg.native_viewer.batch_retarget_status.v4.schema.json)

Profile-backed runs without an Operation config write batch status v3 and
Bridge status v6; configured runs write v4 and v7. All expose
measured phase timings, so an agent can distinguish preflight, Worker,
adapter, SKRV preparation/inspection, and final verified-export copy time
without scraping logs. The corresponding schemas are:

- [`skrtg.native_viewer.batch_retarget_status.v3`](../schemas/skrtg.native_viewer.batch_retarget_status.v3.schema.json)
- [`skrtg.native_viewer.retarget_bridge_status.v6`](../schemas/skrtg.native_viewer.retarget_bridge_status.v6.schema.json)

Older profile-backed batch status v2 remains readable. Timing values are
elapsed wall-clock observations for the completed process phases; they do not
claim per-operation solver profiling.

## Agent diagnostics

`debug` returns the full resolved preflight evidence rather than a short
human summary:

```powershell
skrtg debug profile .\character.skrtgprofile --json
skrtg debug batch --request .\batch-request.json --json
skrtg debug bridge --request .\bridge-request.json --json
```

Use `--trace <file.jsonl>` to append start/finish events. The trace contains
arguments, command identity, exit code, duration, and no fabricated solver
telemetry.

```powershell
skrtg --json --trace .\run.trace.jsonl `
  bridge validate --request .\bridge-request.json
```

Do not put credentials in command-line arguments: an explicitly requested
trace records those arguments verbatim.

## Compatibility executables

`skrtgprofile_pack`, `skrtgprofile_inspect`, `skrtgprofile_install`,
`skrtg_batch_retarget`, `skrtg_retarget_bridge`, `skrv_pack`, and
`skrv_inspect` remain build and package targets. They are useful for old
scripts and component isolation; the unified CLI calls the underlying C++
APIs directly rather than shelling out to them.
