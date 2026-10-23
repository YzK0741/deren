# M4 software-raster / GPU-routing checkpoint - final fresh review

Status: final fresh read-only whole-change review (Task 4 of
`docs/superpowers/plans/2026-10-23-software-routing.md`). Reviewer: hzb-architect.
Base HEAD: `d976ba133f3d2b20ac79e8a5a0b0afaab5fd6900` (short `d976ba13`), worktree clean at
review start except pre-existing untracked user files. No code, CMake, git, or build changes were made;
this report is the only file written. M4 is NOT accepted by this review - the Lead decides.

Range reviewed: `4e3e25f6..d976ba13` (4 commits):
`4e3e25f6` M4 Task 2 scaffolding (CSR tiles, default OFF), `28704969` opt-in software raster
producers + HZB composition checkpoint, `bac2a9b3` experiment records + textured acceptance docs,
`74d185c0` evidence archive, `d976ba13` GPU survivor routing + evidence-integrity hardening.

## Verdict (short)

- Critical findings: **0**.
- Important findings: **4** (I1-I4 below).
- Minor findings: **12** (M1-M12 below).
- The documented **negative-result verdict is CONFIRMED**: on the same build the GPU-routing
  matrix has no stable whole-frame net win over software-off DAG+HZB, CPU-active cost remains
  higher, and the corrected textured matrix shows software raster is a net GPU cost on that scene.
- Blocking condition for recording: **I1 (stale 26x/software-win claim in docs/subtilis.md) must be
  fixed** before the checkpoint is recorded; I4 should be closed or explicitly waived. No
  Critical/code-level blocker was found for default-OFF safety or the fallback contracts.
- Recommendation: the checkpoint may be recorded as **"experimental / opt-in, default-off, not
  accepted for shipping"** once I1 is corrected (and the residual limitations below are carried
  into the record). It must not be recorded as accepted or shipping-ready M4.

## Scope and independent reproduction

Read: all docs/reviews/specs/plans named in task-17; the full `4e3e25f6..d976ba13` diff for
runtime/pass/graph/RHI/geometry/shader/test/CMake files; the archived raw evidence under
`D:/deren-assets/2026-10-23-software-raster/`.

Independently reproduced (observed, not trusted from the reports):

| Check | Command / artifact | Result |
| --- | --- | --- |
| HEAD/worktree | `git rev-parse HEAD`, `git status --short` | `d976ba13...`; only pre-existing untracked user files at start |
| Release CTest | `ctest --test-dir build-release-dyn-clang64 --output-on-failure` | **68/68 passed** in 40.35 s |
| docs checks | `build-release-dyn-clang64/test_docs.exe` | **407 checks, 0 failed -> PASS** |
| Archive completeness | full SHA-256 re-hash of `gpu-routing-acceptance/sha256-manifest.json` (728 files) | 728 files / 1,055,832,427 B / **0 mismatches** |
| Build identity | archive `build/deren.exe`, `m4t3-build-identity.txt` | exe `443e0647...`, shaders `f7806b9d...`, asset `5b626f6f...` |
| Frozen gates | archive `logs/m4t3-gate-full.log`, `m4t3-gate-visibility.log` | **15/15** and **1/1** matched |
| Sanitizer suite | archive `logs/m4t3-asan-ctest.log` | **67/67 passed** (one fewer test than Release; see residual) |
| Converter | archive `logs/m4t3-converter-ctest.log` | **6/6 passed** |
| Final matrix | archive `matrix-final/results.json` | 18 runs, 3 rounds, 129 GPU samples/run; summary medians/spreads match the routing report; same exe/shaders/asset `complete:true` |
| Stage-2 matrix | `docs/reviews/2026-10-23-software-edge-results.json` | 30 runs; medians/spreads/route counts match the routing report |
| Textured matrix | `docs/reviews/2026-10-23-subtilis-textured-results.json` | 24 runs, 3 rounds, exit 0, 129 samples; summary matches the corrected report (DAG+HZB 10.44/5.64; all-enabled 12.12/6.32) |
| Both ring slots | `verify_software_images.py .../slot0/software --pixels 921600 --max-depth-ulp 8 --require-manifest --compare .../slot1/software` | PASS: 115,446 covered pixels, max 2 ULP, GPU depth/ID hashes identical |
| Recovery | same verifier on `captures-final/recovery-near/software` | PASS: 30 pixels / 2 ULP; route JSON: 94 software candidates, hardware primary 7,381, recovery 314, recovered visible 327 |
| Budget fall-open | verifier + route JSON on `budget0`, `budget1` | budget 0: 0 candidates, 6 hardware; budget 1: 1 candidate, 5 hardware, 7,879 pixels / 2 ULP |
| Closed-history fallback | verifier on `fallback/software`; SHA-256 compare with slot0 | PASS: 115,446 pixels / 2 ULP; GPU depth/ID hashes identical to the GPU-route capture |
| Oracle gate | `captures-final/oracle-gate*/` inventory + logs | no `-depth/-id/-cpu-*` files, no manifest, no PNG; log says "CPU oracle failed; GPU images not published" |
| Far-recovery exclusion | custom array diff of `captures-early/recovery-textured/recovery/software-*` | 106 covered pixels both sides; **1 pixel differs in ID** (pixel 594,512: GPU 2172126159 vs CPU 2172126102), depth bits equal (1065341343), other 105 same ID, max depth diff 3 ULP - matches the report's "1 pixel" exclusion |
| Validation smoke | archive logs + `validation_clean` rows | no `VUID-` / `Validation Error` in `captures-final` logs; smoke rows validation enabled and clean |

## Important findings

### I1. docs/subtilis.md still presents the withdrawn 5.22 ms / ~26x software-enabled win as current system gain

- Evidence: `docs/subtilis.md:199-209` (table "Bistro path comparison (2026-10-22...)" rows
  `Subtilis + HZB + software raster | 9.55 ms | 5.22 ms`, then "near-view comparison is
  132.93 / 5.22 = 25.5x, or about 26x ... software raster brings the measured GPU median from
  5.94 to 5.22 ms"). The same manual's top banner says the opposite:
  `docs/subtilis.md:30-33` - "Earlier software-enabled timings, including 5.22 ms / about 26x, are
  historical results awaiting remeasurement after these fixes; they are not accepted performance
  evidence for the corrected renderer." `README.md:119-122` says the same for the 5.22/26x result.
  The corrected three-round textured acceptance says adding software raster **increases** GPU time:
  `docs/reviews/2026-10-23-subtilis-textured-acceptance.md:9-16` and
  `docs/reviews/2026-10-23-subtilis-textured-results.json` summary: DAG+HZB 10.44/5.64 ms ->
  all-enabled 12.12/6.32 ms far/near. The routing checkpoint also states no stable net win and M4
  not accepted (`docs/reviews/2026-10-23-software-routing.md:108-126`).
- Observed vs expected: expected one status for the withdrawn headline; observed both a withdrawn
  banner and an active 26x derivation that uses the withdrawn number. This is an unwithdrawn,
  unsupported net-win claim left in the user-facing manual, and it undermines evidence honesty for
  the checkpoint record.
- GREEN: replace the 2026-10-22 table/derivation with the same-build corrected matrix
  (10.44/5.64 DAG+HZB, 12.12/6.32 all-enabled, with build/asset identities), or mark the whole
  table and the 26x sentence as "historical, superseded, NOT accepted evidence" with the archive
  pointer; remove the "current build" phrasing that uses the old 21.17->18.73 numbers. Re-check
  `docs/reviews/2026-10-22-render-optimization-comparison.md` for the same 9.55/5.22 rows
  (lines 80-107) and add a superseded pointer.

### I2. Shared HZB path changed, but no M3-only (software off, HZB on) correctness comparison exists in this checkpoint

- Evidence: `source/shaders/scene/hzb_cull.slang:58-60` (`hzb_hardware_candidate`), `:108-165`
  (recovery compaction), `:234-256` (history-invalid path), `:276-298` (main compaction) all
  changed for the optional software lane; the same shader is the only HZB cull implementation and
  is used when software raster is OFF. The checkpoint acceptance tables capture only software
  modes: `docs/reviews/2026-10-23-software-routing.md:75-84` (slot0/slot1, budget, recovery,
  fallback, oracle gate, validation smokes) and `:93-113` (timing matrix). The matrix "hardware"
  mode does run DAG+HZB with software off, but only records all-run exit codes, `validation_clean`
  and timings; its screenshots
  (`.../matrix-final/hardware/*/screenshot_*.png`) are hashed, not compared against any pre-change
  reference. `check_render` frozen gates do not enable HZB.
- Failure mode if wrong: default-off M3 users (env-gated) would see changed culling/coverage with no
  evidence in this checkpoint. Code inspection found the non-software path logically equivalent
  (`pad == candidate_count` when software is off, so `hzb_hardware_candidate` and the flag
  normalization keep the old answer), and the matrix hardware rows are validation-clean; that
  lowers, but does not remove, the regression risk.
- GREEN: capture one M3-only frame (software_raster=false, `DEREN_HZB_BUILD/CULL/RECOVERY=1`) on the
  final build with validation enabled and compare it against a pre-change reference (e.g. rerun the
  M3 acceptance scene/camera on `4e3e25f6` or compare to the 2026-10-21 HZB acceptance images);
  record the hash in the routing checkpoint. Alternatively add a focused unit test to
  `test_hzb.cpp`/`test_visibility_software` for the pad==count and flag-normalization equivalence and
  state that shader-execution equivalence is inspected rather than captured.

### I3. T1 diagnostic probe remains in the production runtime and changes rendered frames session-wide when armed

- Evidence: `source/engine/runtime/runtime.cpp:2131-2174` arms the probe from
  `DEREN_SOFTWARE_RASTER_PROBE=workload`; `:2194` sets
  `software_classification_frame_active` for **every** frame while armed; `:2359-2413` classifies
  and, at `:2389-2400`, rewrites the run lane in split order and sets
  `plan.block.geometry.index_count = split.hardware_count` - i.e. hardware dispatch is reduced to
  the prefix on every frame, producing holes by design (the T1 "holes control"). The code comments
  call it capture-only and "never in a default frame", and the JSON dump is screenshot-frame only,
  but the rendering change is not limited to the screenshot frame.
- Observed vs expected: review focus 1 asks that diagnostic/capture env hooks not affect production
  frames; this hook can silently corrupt any run that inherits the env. Default runs (env unset) are
  unaffected, and the frozen gates pass, so default-OFF safety holds; the issue is shipping a
  developer measurement hook that mutates output frames rather than only artifacts.
- GREEN: remove the T1 workload-probe instrumentation now that T1 evidence is archived (the probe
  was a Task 1 measurement tool), or move it behind a developer/capture-only build/lane and
  explicitly document "this env renders holes" in the acceptance record. At minimum do not mention
  it as a capture-only hook in the plan while it changes all frames.

### I4. Corrected textured acceptance does not record the shader bundle identity it claims

- Evidence: `docs/reviews/2026-10-23-subtilis-textured-acceptance.md:7` says the "same textured
  compatibility asset, executable, shader bundle" were used for all configurations and 24 captures;
  `docs/reviews/2026-10-23-subtilis-textured-results.json` has top-level `asset`, `asset_sha256`,
  `executable_sha256`, `resolution`, `frames_per_round`, `rounds`, `summary`,
  `image_comparisons`, `runs` - it has **no** `shader_bundle_sha256` and no `complete` flag. The
  archived run directories (`D:/deren-assets/2026-10-23-software-raster/subtilis-textured-acceptance-resumed/*`)
  contain config/log/run.json but no shader hash; the runner script copies in the archive are tiny
  and record no bundle hash. Later matrices (edge, triangle, GPU routing) do record executable and
  shader hashes and re-check them at completion (`scripts/measure/measure_software_edges.py:18-19,40-41`,
  `capture_software_gpu_route.py:47-48,192`), so this is an inconsistent gap in the checkpoint's
  central corrected-acceptance artifact, not a project-wide limitation.
- Observed vs expected: expected the claimed same build identity to be auditable from the tracked
  JSON; observed executable-only identity. Low practical risk (all 24 runs share one exe and one
  `shaders_dir`), but the claim is not independently verifiable.
- GREEN: record `shader_bundle_sha256` and a `complete:true` flag in the textured results JSON (or
  in the report as a captured identity file), or downgrade the report wording to "same executable;
  shader bundle identity not recorded". If any shader changed during those 24 captures, the
  corrected matrix must be re-run.

## Minor findings

### M1. Env hook in the geometry upload path does work even when software raster is off

- Evidence: `source/engine/geometry/subtilis_upload.cpp:102-122` reads
  `DEREN_SOFTWARE_RASTER_EDGE`/`DEREN_SOFTWARE_RASTER_GPU` and, when either is `1`, computes and
  sorts one triangle-edge record per cluster during scene upload. This happens regardless of
  `[render] software_raster`, so an OFF run started with a leftover env var pays CPU/memory and
  stores metadata it never consumes.
- GREEN: pass an explicit runtime feature request into the upload builder (or gate on
  `software_raster_on`), not on env text; keep env only for feature selection at the runtime level.
  Severity Minor: no rendered-frame change; one-time startup cost only.

### M2. Current RHI ABI value in docs is stale after ABI 47/48

- Evidence: `source/promise/rhi/rhi.contract.cppm:420-426` sets `abi_version = 48` with ABI47/48
  notes; `docs/dynamic_runtime.md:26` says "abi_version (46 today)", `docs/mainpage.md:174` says
  "(46 today)", `docs/vessel.md:57` says "RHI ABI46". This change added two ABI versions without
  updating the current-value notes.
- GREEN: update all three to 48 and add the ABI47 (enabled buffer-int64 atomics) / ABI48 (compute
  indirect) history lines where the ABI list is maintained.

### M3. `git diff --check` fails on a whitespace error in the range

- Evidence: `git diff --check 4e3e25f6..d976ba13` -> `docs/reviews/2026-10-22-render-optimization-comparison.md:156: new blank line at EOF.` (exit 2).
- GREEN: strip the trailing blank line; run `git diff --check` in the final gate if the project
  wants that invariant enforced (CI does not currently appear to run it).

### M4. Archived verification log does not match the final archive manifest totals

- Evidence: `D:/deren-assets/2026-10-23-software-raster/gpu-routing-acceptance/logs/m4t3-archive-verify.log`
  says `files: 727  bytes: 1055832180 ... verify: missing 0, mismatched 0, extra 0`; the final
  `sha256-manifest.json` has `file_count: 728`, `total_bytes: 1055832427`, and the routing report
  says 728 files / 1,055,832,427 bytes. The reviewer's full re-hash of the final manifest found
  728/0 mismatches, so the manifest is good but the archived log is from an earlier archive state
  (the manifest itself is excluded from its own listing).
- GREEN: regenerate `m4t3-archive-verify.log` after the final manifest (and state "728 files
  excluding the manifest" in the report to remove the count ambiguity).

### M5. Final GPU-routing matrix raw results are only in the archive, not repo-tracked

- Evidence: `docs/reviews/2026-10-23-software-routing.md:93-113` publishes the Stage-3 same-build
  matrix, but the only raw `results.json` is
  `D:/deren-assets/2026-10-23-software-raster/gpu-routing-acceptance/matrix-final/results.json`.
  Stage 1 and Stage 2 each have a tracked JSON (`2026-10-23-software-direct-results.json`,
  `2026-10-23-software-edge-results.json`) and the report links those; the Stage-3 table has no
  tracked equivalent link.
- GREEN: add a small tracked summary JSON for the Stage-3 matrix (executable/shader/asset hashes,
  18 per-run rows: mode, camera, round, gpu_ms, cpu_active_ms, cut_wall_us, image hashes,
  medians/spreads) or link the exact archive file plus its manifest path in the report.

### M6. Historical M4 spec still claims HZB build runs after resolve/merge; implementation builds it before software coverage

- Evidence: `docs/superpowers/specs/2026-10-22-software-raster-design.md:5` (status note says the
  rest is a historical snapshot), but `:181` lists "HZB build ... in resolve after append", `:191`
  says "raster->software->resolve->merge->HZB build", and `:276` says HZB samples post-merge depth.
  The implementation builds HZB from the hardware depth **before** the software raster/merge:
  `source/engine/runtime/runtime.post_graph.cpp:966-972` ("Build from the completed hardware depth
  before optional software coverage ... conservative because omitted tiny occluders only reduce
  culling"), then appends software at `:1039-1056` and merge at `:1145-1149`. The implemented order
  is conservative and consistent with the routing design, but the historical spec's order claims
  were not updated in its status note.
- GREEN: add one line to the spec status note recording the actual order (hardware depth -> HZB
  build -> software raster -> resolve -> merge) and that omitting software occluders is intentional
  conservatism; no code change.

### M7. Old M4 plan still contains 30 unchecked boxes

- Evidence: `docs/superpowers/plans/2026-10-22-software-raster.md` line 15 marks the plan as
  superseded by the 2026-10-23 routing design, but `grep -c "- \\[ \\]"` = 30 open boxes (Tasks
  3-7 in the original tile-producer shape). A reader can mistake these for current work.
- GREEN: mark them not-applicable/superseded (or replace with pointers to the routing plan/reports)
  when the checkpoint docs are finalized.

### M8. Capability-refusal fallback for GPU routing is only device-free-tested

- Evidence: `source/tests/test_visibility_software_pass.cpp:54-67` tests that `prepare_triangles`/
  `prepare_gpu_route` refuse without a device/pipeline, and `prepare_gpu_route` checks
  `caps->compute_indirect()` + `prepare_triangles` + int64-atomics-gated triangle pipeline
  (`source/engine/pass/software_visibility.cpp:116-125`); the "capabilities unavailable -> CPU
  routing fallback" runtime path has no test that drives a capability-false
  `device_capabilities`/pass. The fallback claim rests on code inspection plus the closed-history
  fallback capture (which takes a different path).
- GREEN: add a headless unit test with a stub device/capabilities if the RHI test doubles allow it,
  or add a capture with a capability override if available; otherwise record "inspected, not
  exercised" in the residual limitations.

### M9. Manual banner overstates CPU/GPU image agreement for the recovery case

- Evidence: `docs/subtilis.md:1-8` says capture-only snapshots validate the partition and "software
  images match the CPU reference at 2 ULP"; the only nonzero-recovery image evidence is the near
  recovery capture (30 pixels / 2 ULP). The far textured recovery frame's partition is validated
  but its image comparison was excluded at one pixel
  (`docs/reviews/2026-10-23-software-routing.md:120-125`, reproduced above).
- GREEN: qualify the banner: "fixture and near-recovery software images match at 2 ULP; far
  recovery is partition-only evidence (1-pixel ID divergence excluded, see routing report)".
  Severity Minor: the detailed report is honest; the banner is over-broad.

### M10. README update banner points to a result that is no longer "below"

- Evidence: `README.md:119-122` says "The historical 5.22ms / approximately 26x software-enabled
  result below requires remeasurement..."; the features bullet at `README.md:171` no longer
  contains that 26x text (it carries corrected 10.44/12.12 numbers), so "below" is stale.
- GREEN: point the banner at the archived comparison report / manual section that still carries the
  historical claim, or drop "below".

### M11. Acceptance reports point at cleaned workspace raw prefixes

- Evidence: `docs/reviews/2026-10-23-software-routing.md:9` cites raw prefixes
  `tmp/sw-direct-tie-8/9`; `docs/reviews/2026-10-23-software-triangle-atomic.md:25` cites
  `tmp/sw-triangle-{8,9}`, `...-slope-...`, `...-tie-...`; those `tmp/` paths are no longer
  present in the workspace. The archived copies exist under
  `D:/deren-assets/2026-10-23-software-raster/` with different names (e.g.
  `sw-direct-tie-8-*`, `sw-triangle-*`), and the closeout checkpoint
  (`docs/reviews/2026-10-23-subtilis-closeout-checkpoint.md:8-13`) explains the cleanup.
- GREEN: replace the `tmp/...` pointers with the archive paths (or add both, marking `tmp/` as
  ephemeral), so the raw evidence is addressable from the reports.

### M12. Checkpoint report does not summarize the archived default-OFF/CI gate logs

- Evidence: the routing report's evidence sections (`:52-91`, `:115-125`) mention CTest 68/68 and
  validation smokes but do not cite the frozen Full 15/15 / Visibility 1/1, the ASan/UBSan 67/67,
  the converter 6/6, or `test_docs`; all four exist in
  `D:/deren-assets/2026-10-23-software-raster/gpu-routing-acceptance/logs/` (verified above) and
  the archive README lists them.
- GREEN: add a "gates" row/section to the routing checkpoint (or to the acceptance record) naming
  the archived log files and counts; keep the 68 vs 67 Release/ASan difference explained (one test
  absent from the archived ASan build: Release 68/68 contains `test_native_boundary_zero`, the archived ASan run 67/67 does not).

## Negative-result verdict: CONFIRMED

The checkpoint's negative-result claim is supported by the artifacts reviewed:

- Same-build Stage-3 matrix (archive `matrix-final/results.json`, 18 runs, exe `443e0647`,
  shaders `f7806b9d`): medians hardware 9.68/5.73 ms, GPU routing 9.48/5.31 ms far/near, with run
  spreads [9.47-10.41]/[5.55-6.12] vs [8.92-10.64]/[4.87-6.01] that overlap; CPU-active is higher
  for GPU routing (9.23/9.95 vs 8.85/8.66), and cut wall is still 0.16/0.30 ms above hardware.
  The GPU-routing median deltas (-0.20 ms far, -0.42 ms near) are inside the observed spread; the
  CPU-routing near mode also lands below hardware, so the medians do not establish a stable win.
- Stage-2 matrix (tracked `software-edge-results.json`) likewise shows far edge-1px 9.54 vs
  hardware 9.63 (0.09 ms) with larger spread, near 5.43 vs 5.20 (0.23 ms slower).
- Corrected textured matrix (tracked) shows software raster increases GPU 10.44/5.64 ->
  12.12/6.32 ms and raises CPU-active cost; the triangle/CPU-cost work improves the producer pass
  and CPU planning but does not reverse the whole-frame verdict.
- The historical 5.22 ms / ~26x software-enabled number is explicitly withdrawn by the corrected
  reports and must not be used as evidence (see I1).

Confirmation is about the *verdict*, not about the checkpoint being ready to record: I1 must be
fixed so the manual no longer contradicts that verdict.

## Checkpoint recording recommendation

- Record as **"experimental / opt-in, default-off, not accepted for shipping"** only after I1 is
  corrected (and I4 is closed or explicitly waived). No code blocker was found; default-OFF safety
  evidence is good (full re-hashed archive, frozen gates 15/15 + 1/1 in archived logs, Release
  68/68, ASan 67/67).
- Carry the residual limitations into the acceptance record (next section). Do not mark M4
  accepted, do not promote the software lane or GPU routing to default, and do not cite the
  withdrawn 5.22/26x number.

## Residual limitations to carry into the acceptance record

1. Full-stage (manifest/CSR/hardware/resolve/merge) verifier is not implemented. Capture-only
   evidence validates the software producer and hardware/software partition, not the resolve/merge
   chain; the reports state this, and the claim must stay narrowed.
2. Far textured recovery frame: partition validated with nonzero recovery counts, but the image
   comparison differs at one pixel (1 pixel, different ID, equal depth; reproduced above) and is
   excluded from image evidence. The cause is not root-caused (edge/tie difference between CPU
   reference and GPU raster); only the near recovery frame (30 pixels / 2 ULP) is image evidence.
3. CPU reference oracle requires complete retained geometry (every CSR item needs
   `subtilis_geometry`, HZB disabled for the CPU path); ordinary LOD0 fallback items cause an
   explicit oracle failure, so some textured frames are partition-only evidence.
4. No fixed GPU/CPU clocks; observed P8/210 MHz idle between runs; short 130-frame captures; run
   mean medians with large spreads; GPU matrix deltas are not stable. Single machine (RTX 4060
   Laptop, driver 617.42), one textured scene, two fixed cameras, 1280x720; not a release benchmark.
5. GPU routing requires HZB+triangle+enabled buffer-int64 atomics+compute indirect; the
   capability-false fallback is code-inspected and device-free refusal-tested, not exercised
   (M8). The closed-history fallback capture is a different path.
6. M3-only (software off, HZB on) shader behavior after the shared hzb_cull.slang changes lacks a
   dedicated reference comparison in this checkpoint (I2); the matrix hardware mode is
   validation-clean and timing-only.
7. T1 diagnostic instrumentation (I3) and the edge-metadata upload env hook (M1) remain in the
   runtime; they are env-only and do not affect defaults, but should be removed or documented as
   developer-only before any shipping claim.
8. 2.0 px holes control was not separately GPU-measured in T1 (same-build 2.0 px data is CPU-derived);
   the 1.0 px threshold is the measured setting.
9. Release CTest is 68/68; the archived ASan/UBSan run is 67/67; `test_native_boundary_zero` is present in the Release 68/68 list but absent from the archived ASan run, not explained in the checkpoint.
   Frozen gate logs exist and match, but were not independently re-run by this reviewer (Lead
   already verified them; the archived logs were inspected and the full archive re-hashed).
10. Raw workspace `tmp/` prefixes cited by earlier reports were cleaned; the durable archive uses
    different names (M11). Archive SHA-256 manifest itself is complete (728 files, excluding the
    manifest), independently re-hashed by this reviewer.

## Commands run (strongest reproductions)

```text
git rev-parse HEAD; git log --oneline -5; git status --short
git diff --stat 4e3e25f6..d976ba13; git diff --check 4e3e25f6..d976ba13
ctest --test-dir build-release-dyn-clang64 --output-on-failure
build-release-dyn-clang64/test_docs.exe
python - <<full SHA-256 re-hash of 728-file archive manifest>>
python scripts/measure/verify_software_images.py --self-test
python scripts/measure/verify_software_images.py \
  D:/deren-assets/2026-10-23-software-raster/gpu-routing-acceptance/captures-final/slot0/slot0/software \
  --pixels 921600 --max-depth-ulp 8 --require-manifest \
  --compare D:/deren-assets/2026-10-23-software-raster/gpu-routing-acceptance/captures-final/slot1/slot1/software
python scripts/measure/verify_software_images.py \
  D:/deren-assets/2026-10-23-software-raster/gpu-routing-acceptance/captures-final/recovery-near/recovery-near/software \
  --pixels 921600 --max-depth-ulp 8 --require-manifest
python scripts/measure/verify_software_images.py \
  D:/deren-assets/2026-10-23-software-raster/gpu-routing-acceptance/captures-final/budget1/budget1/software \
  --pixels 921600 --max-depth-ulp 8 --require-manifest
python scripts/measure/verify_software_images.py \
  D:/deren-assets/2026-10-23-software-raster/gpu-routing-acceptance/captures-final/fallback/fallback/software \
  --pixels 921600 --max-depth-ulp 8 --require-manifest
python -<<load matrix-final/results.json, software-edge-results.json, software-triangle-results.json,
  subtilis-textured-results.json; print hashes, medians, spreads, route counts, per-run completeness>>
python -<<custom diff of captures-early/recovery-textured/recovery/software-{depth,id} vs
  software-cpu-{depth,id}: covered counts, differing IDs, ULP>>
findstr /S /I /M /C:"VUID-" /C:"Validation Error" <archive>/captures-final/*.log
# archive inspection only (no GPU writes): m4t3-gate-full.log, m4t3-gate-visibility.log,
# m4t3-asan-ctest.log, m4t3-converter-ctest.log, m4t3-build-identity.txt, m4t3-acceptance-summary.json
```

No GPU capture was re-run by this reviewer (Lead had already re-run the frozen gates; the archived
gate logs were inspected instead, and no artifacts were written outside this report).

## Files/directories inspected (non-exhaustive)

- Docs: `docs/superpowers/{specs,plans}/2026-10-2[23]-software-*`, `docs/reviews/2026-10-2[23]-*`,
  `docs/subtilis.md`, `docs/visibility_buffer.md`, `README.md`, `docs/dynamic_runtime.md`,
  `docs/mainpage.md`, `docs/vessel.md`.
- Code: `source/engine/runtime/{runtime.cpp,runtime.post_graph.cpp,runtime.constructor.cppm,
  runtime.declarations.cppm,runtime.frames.cppm,runtime.readback.cppm,runtime.probes.cppm}`,
  `source/engine/visibility/visibility_software.cppm`,
  `source/engine/pass/{software_visibility,software_merge,hzb_cull,output_targets}.*`,
  `source/engine/render_graph/visibility.cppm`, `source/engine/render_layout/render_layout.cppm`,
  `source/engine/geometry/subtilis_upload.cpp`,
  `source/shaders/{common,scene}/{hzb_cull,software_visibility,software_visibility_atomic,
  software_route,software_candidate,software_merge,visibility_resolve,mesh_fetch,heap_slot_constants}.slang`,
  `source/promise/rhi/*`, `source/backends/vulkan/core/*`,
  `source/tests/{test_visibility_software,test_visibility_software_pass,test_render_graph_visibility,
  test_dynamic_link,test_render_resources,test_shader_sources,test_subtilis_*}.cpp`,
  `scripts/measure/{capture_software_gpu_route,measure_software_gpu,measure_software_edges,
  verify_software_images,verify_software_raster,make_software_raster_fixture}.py`.
- Evidence: `D:/deren-assets/2026-10-23-software-raster/gpu-routing-acceptance/` (manifest, logs,
  matrix-final, captures-final/early, fixture-repro, build/) and the tracked results JSONs under
  `docs/reviews/`.
