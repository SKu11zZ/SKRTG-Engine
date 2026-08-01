# Architecture

## Runtime flow

```text
UE export tool
  -> IK Rig JSON
  -> IK Retargeter JSON
  -> rest/animation Golden JSON

External SHA-256 asset catalog
  + source rest FBX
  + source animation FBX
  + target rest FBX
  -> Bridge preflight
  -> UE IK JSON Worker
  -> verified target FBX + review payload
  -> SKRV packer
  -> native Viewer
```

The Worker owns algorithms and FBX I/O. The Viewer owns presentation,
selection, playback, and verified-export UX. SKRV v1 is the read-only boundary
between them.

## Coordinate path

The exact input path replays the UE 5.8 FBX coordinate conversion with the
Autodesk FBX SDK, then compares local and model transforms against every
exported Unreal animation key. A mismatch fails the job before any result is
committed.

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
- batch jobs run one Worker at a time;
- staged operations commit only after validation;
- missing private catalogs produce an empty Viewer, not inferred defaults.
