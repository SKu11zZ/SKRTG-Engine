# Retarget performance and measurement

## Default contract

The normal profile-backed Batch route still produces the complete verified
review result for every animation:

- Foundation and Final FBX exports;
- FBX round-trip verification for both lanes;
- the four-lane review payload;
- one strictly inspected SKRV v1 package;
- one separately verified Final FBX copy.

Performance work must not silently replace that contract with a final-only or
unverified export. A future lighter output mode needs its own explicit request,
status, UI, and acceptance gate.

## Timing evidence

Profile-backed batch status v3 records:

- `timings.planningSeconds`: complete-selection validation and preflight;
- `timings.executionSeconds`: serial execution after planning;
- `timings.wallSeconds`: planning plus execution;
- `jobs[].timings.retargetWorkerSeconds`: Worker process wall time, including
  solver, review generation, dual FBX export, and FBX round-trip checks;
- `adapterSeconds`: frozen HTML-to-SKRV payload conversion;
- `packSeconds`: in-process payload inventory, integrity-index preparation,
  and atomic commit work outside strict inspection;
- `packageInspectSeconds`: normal strict SKRV inspection;
- `verifiedExportCopySeconds`: final hash-bound FBX copy;
- `bridgeTotalSeconds`: the complete per-job Bridge call.

These are elapsed wall-clock observations. They do not fabricate internal
solver-stage telemetry. See the
[batch status v3 schema](../schemas/skrtg.native_viewer.batch_retarget_status.v3.schema.json)
and [Bridge status v6 schema](../schemas/skrtg.native_viewer.retarget_bridge_status.v6.schema.json).

## 2026-08-03 two-clip control benchmark

One Windows x64 machine ran the same hash-bound Mixamo-to-Manny selection:
`Fist Fight A` (141 frames) and `Hokey Pokey` (351 frames). The control used
the preceding packaged executables; the optimized run used a clean Release
build. Both retained `maximumConcurrentJobs=1` and the complete verified review
contract.

| Measurement | Control | Optimized |
| --- | ---: | ---: |
| External CLI wall time | 36.820 s | 14.180 s |
| Completed jobs | 2 / 2 | 2 / 2 |
| Time saved | - | 22.640 s |
| Reduction | - | 61.49% |
| Relative throughput | 1.00x | 2.60x |

The optimized status reported 0.464 s planning and 13.678 s execution. Its
per-job breakdown was:

| Clip | Worker | Adapter | Pack | SKRV inspect | Final copy | Job total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Fist Fight A | 5.207 s | 0.367 s | 0.024 s | 0.172 s | 0.129 s | 5.916 s |
| Hokey Pokey | 6.980 s | 0.358 s | 0.026 s | 0.198 s | 0.147 s | 7.727 s |

Worker processes now account for about 85.9% of the measured optimized wall
time. Absolute timing is machine-, cache-, and asset-dependent; the table is a
controlled result, not a universal performance promise.

## What changed

1. Complete-selection preflight shares immutable hash, catalog, and Profile
   evidence within one planning pass. Every job keeps independent animation and
   Golden evidence and an exact request identity.
2. Batch execution consumes its successful plan instead of repeating the full
   Bridge preflight for every animation. The Worker still re-hashes solver
   inputs.
3. The Bridge seals the adapter payload in place. It avoids a second complete
   directory copy and avoids hashing all large payload files before the normal
   strict inspector hashes them.
4. The Bridge returns the verified Final FBX record from the package result, so
   Batch does not reopen and reinspect the same SKRV merely to find that export.
5. Status v3/v6 exposes each remaining phase so later work can be measured
   against the same trust boundary.

## Equivalence checks

The optimized and control runs used identical input SHA-256 bindings. For both
clips:

- the complete generated review HTML, including pose/mesh payload data, was
  byte-identical between runs;
- Foundation and Final FBX round-trip checks compared 12,549 and 31,239 samples
  respectively, with zero local and model mismatches;
- all before/after mesh fingerprints matched;
- both optimized SKRV packages passed payload-hash and manifest-semantic
  inspection;
- both packages passed the native Viewer's headless rendering smoke test.

Raw exported FBX file hashes are intentionally not used as a cross-run solver
equivalence key. The control executable itself produced different whole-file
hashes on separate runs, while its numerical round-trip evidence stayed the
same. Each individual output is still SHA-256 bound inside its own status and
SKRV package for integrity and provenance.

## Next performance gates

The remaining dominant cost is inside each Worker process. The next safe
sequence is:

1. add measured Worker sub-phases for FBX load, exact-Golden validation, solve,
   review serialization, export, and round-trip import;
2. design a persistent serial Worker protocol that can reuse immutable target
   skeleton, mesh, Rig, and alignment state while preserving per-job hash
   checks and failure isolation;
3. consider bounded parallel jobs only after a measured peak-memory and
   deterministic-output gate;
4. consider an explicit final-only mode only as a separate product contract,
   never as a silent replacement for verified review output.
