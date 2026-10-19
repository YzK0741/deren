# Subtilis M1b foundation Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans inline, TDD and one fresh final review. Continue the user-approved Subtilis design on HEAD feb66065 without reverting the user's seven follow-up commits.

**Goal:** Establish a validated replacement-group asset and deterministic CPU cut/publication contract for the next real-LOD builder and upload integration.
**Architecture:** Pure CPU modules own immutable validated group forests and cut selection. Groups are stored bottom-up, each has a complete coarse meshlet range and either zero or a complete contiguous child group range. Roots cover every group once. Existing M1a visible-run metadata/publication remains the consumer protocol.
**Tech Stack:** C++23 modules, utility::vessel, existing 48-byte meshlets; no RHI or new external dependency in this foundation.
**Spec:** ../specs/2026-10-18-subtilis-design.md §11; this foundation precedes meshoptimizer construction/serialization and runtime real-LOD upload.

## Constraints

- Current checkout; preserve .vscode and the old untracked CI checkpoint. After local gates, follow the user's prior commit/CI request and latest instruction: finish remote CI, record the result, then pause without further implementation.
- Poll 300-minute quota at milestones; remaining <=5% pauses work and writes progress.
- One build at a time, -j12. CI diagnosis can use isolated scratch only.
- M1a default/OFF behavior and GPU layout stay unchanged. Current runtime continues LOD0 until validated built assets are uploaded.
- CPU model is not a GPU/disk ABI. Avoid speculative streaming, DAG, software raster, or history formats.

## Task 0: CI sanitizer link repair

- [x] Reproduce imported-BMI container annotation calls surviving -fno-sanitize=all.
- [x] Apply libc++'s supported container annotation opt-out uniformly to MinGW/Clang BMIs/consumers, retaining compiler ASan/UBSan and DLL runtime policy.
- [x] Verify DLL link/load plus intentional ASan and UBSan failures; rebuild and test actual sanitizer targets. Recheck current clang-tidy gate.

## Task 1: Validated immutable asset

Files: geometry/subtilis_asset.cppm, tests/test_subtilis_asset.cpp, CMake/CI registrations.
Interfaces: cluster_range{first,count}; group_record{coarse,children,error}; asset_description{clusters,groups,roots,lod0_index_count,total_index_count}; asset::make -> expected<asset,asset_error>; const node/cluster/root views; lod0_count and maximum_cut.
- [x] RED: reject out-of-bounds/overlapping/unowned record ranges, repeated/forward child edges, orphan/duplicate roots, invalid/nonmonotone errors, bad index windows and leaf coverage gaps; empty forest valid.
- [x] Validate bottom-up unique-parent forest without recursion. Leaves exactly cover the normalized LOD0 index prefix with base_vertex=0. All source and coarse records are sound; canonical bounds conservatively enclose own records and descendants.
- [x] Compute B(node)=max(coarse.count,sum B(children)); reject a forest exceeding its LOD0 reservation/14-bit item capacity. Own copied data, expose no mutable spans.
- [x] GREEN and independence tests (input lifetime, deterministic bounds/order, deep tree without stack recursion).

## Task 2: Conservative CPU cut and publication

Files: geometry/subtilis_lod.cppm, geometry/subtilis.cppm common checks, tests/test_subtilis_lod.cpp.
Interfaces: view_parameters{model,view,projection,width,height,near_plane,target_error_px,projection_kind,force_finest}; select_cut(asset,view,capacity,scratch)->expected<cut_statistics,cut_error>; prepare_hierarchy(asset,reservation,view,scratch,published,writer)->visible_run.
- [x] RED: parent/children exclusive replacement, full-child refinement, deterministic order, multiple roots, cull-to-empty, exact budget and overflow without partial output.
- [x] Model/view affine transform norm uses Frobenius upper bound; perspective error uses nearest sphere depth, orthographic does not divide by depth. Near-plane crossing and invalid transforms/view force all finest records. This is a quality metric, not a strict final pixel/Hausdorff guarantee.
- [x] Explicit force_finest mode supports MASK/toon/shadow policy in later integration. No hysteresis: this is the deterministic reference for future selectors.
- [x] Reuse publication guard and full shared LOD0 fallback for source mismatch, capacity/overlap/write failure. Validate all complete records before returning visible metadata.
- [x] GREEN tests, including shear/nonuniform scale, near plane, invalid values, translated views and publication failure/alias safety.

## Task 3: Integration verification and handoff

- [x] Full Release tests, relevant ASan/UBSan; CI flag/source changes scoped to the diagnosed failure. Existing M1a tests stay green.
- [x] Fresh review and fixes, then update design/evidence and exact next entry.
- [x] Report scope honestly: asset validation + reference selector completed; actual meshoptimizer builder, asset serialization and runtime LOD geometry upload remain next.

## Review focus

- A forest with some but not all root coverage must fail, rather than silently drop surfaces.
- Parent error/bounds must include descendants; a large coarse run can exceed LOD0 even while reducing triangles.
- Bad input must return a typed error before partial asset/publication escapes.
- Double-precision bound construction and fail-open culling prevent underestimation after rounding.
- No unbounded recursive traversal, per-frame validation, new RHI ABI, or stale writable views.

## Ledger

Baseline: user commits 2b296a08..feb66065 preserve timing changes and source hygiene. Last pushed c5df0913 CI: Release/test/Debug passed, clang-tidy failed; ASan link failed on __sanitizer_annotate_contiguous_container. An isolated agent reproduced and diagnosed the latter; root integrates sequentially.
Ruling: deliver M1b's asset/cut foundation before coupling the offline simplifier and runtime upload — this fixes the data/coverage/error contract first and avoids introducing an unvalidated asset format or half-wired mixed-LOD rendering.
Ruling: bottom-up child indices (children before parent) make cycle rejection and bounds/budget construction iterative and simple; a future disk decoder must normalize to this CPU order.

Verification: full Release build + 53/53 tests; fresh ASan/UBSan build + 7/7 relevant tests, including real probe DLL loading. Independent foundation review and subsequent ownership-move review found no actionable issues. Exact CI tidy gate (31 files, warnings-as-errors) passed after adding the three Subtilis modules and removing the stale, deleted better_pmr.cpp entry. Evidence: ../../reviews/2026-10-20-subtilis-m1b-foundation.md.

User steering: finish CI, then pause. Do not proceed into meshoptimizer/builder/runtime upload after CI.
