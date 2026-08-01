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

The batch panel applies the same contract to multiple animations. It shows
only installed source/target profiles and only catalog clips compatible with
the selected source. Profile-backed batch request v3 stores the two package
bindings once and stores each selected clip's ID, owner, FBX and Golden
bindings, stack, and import modes separately. Every resulting Bridge v5 job is
preflighted before the batch output directory is created. Execution is fixed
at `maximumConcurrentJobs=1`.

Animation-only FBX files are supported by the exact UE path without inventing
a bind pose. If the file contains no Mesh and no FBX BindPose, the
Worker requires its hash-bound Golden JSON reference hierarchy and every local
and model rest transform to match the source IK Rig within strict rest
tolerances. The FBX is still replayed and checked against every Golden key.
If any FBX bind evidence exists, the original SkinCluster/BindPose audit stays
mandatory; partial or damaged bind evidence cannot be bypassed by the Golden
fallback.

## Boundaries

v1 intentionally does not:

- contain animation FBX or Golden animation JSON;
- parse `.uasset`;
- infer skeleton mappings or bone names;
- generate an IK Rig or Retargeter;
- convert arbitrary FBX coordinate systems;
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
