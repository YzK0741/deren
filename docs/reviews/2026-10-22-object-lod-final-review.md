# Object LOD runtime — final review

One fresh, read-only whole-change review covered `972fd77e..bc5bd068` and the five
focus areas in the approved plan. No converter sources were changed. The reviewer
reported no Critical findings and two Important findings. Both are resolved in
`8b331db9`; the lead accepts the scoped implementation with the source-validator
exception stated below. No push.

## Important findings and dispositions

1. **P1: partial meshlet publication after table-capacity fallback.** Confirmed.
   The old runtime resized a run to remaining capacity; visibility then admitted
   the nonempty prefix. The regression failed on missing zero-run/full-frame
   fallback and on consumption of the last slot (three checks). Upload now rejects
   an entire overflowing run, keeps all source indices and leaves remaining slots
   available. Visibility rejects visible zero-run objects. Surface/forward/shadow
   paths must also choose ordinary mesh pipelines after overflow; testing only the
   visibility table would miss that second omission. Runtime frame tests inspect
   the production environment and complete geometry lanes; real pending-import
   creation tests cover exact-full, one-slot and zero-room boundaries. The expanded
   synthetic payload tests capacity bookkeeping, not offline geometry validation.

2. **P2: hysteresis history survives instance address reuse.** Confirmed.
   Shared geometry can outlive a removed instance. A deterministic placement
   destruction/reconstruction test retains that generation and reuses the same
   primitive address/model in the 80–100% hysteresis band. The old runtime incorrectly
   stays at two records; a fresh instance needs four (two failing checks, including
   explicit scene notification). Each primitive now has a monotonic lifetime token,
   checked alongside generation/model. `scene_changed()` and `set_scene()` clear
   histories, removing entries whose geometry may remain shared. This replaces the
   Task 6 ruling that an address alone identifies an import instance over time.

## Explicit scope rulings

- Repeating full geometry/boundary validation at runtime: declined; full validation
  remains the offline converter's contract. Runtime structural/source checks remain.
- Cleaning inherited Bistro Khronos errors or diagnosing existing near-OFF jitter:
  declined; both are transparently quantified in the measurement report. The real
  asset's Khronos-clean gate is an exception, **not** a passed zero-error gate.
- M2/M4/M5 and replacement of the legacy forest selector: declined; excluded by the
  approved runtime-loading scope.
- More hardware, 2px or enabled-hysteresis benchmarks: declined for this stage;
  the measurement report states those limits and retains the measured binary hash.

## Post-fix verification

Post-fix Release build and full CTest: **61/61** (68.75s).
CI-shaped headless ASan/UBSan: **60/60** (244.40s); native boundary covered by Release.
Frozen Full images **15/15**, Visibility **1/1**, unchanged references.
Converter standalone **6/6** and CLI inspect passed; converter sources unchanged.

Five post-fix Bistro controls passed with Vulkan validation clean. Far/near OFF/ON
images each exactly match an archived measured round; original-source OFF equals
cooked OFF. These controls are not a repeated performance matrix.
[Control results and binary hash](2026-10-21-object-lod/post-review-controls.json).

Saved final logs: [Release](2026-10-21-object-lod/final-release-tests.txt),
[ASan/UBSan](2026-10-21-object-lod/final-asan-tests.txt),
[Full images](2026-10-21-object-lod/final-render-full.txt),
[Visibility](2026-10-21-object-lod/final-render-visibility.txt).