# `.skrtgprofile` v1

## Purpose

`.skrtgprofile` v1 is the portable character boundary for the UE IK JSON
route. One package describes one character and may be enabled as a source, a
target, or both.

It replaces loose character-specific paths in the Viewer. It does not replace
the external animation catalog, SKRV, or the Worker.

## Required inputs

Every package contains exactly six records:

| Record | Purpose |
|---|---|
| `manifest.json` | Package identity and payload inventory |
| `integrity.tsv` | Human-auditable byte count and SHA-256 index |
| `profile.json` | Character identity, version, capabilities, and bindings |
| `rest/character_rest.fbx` | Character rest-pose FBX |
| `rig/ik_rig.json` | UE-exported IK Rig definition |
| `alignment/canonical_to_character.ikretargeter.json` | UE-exported canonical-to-character IK Retargeter |

The UE JSON must use export schema v1 or v2 and declare the exact runtime
coordinate contract:

- left-handed;
- forward `+X`;
- right `+Y`;
- up `+Z`;
- centimeters;
- quaternion order `x,y,z,w`.

The IK Retargeter's target IK Rig asset name must equal the packaged IK Rig
asset name. The package stores the exported reference-skeleton fingerprint as
its skeleton signature. No `.uasset` is read.

Packages created through the unified Character Definition compiler also add
an optional `profile.json.authoring` record with source format, source
SHA-256, importer id/version, and `restPoseKind`. This metadata is part of the
hashed `profile.json`; it does not add a seventh container record or weaken
the six-record runtime contract. Legacy v1 packages without this additive
metadata remain valid.

## Active runtime Mesh / LOD selection

`profile.json` may contain a `skrtg.character_mesh_selection.v1` record:

```json
{
  "meshSelection": {
    "schema": "skrtg.character_mesh_selection.v1",
    "schemaVersion": 1,
    "activeLod": 0,
    "meshNodePaths": [
      "CharacterRoot/Character_LodGroup/Character_LOD0"
    ]
  }
}
```

`meshNodePaths` are exact FBX scene paths relative to the FBX scene root.
They are not regular expressions and are never derived from node names. More
than one path may be listed when one LOD is split into body, head, clothing,
or other intentional Mesh nodes. `activeLod` is explicit audit metadata; the
paths are the authoritative selection, so an `_LOD0` suffix is not assumed.

The complete source FBX remains in the profile and verified target FBX exports
retain its complete Mesh/LOD inventory. Only the HTML/SKRV/Native Viewer
review payload is filtered to the declared paths. Mesh selection therefore
cannot modify the skeleton, rest pose, chain mapping, animation, or solver.

For compatibility, a legacy profile without `meshSelection` remains valid
when its review FBX has exactly one Mesh node. On the UE IK JSON runtime path,
an FBX with multiple Mesh nodes and no explicit selection fails closed and
reports the available exact paths. A missing path, duplicate path, non-Mesh
path, or incomplete declaration also fails before an SKRV package is
committed. This prevents several full-body LODs from being skinned and drawn
at the same transform.

## Identity and versions

`profileId` is portable lowercase ASCII. It may contain letters, digits,
periods, hyphens, and underscores, cannot begin or end with punctuation, and
cannot use reserved Windows device names.

`profileVersion` uses SemVer core and optional lowercase prerelease
identifiers, for example `1.0.0` or `1.1.0-preview.2`. When multiple versions
of one ID are installed, the Viewer activates the highest SemVer version.
Older versions remain installed until explicitly removed.

## Binary container

The v1 container is deliberately simple and stream-verifiable:

1. 16-byte magic: `SKRTGPROFILEV1\r\n`;
2. little-endian `uint32` record count;
3. for each record:
   - little-endian `uint16` UTF-8 path byte count;
   - little-endian `uint16` flags, currently zero;
   - little-endian `uint64` payload byte count;
   - 64 uppercase ASCII SHA-256 characters;
   - portable relative path bytes;
   - payload bytes.

The reader rejects duplicate or case-colliding paths, absolute paths,
traversal, Windows device names, unsafe characters, unknown flags, oversized
records, missing records, extra records, trailing bytes, and any hash
mismatch. Payload hashing is streamed; FBX files are not loaded into one large
memory buffer. Each definition JSON is limited to 16 MiB, 128 nested levels,
and one million parser events.

## Installation

The default store is:

```text
%LOCALAPPDATA%\SKRTG\Profiles\<profileId>\<profileVersion>
```

`SKRTG_PROFILE_STORE` may override the root for isolated testing or managed
deployments.

Installation:

1. fully inspects the source package;
2. creates a receipt-bound partial directory;
3. copies and re-hashes the package;
4. extracts each record and verifies it again;
5. writes `install.json`;
6. commits with an atomic directory rename.

An identical reinstall is idempotent. A different package with the same ID
and version is refused. Discovery verifies the package, receipt, directory
identity, and every extracted resource. A damaged install is hidden and
reported as a warning. Reinstalling the exact same package hash and identity
atomically rebuilds damaged extracted resources; it never replaces a
different package.

Discovery recognizes only the exact two-level layout
`<profileId>/<profileVersion>`, rejects directory symlinks, and ignores
unmanaged nested copies. Staging, discovery, verification, and deletion reject
symlinks, Windows reparse points, and junctions. The extracted `content`
directory must remain an exact six-record mirror of the package: missing,
changed, or extra entries all fail closed.

If `install.json` or `package.skrtgprofile` itself is damaged, v1 does not
automatically replace or delete that directory. Remove the exact damaged
ID/version directory from the profile store manually, then install the source
package again. Automatic repair is intentionally limited to extracted content
whose receipt and package still prove the same identity and hash.

Deletion is accepted only for the exact managed ID/version directory whose
receipt matches the known package hash.

## Viewer and Bridge behavior

The Viewer merges active profiles into its source and target skeleton lists.
An installed profile replaces a same-ID loose catalog skeleton for character
resources. Animation files stay external.

The animation selector shows only enabled records whose
`sourceSkeletonId` and `sourceSkeletonSignatureSha256` exactly equal the
selected source profile identity and IK Rig reference-skeleton fingerprint.
The catalog field is an explicit profile binding; it does not claim that an
animation asset's own UE Skeleton reference pose is byte-identical to the
profile rest pose. Choosing a different source clears the old animation
selection. This prevents
an animation authored for an older, structurally different version of the
same profile ID from reaching the Worker.

An animation-only catalog may omit loose character resources. It declares
profile-owned skeleton IDs at its root and binds each clip to a skeleton
fingerprint:

```json
{
  "externalSkeletonIds": ["mixamo_ybot", "metahuman"],
  "skeletons": [],
  "animations": [
    {
      "id": "mixamo_walk",
      "sourceSkeletonId": "mixamo_ybot",
      "sourceSkeletonSignatureSha256": "<64 uppercase hex characters>"
    }
  ]
}
```

Unknown animation owners still fail catalog validation. In a multi-profile
catalog, `externalSkeletonIds` is the complete declared inventory used by the
independent Bridge process; it is not inferred from installed directories.

When a profile-backed selection is launched, Bridge request v5 records:

- source and target profile IDs and versions;
- source and target package paths and SHA-256 values;
- all selected character-resource hashes;
- the original animation catalog identity and hash;
- the animation ID, source profile ID, FBX hash, and Golden JSON hash.

The Bridge independently inspects the packages and checks their installed
resource paths before running the Worker. A changed package, changed extracted
file, mismatched role, wrong version, wrong animation owner, wrong skeleton
fingerprint, or changed catalog fails preflight without starting a child
process.

When present, the Bridge also forwards the inspected profile's exact Mesh
selection across the process boundary. The Worker validates it again against
the hash-bound rest FBX. The Viewer never chooses or guesses a LOD itself.

The batch panel applies the same contract to multiple animations. It shows
only installed source/target profiles and only catalog clips compatible with
the selected source. Profile-backed batch request v3 stores the two package
bindings once and stores each selected clip's ID, owner, FBX and Golden
bindings, stack, and import modes separately. Every resulting Bridge v5 job is
preflighted before the batch output directory is created. Execution is fixed
at `maximumConcurrentJobs=1`. Successful jobs are exposed in the Viewer as an
animation selector while remaining independent, strictly revalidated SKRV
packages.

Animation-only FBX files are supported by the exact UE path without inventing
a bind pose. If the file contains no Mesh and no FBX BindPose, the
Worker requires its hash-bound Golden JSON reference hierarchy and every local
and model rest transform to match the source IK Rig within strict rest
tolerances. The FBX is still replayed and checked against every Golden key.
If any FBX bind evidence exists, the original SkinCluster/BindPose audit stays
mandatory; partial or damaged bind evidence cannot be bypassed by the Golden
fallback.

## Boundaries

The v1 runtime package itself intentionally does not:

- contain animation FBX or Golden animation JSON;
- parse `.uasset`;
- infer skeleton mappings or bone names;
- infer or generate retarget semantics from bone names;
- generate an alignment Retargeter;
- claim UE FullBodyIK parity;
- select or adopt an algorithm route;
- change SKRV v1 or let the Viewer recompute retargeting;
- put animations inside a profile;
- let profile-backed batch scan arbitrary folders or infer clip ownership.

Adding Manny, Vicon, SMPL, or another character requires one separately
authored profile plus external animation records bound to the same profile ID
and skeleton fingerprint. A profile itself does not require loose skeleton
entries in the animation catalog. Adding a profile does not require adding
pairwise mappings to every other character because the alignment remains
canonical-to-character.

The separate Character Profile authoring layer can compile an explicitly
mapped SKRTG JSON/XML definition into the packaged UE IK JSON shape and can
normalize a Rest FBX skeleton using the registered FBX adapter. A Rest FBX by
itself remains a draft: it does not supply root, pelvis, chains, goals, or
canonical alignment. See [Character Profile authoring v2](CHARACTER_PROFILE_AUTHORING_V2.md).
