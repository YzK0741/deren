# Runtime cluster-DAG import checkpoint — 2026-10-22

Execution follows the already reviewed
[spec](../superpowers/specs/2026-10-21-object-lod-runtime-design.md) and
[plan](../superpowers/plans/2026-10-21-object-lod-runtime.md).
Base: `972fd77e`. Current master checkout, local commits, no push.

## Task 1: engine DAG representation

- Added an owning, move-only `dag_asset` with read-only cluster/group/reference/
  leaf/root views and a sorted, contiguous LOD0 meshlet prefix.
- Validates reciprocal producer/consumer links, bottom-up dependencies, precise
  leaf/root sets, ranges, finite spheres/errors, monotone metrics and strict
  triangle progress. The asset layer permits more than 4096 leaves; runtime
  admission applies the packed-item limit separately in Task 3/5.
- Root CMake and the sanitizer CI target mirror include the new CPU-only test;
  existing forest asset/cut tests remain unchanged.
- RED: missing module, then factory stub (23 checks, 2 failed). An additional
  conservative-bound regression also failed before removing an invalid center-
  containment assumption. Current focused Release DAG/forest/docs tests pass;
  ASan/UBSan DAG test passes (0.42s). Root Release build and full CTest pass
  **59/59** (36.66s). Initial sandbox run blocked the backend-boundary test's
  hardlink fixtures with WinError 5; unchanged tests pass outside the restriction.

Rulings:

1. `make` accepts an optional backing index-count bound. Production import must
   supply it; the convenience form cannot validate a backing buffer it never sees.
2. Check nesting of dependency LOD spheres, but do not require a conservative
   cluster sphere or its center to lie inside another independent conservative
   sphere. Both enclose actual vertices, verified offline. This follows the
   approved trust boundary; runtime does not repeat full geometry validation.

Runtime loader, upload adoption and per-frame DAG selection are still pending.
The new representation alone does not enable object LOD in rendering.

## Task 2: shared wire library and CPU DAG cut

- Root `asset_subtilis_core` compiles the original codec/digest/DAG/cut sources.
  No converter source, wire format, builder or loader dependency was changed.
- Engine selection computes metrics, closes dependency decisions, then scans
  cluster IDs once. The same perspective/orthographic and operator-norm formulas
  produce identical selected IDs in **590 cases**, including exact thresholds,
  nonuniform scale/shear, near-plane and invalid-view fallbacks.
- Closure has an independent inconsistent-decision/idempotence regression;
  its no-op stub failed before the actual helper was connected. Capacity errors
  clear all output; runtime will provide the complete LOD0 fallback in Task 5.
- Three CLI-generated assets (actual automatic cross-DAG, UV seam, LOD0-only)
  have checked SHA-256 manifests and full reference validation in root tests.
  The cross fixture also scans producer/consumer links independently of metadata.
- Reference library archive: 3,106,236 B with diagnostics. The executable remains
  88,493,568 B before/after: runtime does not reference the new import path yet,
  so unused static objects are not pulled into it. This is not a final size cost.

Validation: initial cut stub RED (590 failures); raw closure regression RED
(2 failures). Root Release build/full CTest **60/60** (42.69s), ASan/UBSan DAG
tests **2/2** (1.71s), independent converter CTest **6/6** (1.42s). No GPU
performance or runtime adoption is claimed at this step.

## Task 3: CPU payload import and fallback classification

- `import_payload` accepts the loader's 64B interleaved vertices and 16/32-bit
  indices, reuses the wire decoder/structural validator/source fingerprint, and
  creates an owning DAG plus the exact combined index stream. Normal cones make
  no claim (`axis=0, cone_cos=0`); original vertex data is copied unchanged.
- Immutable imported data can be shared; cache identity is
  `{sourceHash, SHA-256(wire bytes)}`, with buildHash retained separately.
- Tests cover invalid extension/wire, profile/material rejection, source/NaN/
  index mismatches, invalid DAG, LOD0-only and >4096 leaves. Source ownership,
  repeat identity and equivalent 16/32-bit core indices also pass. Table budgets
  and frame/admission policies belong to Tasks 4–6, not this pure function.
- Production import does not call `make_asset` or repeat geometric boundary
  analysis; full fixture validation remains in Task 2.

Ruling: the brief's `3*index_count` was a unit typo. `import_input.index_count`
already counts indices, so the LOD0 prefix must equal that count, per spec §4.3.
Callers must supply index counts, not triangle counts.

Validation: initial import stub RED (12 failures). Root Release build/full CTest
**61/61** (42.76s), ASan/UBSan import **1/1** (1.00s). Loader extraction and actual
GPU adoption still follow in Tasks 4/5.

## Task 4: loader extraction and runtime import staging

- Added opt-in sync/async load options and a loader-owned payload view. One
  bounded yyjson pass locates metadata; original parsed buffers provide the bytes.
  Instances of one mesh share the same payload owner, including after scene move.
- The app requests extraction only for `render.subtilis=true`, for both primary
  and background models. Default loads do not parse/copy these payloads.
- `import_scene` optionally detects the view, validates source/DAG data, shares
  imported CPU assets by source/wire hash and records per-reason fallback counts.
  Real loader streams run through a probe-backed runtime in the CPU test. An
  observing iterator confirms OFF reads the payload getter zero times.
- Loaded declarations with bad metadata keep `present=true` and empty/invalid
  fields so engine import can count `invalid_payload`; this reconciles spec §4.4
  with the brief's conflicting `present=false` wording. Presence alone is never
  evidence that a payload can be adopted.
- CPU ownership is staged through `subtilis_import` until Task 5 extends
  `built_geometry`. Rendering still uses LOD0 at this checkpoint; no GPU DAG
  adoption or coarse-frame benefit is claimed yet.

Validation: loader stub RED (5 failures), runtime staging RED (3 failures).
External buffers, malformed/out-of-range extension metadata, unsupported
attributes, optional unknown extensions, async/move lifetime, per-instance
fallback and shared ownership pass. Root Release build/full CTest **61/61**
(52.43s). ASan/UBSan loader/import/frame/injection **4/4** (84.11s).

## Task 5: adopted GPU generations and frame cuts

Payload leaves now define the full LOD0 item run; shared immutable CPU data and
GPU generations are reused across instances. Generation checks compare exact leaf
records and retaining owners. Adopted items use the converter-compatible DAG cut,
with complete LOD0 fallback when the cut exceeds its reservation. A fully geometry-
validated fixture exercises a legal five-record intermediate cut with four leaves.

The old builder also exposes a validated DAG representation, with outward-rounded
dependency bounds and its storage included in the builder byte budget. Explicit
builder uploads retain the existing forest selector to preserve their selection
behavior (different projection norm/order); complete finest/root cut sets agree.
Both forest and adopted paths pass frame-table publication and dispatch tests,
including MASK, exact depth, disabled mode, ownership and overlap fallback.

Validation: GPU-generation RED (1 failure), wrapper RED (1 failure). Release full
CTest **61/61** (63.53s); focused ASan/UBSan **5/5** (15.62s). No push. Runtime
configuration, whole-table admission, image gates and scene measurements remain
Tasks 6–8; this is not final feature acceptance.

## Task 6: runtime policy and capacity

Added positive finite `subtilis_error_px` (default 1.0) and optional
`subtilis_hysteresis` (default false), wired through the app. Disabled history
retains reference cuts. Enabled history coarsens at 80% and refines above 100%;
primitive/weak-generation identity, exact model bits and a view epoch isolate it.
Camera jumps use the existing HZB recovery gate's default motion limits; resize
or generation replacement invalidates history. Parallel planners edit local
history copies; serial publication merges only accepted decisions.

Whole-table admission rejects adopted runs before GPU upload, keeping LOD0 on
fallback. Exact-full/one-extra/overflow counts pass. A 4097-visible-item frame
writes no partial visibility table; an offscreen invalid run is culled before
admission. Actual HZB item descriptions carry the selected cut count. Shadow and
BLAS draw counts remain the LOD0 index prefix.

Validation: final Release full **61/61** (53.89s), ASan/UBSan **5/5** (13.55s).
Frozen legacy **15/15**, visibility **1/1**, no baseline updates. The older
`-Compare frozen` shortcut failed against its obsolete internal reference; the
plan's explicit reference passed. Payload OFF and extension-stripped identical
geometry OFF have **0 changed pixels** at 320x240 with validation clean. Small
images, comparison JSON and gate logs are in [the evidence folder](2026-10-22-object-lod/).

The final timing counters (observation only) separate worker CPU work/us per item
from planning wall time and print every 120 selected frames when GPU timings are
requested. Task 7 must measure actual costs/benefits; Task 8 repeats final image
acceptance after all changes. Tasks 7/8 and whole-change review are outstanding.
