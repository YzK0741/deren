# Subtilis M1b upload implementation plan

> Execute inline with superpowers:executing-plans, TDD and one fresh final review. Continue the user's existing checkout workflow; no unsolicited push.

**Spec:** ../specs/2026-10-20-subtilis-m1b-upload-design.md
**Goal:** Upload one immutable CPU/GPU geometry generation, then expose an explicit primitive/runtime creation path.
**Architecture:** Existing pass-chain and scene buffer requests; source identity retained as dependency; no RHI ABI or render graph changes.
**Tech:** C++23 modules, utility::vessel, existing RHI/pass resources and probe backend.

## Constraints

- Baseline 813de5a7; preserve user untracked files. Current checkout; local commits only.
- One CMake build at a time, -j12. Check 5h usage at milestones, <=5% remaining checkpoint and pause.
- Full uint32 index payload uploaded; ordinary draws/BLAS retain LOD0 count.
- No auto-build at import, per-frame hierarchy selection, shader ABI, or visual/performance claims.

## Task 1: immutable upload generation

- [x] Add test_subtilis_upload and observe missing module RED. Test full bytes, allocation failure and source lifetime.
- [x] Add public uploaded_geometry and upload_geometry, implementation using existing pass-chain/resource_storage_pass. Retain immutable source and complete buffer generations, checking sizes before publication.
- [x] Register CMake/CI/test/tidy entries. Run upload + CPU Subtilis tests GREEN; commit.

## Task 2: primitive/runtime explicit source

- [x] Add failed-first binding/destroy tests and runtime creation coverage using the portable probe.
- [x] Add optional primitive_create_info::subtilis_geometry; immutable primitive generation member and bind helper. Create from source-derived bytes and counts; original null-source upload unchanged. Destroy drops views and generation before resource owner.
- [x] Verify two generations and old leases; run complete Release + relevant ASan/UBSan + new module tidy. Fresh whole-change review, fix important issues with RED/GREEN; document evidence and next selector entry; commit.

## Review focus

- GPU bytes, CPU hierarchy, index width/count and source identity cannot be mixed between generations.
- Failed second allocation must not publish or alter a prior generation; destruction and external leases must not form a cycle.
- Full payload size is distinct from LOD0 draw count; BLAS/shadow use LOD0.
- New explicit source is authoritative; null preserves existing creation. No automatic LOD enablement.
