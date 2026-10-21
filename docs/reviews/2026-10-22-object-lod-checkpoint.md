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
