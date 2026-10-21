# HZB Phase C Camera Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans inline. One fresh whole-change reviewer after implementation; no per-task agents.

**Goal:** Retain correct visibility while a camera moves by recovering history-occluded meshlets against the current frame's first-pass HZB.
**Architecture:** cull(history) → raster#1(CLEAR) → build(current depth) → recovery(append stable records) → raster#2(LOAD) → resolve. Keep the original frozen per-item plan, prefix IDs and static B2 path; only the recovery option relaxes camera gating.
**Tech Stack:** C++23 modules, existing RHI/Boost render graph, Slang, existing indirect-buffer heap alias.
**Spec:** docs/superpowers/specs/2026-10-20-hzb-occlusion-design.md §§4–11,15–16. Baseline4db78b04. This is the previously approved phase C; user explicitly requested implementation in this session.

## Global Constraints

- No new RHI virtual/API or separate command allocation. Four65536-command runs per slot; existing visibility run2 unchanged, recovery run3.
- One preparation/candidate selection per frame, serial replay for second draw. No second CPU selection/publication or host count overwrite.
- Count fallback UINT32_MAX preserves complete original first draw and zero recovery. Recovery header invalid/unusable means recover every rejected candidate; never invalidate/reinterpret the first-pass prefix.
- All cross-pass synchronization comes from graph scopes. Recovery results/survivors are read-write to preserve flags/prefix; second attachments read-write LOAD.
- DEREN_HZB_RECOVERY requires BUILD+CULL; default OFF; without it B2 static gate remains byte-exact. Explicitly opt in with env gate as existing HZB phases (product config remains phase D).
- Motion cost limits configurable via env: MAX_TRANSLATION_RATIO=.05 of scene radius, MAX_ROTATION_DEGREES=5, MAX_PROJECTION_DELTA=.01, all prefixed DEREN_HZB_RECOVERY_. Compare current vs saved history-slot camera (strict conservative cost gate, not correctness). Invalid/nonfinite inputs or over-limit changes use full single-pass draw. Recovery itself guarantees correctness.
- Current checkout authorized, preserve user untracked files; builds one at a time -j12; local commits/no push; archive bulky artifacts on D. Checkpoint+pause if5h quota remaining<=5%.

## Review Focus

1. Original vs recovered IDs: append after first_count and include that offset in packed meshlet ID, preserving resolve source; test stable prefix/order/count/boundaries.
2. First cull sentinel and recovery invalid header: full geometry survives; tests must preserve original commands, prefix and zero second command when rejected.
3. Graph cull→first raster→current build→recovery→LOAD raster→resolve order and scopes, including depth sampled→depth-write→sampled and UAV prefix preservation; compile actual production declarations in headless test.
4. Warm-up/resize/missing recovery pipeline, fast motion/projection discontinuity and TAA jitter: only ready full chain relaxes gate, option-off stays B2; finite rigid-camera motion tests + hardware capture.
5. Probe history snapshot must be frozen before current build overwrites saved snapshot; current-depth recovery reference/command prefix/suffix and explicit GPU→host publication must verify real GPU results, not only pixels.

## Task1 — CPU and graph contracts (RED→GREEN)

Files: visibility/hzb.cppm, tests/test_hzb.cpp, render_graph/visibility.cppm, tests/test_render_graph_visibility.cpp, pass/visibility_raster.cppm/.cpp, tests/test_visibility_raster_pass.cpp, pass/scene_tables.cppm, tests/test_runtime_injection.cpp.

- [x] Add failing tests for4run command lane offsets, recovery prefix preserved/zero/all/sparse/malformed/capacity atomic refusal, rigid-camera translation/rotation/projection gates with NaN/bad limits.
- [x] Implement hzb_recovery_command_word_offset(slot,item), hzb_compact_recovery(candidate_ids,flags,keep_current,first_count,output) returning optional<hzb_recovery_compaction>{retested,recovered}, hzb_recovery_gate(previous,current,scene_radius,limits). Motion arithmetic uses finite rigid view matrices and derives camera positions; cost gate only, unchanged byte-exact hzb_scene_gate.
- [x] Move existing HZB build/cull graph declarations into visibility adapter and add recovery_parameters (levels/header/items/candidates/models reads; results/survivors/commands RW), append_hzb_recovery. Add recovery_parameters for LOAD attachments under distinct name (raster_load_parameters / append_recovery).
- [x] Compile full six-node chain test with shared table identities, assert ordered memory dependencies and preserved attachment load/write intent. Existing append remains discard-write. Add visibility_raster_pass::set_load_existing(bool), test accessor/default. Fourrun scene_tables allocation regression.
- [x] Build targeted tests, confirm RED then GREEN; no new giant graph subsystem.

## Task2 — GPU and runtime two-pass chain (RED→GREEN)

Files: shaders/common/hzb.slang, shaders/scene/hzb_cull.slang, shaders/scene/visibility.slang; pass/hzb_cull.cppm/.cpp; runtime/runtime.declarations.cppm, runtime.cpp, runtime.frames.cppm, runtime.constructor.cppm, runtime.post_graph.cpp.

- [x] Cull push adds mode (set_frame(..., recovery=false)); recovery enumerates only flags1 and preserves flags, retests using current header/pyramid, serial ascending appends after first_count, writes pass2_count and full3word recovery command. Keep-all for unusable current header. Original pass writes pass2_count0; zero/unsupported/rejected second commands initialized0. No pass1 rewrites in recovery.
- [x] Shader pad=1 chooses recovery dispatch and adds pass1_count to fetch AND packed ID. Resolve remains unchanged; per-item metadata stays frozen.
- [x] Runtime uses distinct ready cull-pass and LOAD raster instances, retains replay{block,cull,item_index} of the first frame's selected HZB items, records replay against run3 with pad1. Never recompute candidates or overwrite original draw rows during recovery.
- [x] Admission requires full ready recovery chain before relaxing camera gate; parse finite positive env limits once, apply CPU motion helper. Warm-up stays full draw/build, valid history enables prediction; old B2 gate without recovery.
- [x] Reorder active C graph build before recovery/resolve; append new lanes23/24 only. Add positionally truthful build/recovery/second-raster GPU marks, fill pruned marks before scene resolve for OFF/non-C paths. Preserve non-C/A2 build schedule.
- [x] Add recovery counters (retested/recovered/still-occluded) and session log. Shader compilation + release headless suite.

## Task3 — probes, real GPU acceptance and review

Files: runtime probes/decls/runtime.cpp; scripts/measure/hzb_cull_verify.py; docs/reviews/2026-10-21-hzb-recovery.md and performance evidence; existing capture harness only if necessary.

- [x] Probe freezes expected history view/proj BEFORE build changes snapshot; copy final survivor table after recovery (not only first cull), current pyramid/header, pass2_count and run3 commands, preserving old B2 probe version. Publish all directly read GPU tables to host.
- [x] In-engine CPU hzb_test_sphere+hzb_compact_recovery over current HZB validates every rejected candidate decision and suffix record. Python checks flags/counts/prefix/suffix/commands/stats and engine reference metadata, supports legacy dumps.
- [x] Release full CTest; ASan relevant headless tests; targeted tidy (document known optional-access analyzer crash and backend DLL sanitizer link limit). No redundant tests that merely copy implementation.
- [x] Bistro static regression OFF/B2/C, moving yaw and disocclusion (nonzero recovered witness), rapid-motion cost gate, TAA jitter and invalid-header/fallback injection if needed. Compare final depth where equal-depth color ties differ; C1<=.05% differing pixels, no missing geometry, tie depth<=1ulp; account known OFF-vs-OFF near2variant jitter using controls. If no true pixel correctness, fix before acceptance.
- [x] Interleaved>=3 OFF/ON130frame launches per moving view, report total GPU plus build/recovery/raster costs and CPU post. Default remainsOFF even if accepted.
- [ ] One fresh read-only whole-change reviewer on gpt-6-astra high per executing-plans; Critical/Important one RED→GREEN fix pass+green suite; Minor durable ledger. No second review cycle. Save local commit and durable progress, archive verified artifacts D, remove only task scratch. Next phase D remains separate.

Self-review: tasks cover C requirements and deliberately preserve B2's narrower host-cut input scope; shared/import-time candidates remain old path. Motion limits are configurable capture-phase environment settings rather than premature GUI/config expansion. Recovery uses existing candidate bound/extent checks, same conservative sphere reference and same draw-item IDs.

## Status (2026-10-21 finish pass)

Tasks 1–2 and every Task3 verification item are complete. The one Important review finding (verify-script stale-tail truncation) is fixed with RED→GREEN and normal/legacy/corrupt-command control evidence; the static-C same-variant colour/depth comparison is exact (0 differing pixels / 0 differing depth words; the other already-known variant's 2,901 px/max10 jitter is reported separately, not waived); and the 3-round interleaved moving-view OFF/recovery measurement is recorded in `docs/reviews/2026-10-21-hzb-recovery.md` (GPU frame total median near −78.6%, far −61.6%; CPU post+scene median delta +1.23/+0.77 ms, reported honestly as above the 0.2 ms design target, which is scoped outside `record_visibility_items` and is left as a phase-D profiling item). The only remaining item is the unchecked last Task3 line: the Lead's local commit and the verified D-drive archive — this finish pass performed no git mutations. Default remains OFF; phase D (product config/GUI and broader item sources) remains separate.

