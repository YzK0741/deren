# Subtilis M1a Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans in this session, with one fresh final review. The user approved §11's M1a route and requested pausing/recording when remaining 5-hour usage reaches 5%.

**Goal:** Publish a conservative CPU-selected LOD0 run consistently to visibility raster, resolve and indirect dispatch.

**Architecture:** A pure CPU geometry/subtilis module owns reservation checks, conservative sphere/frustum filtering and complete-publication fallback. Runtime freezes the switch with visibility admission, uses the existing per-frame culled table, and composes all consumer metadata from one published run. No hierarchy asset format, simplification or GPU selection in M1a.

**Tech Stack:** C++23 modules, utility::vessel, Slang, existing RHI/visibility graph.

**Spec:** ../specs/2026-10-18-subtilis-design.md §11.

## Global Constraints

- Current checkout retains the approved preceding fixes. No commits/push; build with -j12, one build at a time.
- Subtilis defaults false; only applies when visibility_buffer is requested and the whole frame is admitted. OFF has unchanged pixels.
- Keep current 32-bit visibility ID and 48-byte meshlet; meshlet IDs are visible-run ordinals. Empty selection is successful, failed publication returns full shared LOD0.
- Validate per-item reservation, global 65536 table budget, and 16384 records per item before writing. Complete records precede metadata publication.
- Reserve published intervals for this frame; overlapping aliases fall back without writing. While M1a is active, other camera host-cull writers are disabled, retaining their full LOD0 dispatch so later toon recording cannot overwrite this table. Shadows/BLAS stay unchanged.
- Poll 300-minute usage at task boundaries and during sustained work; remaining <=5% means write checkpoint, pause and report.

## Review Focus

- Nonuniform scale/shear, boundary-touching spheres and nonfinite transforms must not discard visible geometry.
- Empty selection must produce zero dispatch and no stale pixels, distinct from a failed write.
- Overflow, missing source rows or aliasing cannot write a neighboring item's range or publish partial selection.
- Raster bit0, resolve source/base and indirect count must agree; snapshot frame_slot must remain the same.
- Whole-frame admission, MASK handling and toon exact-depth re-shading retain their existing behavior.

### Task 1: CPU LOD0 visible-run publication

**Files:** Create source/engine/geometry/subtilis.cppm and source/tests/test_subtilis.cpp; register module/test in CMakeLists.txt and CI list.
**Interfaces:** reservation{base,count}, visible_run{base,count,source,reason}, prepare_lod0(records,reservation,clip_from_object,scratch,published,publish). Source shared/visible; failures capacity/source_mismatch/overlap/publication_failed. Writer receives base and span<const meshlet>, returns bool.
- [x] Write CPU tests: stable survivor order, zero result, capacity edges, ID limit, missing records, alias refusal without writer calls, failed writer returns full source metadata; conservative planes under shear/scale and nonfinite bounds/matrix.
- [x] Observe missing API failure, implement minimal pure selector using double plane equations and fail-open invalid bounds, run test green.

### Task 2: Config and visibility consumers

**Files:** application_configuration module/parser, fixtures, make_config.py, config.example.toml, app/main.cpp; runtime declarations/cpp/frames; visibility.slang; tests config/visibility/runtime.
**Interfaces:** runtime::set_subtilis(bool), subtilis_enabled(); frozen active frame; metadata composed from Task 1 visible_run.
- [x] Add config/default/generator/runtime switch tests and observe failures.
- [x] Add default-off config + parser/generator/fixture/app wiring; runtime switch is separate from visibility_buffer.
- [x] Publish selected records before draw-item/push/indirect metadata. All three consume same source/base/count. Use jittered camera matrix for current-frame conservative culling.
- [x] Add raster culled-table branch; preserve resolve math. Skip other camera host-cull writers on admitted active frames. Log first-frame input/selected/fallback counts.
- [x] Build and run focused tests; check shader compilation and reflection.

### Task 3: Validation and final review

- [x] Full Release CTest, relevant ASan; device ON versus existing visibility reference, OFF against checked-in recent legacy hashes. Use explicit recent references; built-in frozen set is known stale.
- [x] Capture MASK/toon/admission fallback and forced publication/capacity failure (CPU tests); no new GPU validation/recording errors.
- [x] Fresh review; address concrete findings, update roadmap/spec/checkpoint with exact evidence and remaining scope.

## Execution ledger

Ruling: keep the current checkout — it carries the preceding approved uncommitted fixes and existing builds; do not create a second checkout or discard them.
Ruling: M1a uses the existing reservation only; no hierarchy or geometric error API until M1b. Plane tests are in object space so nonuniform scale/shear need no guessed scalar radius.
Ruling: prevent shared-table overwrite using frozen per-frame writer exclusion and per-item alias fallback, rather than adding a new descriptor/ABI for this milestone.
Pre-flight: Task 2 consumes Task 1's source/base/count as one published run; Task 3 validates that same tuple against raster and resolve. No independent metadata decisions permitted.
Baseline: prior work completed Release 50/50, ASan eight relevant tests, device GUI/RT checks; dirty files listed in previous review record remain intentional.

Task 1: complete — missing-module RED observed, then 25 CPU checks passed (`subtilis-cpu-green-build.log`). Metadata consumer agreement assertions added in Task 2.
Task 2: in progress — missing config/runtime/helper API RED recorded (`subtilis-wiring-red-build.log`); first wiring build exposed a static callback using this instead of self, corrected.
Usage checkpoint: 300-minute window used 69%, remaining 31%; threshold remains remaining <=5%.

Task 2: complete — focused Release 3/3, actual generator defaults confirmed, full Release 51/51 before final adapter change. CPU metadata test now 29 checks.
Task 3: in progress — fresh review completed; culled-buffer graph declaration omission fixed (74 graph checks), final device/ASan/Release runs underway. Preliminary A/B face/body+TAA/MASK/deformation pairs are byte-identical.
Ruling: add optional culled-table graph reads for both consumers — the new shader branch introduces a real resource read even though host publication itself is outside the GPU graph.
Usage checkpoint: used 79%, remaining 21%; pause at remaining <=5%.

Final CPU/graph verification: Release build succeeded; final Release CTest 51/51. ASan initial six relevant tests and final changed CPU/runtime/graph targets (3/3) all passed, seven distinct tests. Final admitted visibility reference 1/1 and four A/B pairs passed. OFF full gate still running; do not mark Task 3 complete until its explicit recent-reference comparison is read.
Usage checkpoint: used 84%, remaining 16%; no threshold pause triggered.

Task 3: complete — final OFF full gate matched the committed recent reference 15/15 with zero mismatches; visibility ON 1/1, four final A/B pairs, Release 51/51 and seven distinct relevant ASan tests passed. Fresh review plus targeted graph-input re-check completed. No reference updates or commits.
Resume point: M1a is complete; the next milestone is an M1b asset/builder implementation plan (fixed dependency version, replacement-group tree, boundary preservation and worst-cut budget). Do not repeat M1a or assume true hierarchical LOD already exists. Preserve the preceding approved fixes in this worktree.

Final usage checkpoint: 5-hour (300-minute) window used 86%, remaining **14%**. M1a completed before the remaining-5% pause threshold. On continuation, keep polling and pause/write a checkpoint at remaining <=5%; do not automatically redeem rate-limit reset credits.
