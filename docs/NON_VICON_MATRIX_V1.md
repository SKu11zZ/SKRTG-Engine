# Non-Vicon Matrix V1

## Scope

The private validation corpus for this stage contains four character profiles:

- Mixamo Y Bot;
- MetaHuman;
- UE5 Manny;
- SMPL.

Vicon is explicitly outside this stage. Character FBX, animation FBX, Unreal assets, exported definitions, Golden JSON, SKRV packages, and compiled releases remain outside the public source repository.

## Input inventory

The supplied corpus contains four independent motion clips suitable for the production catalog:

- two Mixamo clips;
- two UE5 Manny clips.

Three additional Unreal `AnimSequence` assets associated with imported character FBX files were inspected and excluded. Each contains one frame over approximately 0.033 seconds and is a static import sequence, not a usable motion clip.

MetaHuman and SMPL do not yet have independently authored source-motion FBX inputs in the supplied corpus.

## Profile coverage

Mixamo Y Bot, MetaHuman, UE5 Manny, and SMPL are each represented by a verified `.skrtgprofile v1` package. SMPL is no longer consumed as a loose Rest/IK definition on this route.

Each profile remains canonical-to-character. Supporting four profiles therefore does not introduce twelve pairwise configuration files.

## Validation coverage

The four profiles produce twelve non-identity directed routes.

- Static route construction and IK/FK chain validation passed for all 12 directions.
- Original Mixamo and Manny source motions produced 12 successful dynamic jobs across six directions.
- MetaHuman and SMPL reverse-path validation used one explicitly labeled UE round-trip probe per source profile and produced six successful dynamic jobs across the remaining six directions.
- All 18 result packages passed independent SKRV package, manifest-semantics, and payload-hash inspection.
- All 18 final FBX files matched the SHA-256 recorded in Batch status v2.

Every execution retained:

```text
candidateRouteSelected=false
candidateRouteAdopted=false
maximumConcurrentJobs=1
```

The round-trip probes were created by importing successful engine output into UE 5.8 and exporting hash-bound Golden motion data. They demonstrate that MetaHuman and SMPL can traverse the source side of the runtime, but they are not substitutes for independently authored source clips.

## Product boundary

- Configuration is exported from UE 5.8 as JSON; the runtime does not parse `.uasset`.
- Animation FBX and Golden JSON remain external to `.skrtgprofile`.
- The production catalog exposes only the four independently supplied motion clips.
- Static one-frame imports and round-trip probes are not presented as product animation choices.
- MetaHuman finger behavior remains on the preserved baseline; this stage does not adopt a new finger algorithm.
- Materials, textures, morph targets, Blend Shapes, portable runtime, and cross-platform releases remain outside the validated gate.
