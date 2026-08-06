# Operation System v2

Operation System v2 is the candidate-only Final-stage pipeline layered after
the frozen Foundation result. It does not alter the Foundation algorithms or
silently adopt a new route. A configured run reports `candidate=true`,
`selected=false`, and `adopted=false` until a separate quality review changes
that product decision.

## Execution model

The system carries named model-space goals through ordered phases and performs
pose writeback in one final solver phase:

```text
Foundation local/model poses
  -> explicit goal seeds
  -> goal_generation
  -> goal_warp
  -> temporal_goal_constraint
  -> spatial_goal_constraint
  -> pose_solve
  -> post_solve (reserved for later operators)
  -> Final local/model poses
```

Every operator declares its phase and exact bone/goal write set. The runtime
checks phase ordering and dependencies, runs operator preflight, rejects writes
outside the declaration, rebuilds and validates model pose after each stage,
and commits Final only when the complete stack succeeds. A disabled entry is an
exact value-for-value passthrough. Disabled entries still need valid JSON and
exactly resolvable names; disabling execution is not a way to hide invalid
configuration.

`per_operator_audit` runs each enabled operator twice and compares the complete
result bit-for-bit. `single_pass` runs once and reports repeatability as not
checked; it is the intended production/performance mode after tests or a
separate audit have established determinism.

## Candidate operators

### Weapon Goals (exact name)

`weapon_goals_exact_name_v1` creates or updates named goals from an explicitly
named anchor bone. `Grip` and `Handle` are ordinary case-sensitive names, not
keywords. An anchor may belong to the aligned source skeleton or the current
target skeleton, and may use an authored rigid offset or preserve the input
goal's first-frame offset.

The operator only generates goals. `unified_goal_solver_v1` owns the eventual
pose write. For a hand attachment, a two-bone arm binding is normally safer
than translating the hand bone directly. For a backbone aim, rotation-only
direct writeback is the conservative starting point; translating a spine bone
can change bone lengths and deform the mesh.

Current boundary: the anchor must be a bone in the selected source/target
character pair. A separately loaded weapon skeleton, socket graph, physics
constraint, multi-effector full-body solve, or automatic `Grip`/`Handle`
discovery is not implemented.

### Stride Warping

`stride_warping_goal_space_v1` scales the forward and lateral components of
explicitly named goals around an explicit target pivot. Forward/up axes and
left/right signs are authored data; no bone-name or facing-direction inference
is used. This is deterministic goal-space warping, not motion matching, speed
prediction, pelvis compensation, or gait synthesis.

### Contact Foot Plant v2

`contact_foot_plant_v2` detects contact from an explicitly named source contact
bone and optional local contact point. It uses speed/height entry and exit
thresholds, confirmation frames, minimum plant duration, release blending, and
an anchor-drift guard. The plane is explicit model-space data.

This is source-motion contact evidence. It does not read force plates, authored
contact curves, scene collision, or terrain. Thresholds therefore need
character- and clip-appropriate review.

### Ground / Floor Constraint

`ground_floor_constraint_explicit_plane_v1` prevents named goals (or authored
local-space foot footprint points) from penetrating an arbitrary explicit
plane. It can optionally align a declared goal-up axis to the plane normal. It
only corrects penetration upward; it does not raycast, follow uneven terrain,
solve stairs, or lower a hovering foot.

### Unified Goal Solver

`unified_goal_solver_v1` is the only pose-writing stage in the example stack.
It supports analytic two-bone chains and explicit direct-bone writes. Multiple
goal modifiers therefore do not each re-solve the same limb. Overlapping bone
writes fail preflight.

Direct-bone translation is deliberately available for explicit technical
uses, but it changes the local translation relative to the parent and can
stretch a skinned chain. Prefer a two-bone binding for arms and legs.

## Configuration and use

Start from [`examples/op-stack-v2.example.json`](../examples/op-stack-v2.example.json)
and replace every illustrative bone/goal name with names that exist exactly in
the selected profiles. The machine contract is
[`schemas/skrtg.op_stack.v2.schema.json`](../schemas/skrtg.op_stack.v2.schema.json).

In the native Viewer, choose the optional **Operation System v2 JSON** for a
single job or once for the whole batch. The Bridge hashes it during preflight,
passes the resolved digest to the Worker, and records the config and every
stage result in provenance. Direct automation uses Bridge request v6 or
profile-backed Batch request v4. Omitting the config preserves the existing v5
or v3 request route and exact Foundation-to-Final passthrough behavior.

The current UI selects one complete JSON program; it does not yet expose a
live per-operator parameter editor. The generated Final result is reviewable,
but these operators are not adopted production defaults.
