# Profile-backed batch v3

## Purpose

Profile-backed batch v3 extends the verified single-job profile route to a
catalog selection without weakening its provenance. The native Viewer chooses:

1. one installed source `.skrtgprofile`;
2. one installed target `.skrtgprofile`;
3. one or more enabled catalog animations compatible with the source;
4. a new or empty output directory.

The Viewer does not ask for loose rest FBX, IK JSON, or an arbitrary animation
folder on this route.

## Request contract

The request schema is:

```text
skrtg.native_viewer.batch_retarget_request.v3
```

`assetSelection` binds:

- catalog path, ID, and SHA-256;
- source and target profile IDs;
- source and target package paths, versions, and SHA-256 values;
- all extracted source/target rest, IK Rig, and alignment JSON hashes.

Every `animations` entry independently binds:

- stable animation ID and label;
- source profile ID;
- animation FBX path and SHA-256;
- Golden animation JSON path and SHA-256;
- animation Stack;
- source-animation and rest-FBX import modes.

The profile-backed route requires an empty `animationDirectory` and
`recursive=false`. It rejects duplicate animation IDs, duplicate final-output
paths, clips owned by another source profile, changed packages, changed
resources, changed catalogs, changed FBX/Golden files, unsupported import
modes, and incompatible skeleton signatures.

## Planning and execution

Before creating the output directory, the batch planner:

1. verifies both profile packages and their installed resources;
2. verifies the catalog identity and hash;
3. verifies every selected animation and its Golden data;
4. builds the exact Bridge v5 request for every selected clip;
5. runs Bridge preflight for the complete job list.

If any job fails preflight, no batch output directory is committed. A
successful plan is then executed with the fixed policy:

```json
{
  "maximumConcurrentJobs": 1,
  "mode": "streaming_one_animation_per_worker",
  "continueAfterJobFailure": true
}
```

One Worker process exits before the next begins. A runtime failure is recorded
per job; remaining preflighted jobs may continue. Existing non-empty output
directories are never overwritten.

Stable outputs use the catalog animation ID:

```text
FinalFBX/<animationId>__SKRTG_Final.fbx
Jobs/<index>_<animationId>/review.skrv
batch_status.json
```

## Status contract

Profile-backed status uses:

```text
skrtg.native_viewer.batch_retarget_status.v2
```

It records top-level package/catalog provenance and per-job animation/Golden
provenance, hashes the final FBX, reports the serial execution policy, and
keeps:

```json
{
  "candidateRouteSelected": false,
  "candidateRouteAdopted": false
}
```

The reader rejects a profile-backed status that claims either value is true.
It also rejects missing per-job provenance, duplicate animation IDs, a job
owner that differs from the bound source profile, a job inventory that differs
from the declared total, folder-scan semantics, or a non-serial policy.
The implementation and successful execution of this route do not constitute
algorithm selection or adoption.

## Compatibility and limits

- Legacy external request v1 and loose UE request v2 remain readable and keep
  their original JSON shape.
- Legacy status v1 does not gain profile provenance.
- The fixed inventory safety limit is 100,000 selected animations.
- Profiles contain character configuration only; animation FBX and Golden
  JSON remain external catalog assets.
- No `.uasset` is read, no skeleton mapping is inferred, and no arbitrary FBX
  coordinate conversion is introduced.
- SKRV v1 remains a read-only Viewer boundary.
