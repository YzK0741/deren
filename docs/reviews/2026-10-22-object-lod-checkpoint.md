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
