# Subtilis M1b selection implementation plan

> Execute inline with executing-plans/TDD and one fresh whole-change review, continuing the user's current-checkout workflow. No push unless requested.

**Spec:** ../specs/2026-10-20-subtilis-m1b-selection-design.md
**Baseline:** ebc87303.
**Goal:** Select one complete CPU hierarchy cut for eligible explicit-source main-view items and publish one coherent visibility run.
**Architecture:** Pure primitive/generation/view adapter plus a small branch in runtime::record_visibility_items; existing asset/selector/upload/graph APIs remain intact.
**Tech:** C++23 modules, vessel, existing portable probe and frame-slot publication.

## Constraints

- Preserve .vscode and untracked CI checkpoint files. Current checkout and local commits; no unsolicited push.
- One build at a time, -j12. 5h remaining <=5%: record exact state and pause, no further implementation.
- No shader/RHI/reservation changes, auto import build, or claims about final image/performance.
- Existing shadow/BLAS/geometry lane draw count stays LOD0.

## Task 1: pure generation and policy adapter

- [x] Retain normal_offset/uv_offset in build_statistics with failing-first metadata checks. Only64B/normal12/UV24 permits coarse; visibility admission rejects non64B stride, with pure admission regression. Update runtime facts in Task2.

- [x] Add tests first: valid upload matches leaf; swapped buffer/lease/type/count/stride/meshlet windows reject hierarchy; null source preserves old path. Observe missing adapter RED.
- [x] Add geometry/subtilis_selection.cppm and focused implementation if size warrants. Model policy facts explicitly (mask/blend, overlay, deforming, instanced, default pipeline, exact-depth redraw); derive force_finest without changing frame admission.
- [x] Build view_parameters from frozen matrices and render extent; test perspective/orthographic/invalid matrices, actual near extraction and nonuniform transforms. Invalid view gives complete finest.
- [x] Route supported identity to prepare_hierarchy, otherwise prepare_lod0/shared mismatch. Test no partial publication, count0, alias and capacity failure. Register headless CMake/CI and tidy; GREEN then commit.

## Task 2: actual runtime recording

- [x] Add portable runtime regressions for row/push/indirect count and source-table marker; observe old always-LOD0 behavior RED with a reducible uploaded grid.
- [x] Change record_visibility_items to build per-leaf policy/view, select exactly one run, keep existing apply_meshlet_run and publication ordering. character_forward active + !environment conservatively forces LOD0, regardless of toon_family.
- [x] Validate two frame slots, disabled and null-source old paths, MASK, exact-depth, invalid identity and publication failure. Retain existing culled-table graph ownership and host writer exclusion.
- [x] Full Release build/CTest, relevant sanitizer and new adapter tidy; fresh whole-change review, important fixes RED->GREEN, document exact evidence and remaining hardware acceptance; commit.

## Review focus

- A source pointer alone cannot prove association; primitive fields and leases are public.
- Exact-depth re-shading includes ordinary non-background opaque leaves, not only toon-tagged leaves.
- Current camera near/render extent and jitter must agree with the raster frame; invalid view must not guess.
- One selected run must govern raster, resolve and indirect, without exposing partial writes or cross-slot aliases.
- Forced finest/shared fallback must retain complete LOD0 and existing reservation limits.

Task 2 validation note: writer refusal is covered by the pure adapter; actual frames cover overlapping reservations. Environment coarse and ordinary-PBR character-forward finest are separate frame cases because the portable last-push probe also sees later character pushes of the same byte size. Combined hardware acceptance remains outstanding.
