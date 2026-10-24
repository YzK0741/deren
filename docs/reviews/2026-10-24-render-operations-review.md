# Deferred render operations - independent review (2026-10-24)

Status: final read-only review of the deferred render-operations checkpoint. Base HEAD:
`7f38b72021ba54eb138de926cf4e61945ee139eb` (implementation commit `c7382586`, RHI ABI 53).
No code, CMake or git changes were made; this review file is the only write. The worktree was
not clean: `docs/reviews/2026-10-24-render-operations.md` and its result directory are the
author's untracked artifacts, and `source/tests/test_render_operations.cpp` has an uncommitted
edit made outside this review (an explicit adjacent-duplicate-state case). Findings below are
against HEAD plus the untracked acceptance artifacts; the uncommitted test edit was not used as
evidence and does not change the verdict.

## Verdict: ACCEPTED WITH FOLLOW-UPS

- Critical findings: **0**.
- Important findings: **5** (I1-I5).
- Minor findings: **11** (M1-M11).
- The implementation behaves correctly on every exercised path, and the claimed gate results are
  reproducible: Release CTest 69/69, `test_docs` 419/0, clang-format-check PASS,
  check-backend-boundary PASS, frozen render gates 15/15 (`-Full`) and 1/1 (`-Visibility`), and
  the deferred-recording device probes all pass on this build.
- The plan's **baseline claim is not proven**: the optimized-vs-unoptimized control measures
  simplification against replay on the same deferred implementation, not deferral against the
  pre-deferral build. The author's acceptance report already states this limitation; the plan
  checkboxes and claim must be updated accordingly (I1).
- Before the acceptance report is finalized: fix the `docs/resource_groups.md` contradiction
  (I4), disposition the missing spec-required measurements (I2), and record the native-interop /
  post-ABI53 GUI evidence gap (I3) or narrow the claims. No functional defect requires a code
  change to accept the checkpoint as opt-in/default behavior, but the plan must stay partial
  until these are dispositioned.

## Claims independently verified

| Claim (acceptance report) | How checked | Result |
| --- | --- | --- |
| Release CTest 69/69 | `ctest --test-dir build-release-dyn-clang64 --output-on-failure` | PASS (40.30 s) |
| `test_docs` 419/0 | `build-release-dyn-clang64/test_docs.exe` | PASS |
| clang-format-check / check-backend-boundary | `cmake --build build-release-dyn-clang64 --target clang-format-check check-backend-boundary` | PASS |
| Frozen render gate full 15/15 and visibility 1/1 | `check_render.ps1 -Full -Compare ...legacy-hashes.txt`; `-Visibility -Compare ...vis-hashes.txt` | PASS (15/15, 1/1; hashes match the frozen references) |
| `test_render_operations` 271 checks | binary run | PASS |
| Device probes (resource-group/texture-group/submission/graph frame/gaussian GPU frame) | binaries with `--with-device` | PASS: 158, 95, 63, 12 and 65 checks respectively |
| Measurement table (requests/retained/setters/work, screenshots) | `docs/reviews/2026-10-24-render-operations/results.json`, the two debug logs, PNG SHA-256 | Matches: deferred 47/47/33/8 optimized vs 47/47/41/8 unoptimized; gaussian 56/55/40/11 vs 56/56/52/11; each arm's PNG hashes are identical and equal the frozen references (`FAE60C0B...`, `8555C829...`) |
| Stats definitions support the quoted counts | `core.api_core.cpp:5025,5179-5255`; `core.render_operations.cpp:43-48,153,202,284` | Definitions match: requested counts enqueues, retained counts applied batch size, setters count emitted native state calls, work counts draws/dispatches |
| Unrelated HEAD commit `7f38b720` | diff inspection; CTest + visibility gate | Only mesh-task indirect stride validation/test; no deferred-recording change; the visibility gate passes with it |

Not re-run by this reviewer (reported by the author and accepted as reported, or listed under
"unverifiable" below): the two measurement captures themselves, the GPU sort probe (its PASS line
is visible in both gaussian debug logs), and any long-duration GUI smoke.

## Post-review disposition (commit cae23b92)

- I4 is fixed: `docs/resource_groups.md` now documents ABI 53 queued group/push state, same-pipeline
  preservation, switch retention/republish and apply/end error surfacing; `docs/dynamic_runtime.md` ABI 53
  and ABI 52 wording updated.
- I5 is partially addressed: the CPU stream test gained independent cull/depth-write state coverage (275
  checks) and the acceptance report's residual-gap list was expanded.
- I1, I2 and I3 remain open follow-ups (pre-deferral baseline, CPU/allocation cost, native-interop/execute/
  post-ABI53 GUI smoke); they are not claimed as done. The plan is marked accepted-with-follow-ups with
  those boxes open.
- The findings above describe the tree at review time (HEAD 7f38b720) and are kept as the historical review
  record; the acceptance report carries the current disposition.

## Important findings

### I1. The optimized-vs-unoptimized control does not establish the plan's baseline claim

- Evidence: `docs/superpowers/plans/2026-10-24-render-operations.md:36-44` (Task 1 capture a
  baseline "before enabling deferral", Task 2 integration, Task 3 compare against the baseline);
  `docs/superpowers/specs/2026-10-24-render-operations-design.md:247-251` (measure with
  optimization on and off on the same workload); acceptance report
  `docs/reviews/2026-10-24-render-operations.md:21-26` and `:58-63` (the author explicitly says
  the control is simplification-vs-unoptimized on the same deferred stream, not a pre-deferral
  build, and that the original before-deferral baseline could not be reconstructed).
  `DEREN_RENDER_OPERATIONS_UNOPTIMIZED` only disables simplify/cache skipping in
  `source/backends/vulkan/core/core.render_operations.cpp:36`; both arms still use the new
  deferred stream, the new group/token lowering and the new native publication. The untracked
  `tmp/render-operations-baseline/` artifacts contain request counts only (no `lowered` line,
  no native setter count, no build identity).
- Observed vs expected: expected a comparison that supports "deferral+optimization records fewer
  native state changes than the previous immediate implementation". Observed control supports
  the narrower claim "on one deferred implementation, simplification removes 8/41 (deferred) and
  12/52 (gaussian) native setters while work counts and screenshots are unchanged". The broader
  claim remains unmeasured/unverifiable.
- GREEN: either (a) capture a pre-deferral build (`tmp/render-operations-before/` source snapshot,
  if it is genuinely pre-deferral) with the same stats counters and compare setters/work/captures,
  or (b) narrow Task 1/Task 3 wording and the acceptance report to the simplification claim, mark
  the baseline checkbox "not reconstructed" with the reason, and do not claim a deferral
  performance/recording-cost improvement. Record executable/shader hashes with both arms.

### I2. Spec-required measurement of CPU recording cost and allocation count is missing

- Evidence: `docs/superpowers/specs/2026-10-24-render-operations-design.md:247-251` requires
  measuring "CPU recording cost, allocation count, and native setter count with optimization on
  and off on the same workload". The acceptance report (`docs/reviews/2026-10-24-render-operations.md:21-41,58-69`)
  reports only requests/retained/native setters/work and screenshots; no CPU recording time and no
  allocation count is recorded. The report's limitations state no frame-time claim but do not
  disposition the missing CPU/allocation measurements.
- Impact: the checkpoint can show fewer native setters without knowing the added CPU/alloc cost;
  the spec explicitly warns that fewer setters alone do not establish a performance improvement.
- GREEN: measure CPU recording cost (rdtsc/steady clock around recording, optimized and
  unoptimized arms plus the pre-change build if available) and allocation count for both scenes,
  or explicitly move both measurements out of scope in the plan/spec update with rationale.

### I3. Native interop / execute / post-ABI53 GUI restore path is not exercised by tests or captures

- Evidence: `source/tests/test_resource_group.cpp:298-303` and
  `source/tests/test_texture_group.cpp:315-322` call `execute(span)` immediately after
  `begin_recording()` with an empty pending stream, and no consumer follows the execute; the
  `apply_operations()` + `finish_native()` invalidation and later restore path is therefore not
  tested. `source/tests/gpu_probe.cpp:71-94` records the radix sort on a clean stream (no pending
  RHI state before `prepare_native`, no consumer after `finish_native`). The GUI explicit-heap-bind
  evidence (`docs/reviews/2026-10-24-gaussian-gui.md:15-21`,
  `docs/reviews/2026-10-24-gaussian-gui-results.json`) was captured on 2026-10-23, before the
  ABI 53 commit (2026-10-24 19:56), and no post-ABI53 GUI smoke is recorded; the frozen captures do
  not enable the GUI.
- Observed vs expected: expected verification of the documented rule "immediate/native interop
  boundaries flush in order, then restore logical state for subsequent consumers"
  (`docs/superpowers/specs/2026-10-24-render-operations-design.md:161-164,239-245`). Observed only
  clean-stream coverage; the restore path after `execute`/`finish_native` and the GUI heap-bind
  position are code-inspected.
- GREEN: add a device probe: begin -> bind pipeline/push/viewport -> `execute(segment)` -> draw or
  readback that depends on the pre-execute state; add a `prepare_native`/`finish_native` case with
  pending push/group state and a consumer after the boundary; rerun the static GUI capture on the
  final ABI 53 build with validation enabled and record bright-pixel counts + validation result.

### I4. `docs/resource_groups.md` contradicts ABI 53 deferred semantics

- Evidence: `docs/resource_groups.md:77-79` still says "This version records immediately. Reload a
  compatible group after changing pipelines..." and `:79` says "Lazy state application and merging
  adjacent state updates remain subsequent work." That contradicts the implemented/specified
  behavior: `docs/superpowers/specs/2026-10-24-render-operations-design.md:123-143` (keep desired
  logical state; rebinding the same pipeline must not erase a loaded compatible group; retain the
  logical group across a switch and republish at the next consumer), and
  `source/backends/vulkan/core/core.render_operations.cpp:156-186,235-244` (group/token state is
  retained in `current_resources`/`current_textures` and republished at pipeline/group lowering).
  `docs/dynamic_runtime.md:81-91` (ABI 53 section) does not state that push constants and group
  loads are now queued state; ABI 52's "records immediately" line (`:103`) is historical but the
  ABI 53 section does not supersede it for push.
- Impact: this is the public contract documentation for resource groups; it tells callers to
  reload groups and to avoid lazy state even though ABI 53 preserves and republishes them. It is
  currently wrong for the shipped checkpoint.
- GREEN: update `docs/resource_groups.md` to ABI 53 semantics (pipeline/push/group operations are
  queued; same-pipeline rebind preserves a loaded compatible group; a switch retains the logical
  group and validates/republishes at the consumer; errors surface at apply/end) and add an ABI 53
  note to the unified-push section of `docs/dynamic_runtime.md` (the plan's Task 3 doc update is
  still unticked).

### I5. Spec-required correctness coverage is incomplete and the acceptance report understates it

- Evidence: spec `:239-245` requires coverage of "repeated draws, A/B/A state, consumers between
  setters, different state slots, push copying/full and partial overlap, pipeline/group token
  interactions, intermediate and final application, scope/timestamp/execute boundaries, native
  interop invalidation, stale identities, failure/reset, and group retention through completion".
  The CPU test (`source/tests/test_render_operations.cpp`) covers vertex/index/pipeline/
  draw_indexed plus push copy and partial/full overlap; it does not cover resource_group/
  texture_group/push liveness, viewport/scissor/cull/depth_write/depth_bias state keys,
  pipeline/group token interactions across a pipeline switch, execute/native boundaries, or
  address-recycling identities. The acceptance report's residual list
  (`docs/reviews/2026-10-24-render-operations.md:54-56`) names only "mid-window state beyond the
  resource-group apply-then-bind sequence" and "secondary/mergeable ordering under a failing
  implicit flush"; additional required cases above are uncovered (see Coverage gaps).
- GREEN: add the missing tests (see Coverage gaps) and expand the acceptance report's residual
  list; if the plan stays partial, mark these explicitly as "not covered, code-inspected only"
  rather than leaving the report claiming the spec verification list is satisfied.

## Minor findings

### M1. CPU probe backend does not implement equivalent recording/application semantics

- `docs/superpowers/specs/2026-10-24-render-operations-design.md:234` requires the CPU probe
  backend to have equivalent recording/application semantics; plan Task 2's
  "Update CPU probe recording semantics without test-only production switches" is unticked
  (`docs/superpowers/plans/2026-10-24-render-operations.md:53`). `source/tests/probe_backend.cpp:1837-1908`
  replays `simplify()` then clears operations, with no `saved_state` carry-forward, no group/
  texture replay (`default: break` at `:1901-1902`), no identity checks and no native boundaries.
- GREEN: implement the equivalent save/restore/validation semantics or document the probe as an
  ABI-surface double only and rely on the Vulkan device probes for contract semantics.

### M2. Test-only environment switches affect production behavior and are undocumented

- `source/backends/vulkan/core/core.render_operations.cpp:36` toggles simplification/native-cache
  skipping via `DEREN_RENDER_OPERATIONS_UNOPTIMIZED`; `source/backends/vulkan/core/core.api_core.cpp:5179-5180`
  emits per-frame stats via `DEREN_RENDER_OPERATION_STATS`. The spec allows an internal
  unsimplified replay path (`:250-251`), but the switches are undocumented in the ABI/manual docs
  and change backend behavior in a normal build. GREEN: document them as developer-only or gate
  them behind a debug/test build; note the measurement contexts in the acceptance report.

### M3. Measurement artifacts record no build identity

- `docs/reviews/2026-10-24-render-operations/results.json` has scenario counts, work/setter counts,
  split outcome and image hashes, but no executable/shader hash; the untracked baseline
  `tmp/render-operations-baseline/results.json` and `counts.json` likewise have no build identity.
  The acceptance report says "one Release build" (`:23-24`). GREEN: record executable and shader
  bundle SHA-256 in the results file; this is the project convention for the other measurement
  matrices.

### M4. Plan/spec/report status is inconsistent

- Plan `:13-17` says the implementation is present in the working tree and all Task 1-3 boxes are
  unticked because the final comparison/review/report are not complete; the acceptance report is
  now present (untracked, `:1-3`, `:78-85`) and the report's own status says review is pending.
  Spec `:4` still says "design only, implementation pending". GREEN: when the Lead updates the
  plan/report after this review, tick/annotate the boxes actually satisfied, mark the baseline
  checkbox as not reconstructed (I1), and update the spec status. Do not mark a box done for work
  that is only covered by untracked artifacts.

### M5. Pipeline-introduced push/token conflicts are reported only at the next consumer

- The spec says "If a later pipeline introduces a conflict, report an error"
  (`docs/superpowers/specs/2026-10-24-render-operations-design.md:136-143`). In
  `source/backends/vulkan/core/core.render_operations.cpp:85-148` the pipeline case
  (`:87-92`) validates only the pipeline identity; push ranges are checked against the new
  pipeline's reserved token offsets only when a unique consumer is validated (`:141-148`).
  If a pipeline with a conflicting token layout is bound after a user push with no consumer in
  the same batch, an intermediate application (`apply_operations()`) succeeds and the pipeline
  lowering pushes zero over the reserved native bytes (`:211-220`) before any later consumer
  errors. There is no observed rendering failure (the recording becomes non-executable at the
  later consumer), but the specified immediate error is missing. GREEN: validate the current live
  push ranges against the new pipeline's reserved offsets in the pipeline case as well, or record
  this deferred-at-consumer choice in the spec/plan.

### M6. Final/restore application replays saved logical state at `end_recording`

- `source/backends/vulkan/core/core.render_operations.cpp:38-47` replays `saved_state` whenever
  `restore_state` is set (after `finish_native`), and `:322-332` recompacts it; for a final
  application this can re-emit state setters after the last consumer even though the final
  simplification (`live = 0`) would have dropped trailing state. Correctness is unaffected (the
  state commands are legal and precede command-buffer end), but it can add native setters at end
  frames and skew setter counts if a native boundary occurred. GREEN: for `final_application`,
  use final liveness when replaying saved state (or document the extra re-establishment).

### M7. GUI plugin leaves the segment recording if `prepare_native()` fails

- `source/backends/vulkan/graphical_user_interface/gui_dll.cpp:287-289` returns an empty handle
  when `prepare_native()` fails without calling `end_recording()`; the adjacent failure paths
  (`:281-284`, `:272-275`) do end the segment. A failed prepare leaves the mergeable target
  recording and the overlay can be skipped on later frames even after recovery. GREEN: end the
  segment on that failure path and mirror the existing log/return pattern.

### M8. Deferred retained-count includes batches that later fail validation

- `source/backends/vulkan/core/core.render_operations.cpp:43-48` increments
  `owner->deferred_retained` before whole-batch validation and lowering; a batch that fails
  validation still contributes to the retained counter. This is instrumentation only (the stream
  and error latch are correct) but makes failed-path statistics misleading. GREEN: count after
  successful lowering or track failures separately.

### M9. Device probes are outside CTest and not recorded with commands/counts

- `CMakeLists.txt:1667-1679` deliberately builds `test_resource_group`, `test_texture_group` and
  `test_submission_completion` outside headless CTest; only the CPU `test_render_operations` runs
  in CI. The acceptance report claims device probes cover the semantics but records no command,
  probe names or check counts; this reviewer ran them manually (all pass) but CI cannot.
  GREEN: record the probe command lines and check counts in the acceptance report (and consider an
  optional device-CI job, as other device probes in the project already use).

### M10. Stale historical review text about device suites is now false for this build

- `docs/reviews/2026-10-24-rhi-command-recorder.md:56` says "The full device suites are not green"
  and its table cites a texture-group device failure; on this HEAD the reviewer's
  `test_texture_group --with-device` run passes 95 checks and `test_resource_group --with-device`
  passes 158. The document is a historical ABI 49 review, but it is not dated in a way that stops
  a reader using it against ABI 53. GREEN: annotate as "superseded by ABI 53 texture-group table
  relocation" or update the sentence.

### M11. Legacy raw `vk_pipeline::begin_pipeline` path remains and is not deferred-aware

- `source/backends/vulkan/core/handles/handles.cpp:204-208` still records
  `vkCmdBindPipeline`/`vkCmdSetViewport`/`vkCmdSetScissor` directly on a raw command buffer.
  No current caller was found after the ABI 53 refactor (grep over `source/` found only the
  definition and declaration), so this is dead legacy code; if a future caller appears it will
  bypass the deferred stream and break the applied-state cache. GREEN: delete or mark it
  private/legacy with a warning.

## Coverage gaps against the plan's required cases

CPU `test_render_operations` actual coverage (HEAD file, 101 lines):

- Covered: overwritten state (vertex 1 -> 2), A/B/A with consumers (vertex/draw), duplicate
  execution protection (two identical draw_indexed retained), push byte copying (source mutated
  after append), partial overlap and full-overlap push elimination, terminal vs intermediate
  application, dispatch not consuming vertex/index but consuming pipeline/push, and 256
  deterministic random streams whose consumer observations compare `simplify(true)` with
  `simplify(true, false)`.
- Gaps: only vertex/index/pipeline/draw_indexed state kinds are exercised; viewport, scissor,
  cull, depth_write, depth_bias, resource_group, texture_group and push liveness across consumers
  are not; no explicit A/B/A for push/group/pipeline; no effect-count check for dispatch, mesh,
  trace or indirect draws; no `optimize=false` run for the intermediate case; no append_copy or
  empty-stream test; no equality test for resource/texture refs.
- Device coverage gaps: execute with pending primary state and a consumer after execute; prepare/
  finish native with pending state and a later consumer; post-ABI53 GUI explicit heap bind smoke;
  pipeline token conflict introduced by a later pipeline (M5); same-address/different-generation
  identity recycling (code compares a monotonic generation, so the logic is correct, but only
  object-lifetime, not address recycling, is tested); failing implicit flush / partial recording
  path; different-but-compatible pipeline switch retaining a loaded group (the probe only rebinds
  the same pipeline, `test_resource_group.cpp:424`).
- `E2E` coverage is indirect: frozen gates and the byte-identical PBR/Gaussian screenshots verify
  the tested scenes produce the same output, not every RHI family or native boundary.

## Baseline-control assessment (answer to the plan question)

The control is sound evidence for: same workload (same requests), identical effect count (work 8
and 11), byte-identical output within the tested scenes, and simplification removing 8/41 and
12/52 native setters on the same deferred implementation. It is **not** sound evidence for the
plan's "before deferral" baseline: both arms run the new deferred stream and the new group/token
lowering; the pre-deferral build was not captured with native setter counts (only API request
counts exist in untracked `tmp/render-operations-baseline/`), and `DEREN_RENDER_OPERATIONS_UNOPTIMIZED`
does not restore the old immediate implementation. The acceptance report already admits this; the
plan Task 1/Task 3 wording and checkbox disposition must follow.

## Unverified claims (treated as limitations, not accepted)

- Any reduction of native state recording relative to the pre-deferral implementation (I1).
- CPU recording cost and allocation count (I2).
- The restore path after `execute`/`prepare_native`/`finish_native` with pending state, and the
  post-ABI53 GUI explicit heap bind behavior (I3).
- "Full device suites green": the device probes pass manually, but are outside CTest and no
  recorded run is in the acceptance artifacts (M9); the historical review says otherwise (M10).
- The untracked acceptance artifacts have no executable/shader identity (M3), so "one Release
  build" is reported but not independently tied to a specific binary; the frozen gate references
  and CTest results were independently reproduced on the current HEAD build.

## Residual risks

- The tests that exercise deferred semantics are device probes outside CI; a headless-only
  regression can pass while a native-boundary bug returns.
- No frame-time or CPU-cost evidence; the win is measured only in native setter calls, and the
  report correctly does not claim a frame-time improvement.
- The CPU probe backend's weaker semantics can mask contract regressions in probe-based tests
  (M1).
- The pipeline-induced token-conflict behavior is deferred to the next consumer (M5); a future
  native interop consumer could observe transient native bytes.
- The measurement and baseline artifacts are untracked and carry no build identity (I1/M3).

## Commands run

```text
git rev-parse HEAD; git log --oneline -12; git status --short; git diff --check c7382586^..HEAD
ctest --test-dir build-release-dyn-clang64 --output-on-failure
build-release-dyn-clang64/test_docs.exe
cmake --build build-release-dyn-clang64 --target clang-format-check check-backend-boundary
build-release-dyn-clang64/test_render_operations.exe
build-release-dyn-clang64/test_resource_group.exe --with-device
build-release-dyn-clang64/test_texture_group.exe --with-device
build-release-dyn-clang64/test_submission_completion.exe --with-device
build-release-dyn-clang64/test_render_graph_frame.exe --with-device
build-release-dyn-clang64/test_gaussian_gpu_frame.exe --with-device
pwsh -File scripts/windows/check_render.ps1 -Full \
  -Compare docs/reviews/2026-10-17-render-graph-gate/legacy-hashes.txt \
  -BuildDir build-release-dyn-clang64
pwsh -File scripts/windows/check_render.ps1 -Visibility \
  -Compare docs/reviews/2026-10-18-visibility-buffer/vis-hashes.txt \
  -BuildDir build-release-dyn-clang64
# inspection of docs/reviews/2026-10-24-render-operations/{results.json,*.debug.log,*.png}
# and tmp/render-operations-{baseline,optimized,before}/
```

## Files inspected (non-exhaustive)

- `source/promise/rhi/render_operations.cppm`, `source/backends/vulkan/core/core.render_operations.cpp`,
  `core.api_core.cpp`, `core.declarations.cppm`, `core.resource_groups.cpp`,
  `core.texture_groups.cpp`, `core.cpp`, `radix_sort.cpp`,
  `source/backends/vulkan/shared/command_buffer.cppm`,
  `source/backends/vulkan/graphical_user_interface/gui_dll.cpp`,
  `source/backends/vulkan/core/handles/handles.cpp`,
  `source/tests/{test_render_operations,test_resource_group,test_texture_group,
  test_submission_completion,test_dynamic_link,probe_backend}.cpp`,
  `source/engine/gaussian_splatting/gpu_probe.cpp`.
- Docs: plan/spec above; `docs/reviews/2026-10-24-render-operations.md`;
  `docs/resource_groups.md`; `docs/dynamic_runtime.md`;
  `docs/reviews/2026-10-24-gaussian-gui.md` and results; `docs/reviews/2026-10-24-rhi-command-recorder.md`.
- Artifacts: `docs/reviews/2026-10-24-render-operations/`; untracked
  `tmp/render-operations-{baseline,optimized,before}/`; frozen gate references.
