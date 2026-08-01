# Architecture

## Runtime flow

```text
UE export tool
  -> IK Rig JSON
  -> IK Retargeter JSON
  -> rest/animation Golden JSON

External SHA-256 asset catalog
  + source animation FBX
  + animation Golden JSON

Installed .skrtgprofile v1 packages
  + source rest / IK Rig / alignment
  + target rest / IK Rig / alignment
  -> single-job Bridge v5
     or profile-backed Batch v3 -> one Bridge v5 request per clip
  -> complete-selection preflight
  -> UE IK JSON Worker
  -> verified target FBX + review payload
  -> SKRV packer
  -> native Viewer
```

The Worker owns algorithms and FBX I/O. The Viewer owns presentation,
selection, playback, and verified-export UX. SKRV v1 is the read-only boundary
between them.

The Viewer merges installed character profiles into the selectable skeleton
inventory while leaving animation files in the external catalog. Selecting a
source profile rebuilds the animation list from exact `sourceSkeletonId` and
`sourceSkeletonSignatureSha256` matches, so clips declared for another
character or an incompatible hierarchy are never shown. Catalogs may declare
`externalSkeletonIds` and contain animation records without duplicating loose
profile resources.

The Viewer writes a v5 Bridge request when either selected character comes
from a profile. The Bridge does not trust the Viewer's in-memory merge: it
re-hashes and inspects each bound package, checks profile identity, version,
role capability, and extracted resource hashes, then verifies the animation
against the original external catalog.

The profile-backed batch panel writes request v3. It accepts only installed
source and target profiles and explicit animation records that match the
source profile ID and skeleton signature. The batch planner converts every
record into the same Bridge v5 contract used by the single-job flow and
preflights all jobs before creating the output directory. It never scans a
loose animation folder. Execution remains streaming and serial: one Worker
process exits before the next starts. Legacy request v1/v2 remains readable
without acquiring profile or catalog provenance.

## Coordinate path

The exact input path replays the UE 5.8 FBX coordinate conversion with the
Autodesk FBX SDK, then compares local and model transforms against every
exported Unreal animation key. A mismatch fails the job before any result is
committed.

For an animation-only FBX with no Mesh and no FBX BindPose, the Worker uses
the hash-bound UE Golden reference skeleton only after its hierarchy and all
local/model rest transforms match the selected source IK Rig within strict
rest tolerances. An FBX containing any Mesh or BindPose remains on the original
direct bind-audit path; incomplete bind data fails closed.

Rest-pose reconciliation, root/pelvis ownership, chain FK, analytic limb IK,
finger transforms, and optional post operations are represented as explicit
stages. The route JSON supplies the bone inventory, hierarchy, chains, goals,
and retarget-pose alignment. Runtime bone-name guessing is not used.

## Review contract

The Viewer presents four synchronized lanes:

1. original source motion;
2. FK result with source overlay;
3. Foundation result with source overlay;
4. final result.

All lanes share clip, frame, camera, projection, and scale. The Viewer can
switch only between results already present in SKRV; it cannot alter the
retarget solve.

## Failure policy

- all external inputs are SHA-256 bound;
- route and hierarchy mismatches fail closed;
- output directories follow no-overwrite rules;
- profile-backed batch jobs all preflight before output creation and run one
  Worker at a time;
- a batch clip keeps its own catalog ID, owner, FBX hash, Golden JSON hash,
  stack name, and import modes in request and status provenance;
- profile-backed batch status cannot select or adopt the candidate route;
- staged operations commit only after validation;
- missing private catalogs produce an empty Viewer, not inferred defaults.
- profile installation uses a verified partial directory and atomic rename;
- damaged extracted profile resources are excluded during discovery;
- reinstalling the identical package atomically repairs damaged extraction;
- profile discovery accepts only the exact ID/version directory depth;
- profile staging and managed deletion reject symlinks and reparse points;
- extracted profile content must exactly match the six package records;
- profile deletion is limited to an exact receipt-bound managed directory.
