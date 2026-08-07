# SKRTG Standard T-Pose Reference v1

Status: human visual review accepted on 2026-08-07 for Mixamo Y Bot,
UE5 Manny, MetaHuman, and the UE-compatible SMPL-X character.

This document is the pose-authoring baseline for future work involving those
four skeleton families. It defines the visible target, character-specific
exceptions, validation rules, and evidence boundary. It does not contain or
depend on external reference-character identifiers, assets, paths, hashes,
screenshots, or configuration files. Those must not be added to Git or a
release package.

## What acceptance means

The accepted result is a visually reviewed standard T-pose. Acceptance covers
the character's static pose, facing direction, grounding, arm and hand
orientation, and the absence of obvious mesh damage in the selected review
mesh. It does not by itself approve:

- animation-retarget quality;
- skin-weight, material, texture, morph-target, cloth, or facial fidelity;
- every LOD as an independently reviewed runtime mesh;
- a new solver route or automatic pose-conversion policy.

An authored Rest Pose and an alignment T-pose are different contracts. For
example, a character may retain an A-pose Rest FBX while an explicit alignment
artifact supplies the accepted T-pose. Never relabel or replace the Rest Pose
silently.

## Shared visual standard

Evaluate all directions in the character's declared coordinate basis. Do not
assume that a raw FBX, Blender scene, or DCC viewport uses the runtime axes.
Normalize the declared basis first, then apply these rules:

1. The character faces its declared forward direction with no unintended root
   yaw, roll, or lateral lean.
2. The mesh is upright and grounded. The lowest intended foot contact is on
   the ground plane without moving the character's internal proportions.
3. The pelvis is centered and the spine keeps a natural, visually upright
   profile. Preserve accepted anatomical curvature; do not force one shared
   numeric spine lean onto every character.
4. Both arms extend laterally in the horizontal plane. The shoulder-to-hand
   line is visually perpendicular to the character up axis and approximately
   perpendicular to the trunk.
5. Arm roll remains anatomical: the elbow crease faces forward and the elbow
   point faces backward. A horizontal arm is not sufficient if the forearm is
   axially twisted.
6. Palm surfaces face down. Determine the palm plane from the index-through-
   pinky side of the hand, not from the thumb direction.
7. Index, middle, ring, and pinky chains are straight and continue the hand
   direction without visible curl, splay, or alternating phalanx roll.
8. Each thumb points downward enough to read as the accepted T-pose while
   retaining its side-specific horizontal direction, internal curvature, and
   roll. Do not flatten the thumb into the palm.
9. Legs remain neutral, knees and feet follow the declared forward direction,
   and the soles make a stable ground contact.
10. Character proportions, bone names, hierarchy, mesh topology, skin binding,
    and supported shape-key inventory remain unchanged unless a separately
    versioned operation explicitly declares otherwise.

## Character-specific rules

### Mixamo Y Bot

The original accepted pose is an identity-pass case. Do not apply a generic
spine correction, because doing so previously introduced an unwanted forward
lean. When the bound input is the accepted identity case, preserve it byte for
byte instead of running a cosmetic conversion.

### UE5 Manny

The current standard T-pose is accepted without a special asymmetric override.
Preserve the accepted torso attitude, shoulder placement, anatomical elbow
roll, palm-down orientation, and straight long fingers. A future converter
change must demonstrate that it does not disturb those properties.

### MetaHuman

MetaHuman's authored pose may be an A-pose; conversion to the accepted T-pose
must therefore remain explicit alignment data. It must not silently mutate the
meaning of the original Rest Pose.

The reviewed FBX can contain several complete body LOD meshes at the same
transform. Review and SKRV generation must use the exact Mesh node paths from
the Character Profile's active mesh selection, currently the intended LOD0
set. Drawing every LOD simultaneously creates false surface thickening and
finger/body overlap that can be mistaken for pose or skinning damage. Export
may preserve the full LOD inventory, but the review payload must contain only
the declared active set.

### SMPL-X, UE-compatible

Do not assume mirrored local bone axes or derive one arm from the other. The
accepted result uses independent side policies: preserve the spine attitude,
retain the left arm's source elbow shape and roll as a rigid aggregate, and
align the right arm by its explicit segments. Treat both thumbs independently
and preserve each side's azimuth, curvature, and roll while tilting it down.

This exception is intentional. A visually symmetric target does not imply a
mathematically symmetric source skeleton.

## Converter and validation rules

- All bone and chain roles come from explicit Character Profile data. Never
  infer a runnable conversion from bone names alone.
- Solve left and right sides independently. Mirroring is allowed only when the
  profile explicitly proves compatible axes and the result passes review.
- Prefer identity passthrough for an already accepted pose.
- Make spine alignment optional per character. A universal lean target is not
  part of this standard.
- Rotate an arm or thumb as a rigid aggregate when preserving its accepted
  bend and roll is more important than forcing every segment to an ideal line.
- Apply exact per-segment alignment only where the character-specific policy
  calls for it.
- Bake the pose and mesh consistently. A skeleton-only pose edit with an
  unbaked or differently based skinned mesh is invalid.
- Write to a new versioned output, re-import it into a clean scene, validate,
  and commit the final file only after every required check passes.
- A failed conversion produces no committed output and never overwrites the
  accepted baseline.

The current default numeric checks are subordinate to human review:

| Check | Default tolerance |
| --- | ---: |
| Arm direction relative to its selected horizontal target | 0.5 degrees |
| Hand direction | 1.0 degree |
| Palm-down plane | 2.0 degrees |
| Long-finger direction | 1.0 degree |
| Selected spine and facing directions | 1.0 degree |
| Ground distance | 0.2 cm |

These tolerances validate a declared per-character target; they do not define
one universal bone rotation. Passing them cannot overrule visible leaning,
twist, mesh damage, or a wrong active LOD.

## Required visual review

Review the converted mesh, not only the skeleton, from front, side, and
three-quarter views. Confirm:

- facing, grounding, pelvis centering, and spine attitude;
- arm elevation and left/right shoulder continuity;
- elbow crease forward and elbow point backward;
- palm and long-finger surfaces down;
- straight long fingers and independently correct thumbs;
- no new torso lean, asymmetric arm twist, collapsed fingers, or mesh doubling;
- only the declared active mesh set is visible during LOD-bearing review.

Any future change to these four accepted baselines is versioned and requires a
new numeric re-import audit plus human visual review. Until that happens, the
accepted result remains the rollback target.
