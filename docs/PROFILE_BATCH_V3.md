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

The planner hashes and parses immutable shared evidence once per planning
pass. Later jobs may reuse catalog, Profile, rest, Rig, and alignment evidence,
while animation FBX and Golden JSON remain job-specific. Every returned
preflight carries an opaque identity for the complete Bridge request,
including its output path; supplying it to a different job fails before a
Worker can start.

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

The Bridge consumes the already completed batch preflight. The Worker still
re-hashes its solver inputs against the supplied SHA-256 values. After the
Worker and frozen HTML-to-SKRV adapter finish, the Bridge seals the adapter
payload in place, creates `integrity.tsv`, performs the normal strict SKRV
inspection once, and only then commits `review.skrv`. This removes redundant
whole-package copies and inspections without weakening the SKRV v1 boundary.

Stable outputs use the catalog animation ID:

```text
FinalFBX/<animationId>__SKRTG_Final.fbx
Jobs/<index>_<animationId>/review.skrv
batch_status.json
```

## Status contract

Profile-backed status uses:

```text
skrtg.native_viewer.batch_retarget_status.v3
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

Status v3 adds measured phase timings. Top-level `timings` reports planning,
execution, and end-to-end wall time. Every job reports planning preflight,
whether its Bridge preflight was reused, Worker, adapter, package preparation,
strict package inspection, verified-export copy, and Bridge total time. These
are elapsed wall-clock measurements, not inferred solver telemetry. Status v2
remains readable; missing timing fields are interpreted as zero.

The versioned machine contract is
[`schemas/skrtg.native_viewer.batch_retarget_status.v3.schema.json`](../schemas/skrtg.native_viewer.batch_retarget_status.v3.schema.json).

## Viewer animation selector

Every successful job is projected into a Viewer playlist in deterministic
batch-job order. The batch-result panel provides a successful-animation
selector and `加载所选动画到四视图`. The main review page shows an `动画`
selector whenever the current batch contains more than one successful
animation. If no SKRV is open yet, the empty review page offers the same
successful-animation list.

Selecting an animation does not trust the batch status as display data. The
Viewer opens that job's independent `review.skrv`, repeats the full SKRV
integrity and manifest validation, then pauses playback, seeks to frame zero,
loads the selected clip's frame count and rate, and resets the synchronized
camera baseline. Failed, pending, incomplete, or pathless jobs are not listed.

This is a presentation playlist, not a new container format: batch v3 still
stores one SKRV per animation and SKRV v1 remains unchanged.

## Compatibility and limits

- Legacy external request v1 and loose UE request v2 remain readable and keep
  their original JSON shape.
- Legacy status v1 does not gain profile provenance.
- Profile-backed status v2 remains readable; new writers emit v3.
- The fixed inventory safety limit is 100,000 selected animations.
- Profiles contain character configuration only; animation FBX and Golden
  JSON remain external catalog assets.
- No `.uasset` is read, no skeleton mapping is inferred, and no arbitrary FBX
  coordinate conversion is introduced.
- SKRV v1 remains a read-only Viewer boundary.
