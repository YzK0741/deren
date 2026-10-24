# Project review (2026-10-24, time-boxed, read-only)

Scope: current `master` tree at `b9006123` (commits c7382586 ABI 53, 7f38b720 mesh-task
stride, cae23b92 render-operations acceptance, a73dbdac docs audit, b9006123 test-data
archive). Read-only: no code, CMake, git or other document was changed; this file is the
only write. No build, GPU capture, CTest run or device probe was executed.

## 1. Verification actually run

- `git status --porcelain`, `git log --oneline -15`, `git branch --show-current`,
  `git show --stat` for HEAD/HEAD~1/HEAD~2, c7382586 and 7f38b720.
- `build-release-dyn-clang64/test_docs.exe` -> `[test_docs] 419 checks, 0 failed -> PASS`
  (matches the acceptance report's 419/0).
- `ctest --test-dir build-release-dyn-clang64 -N` -> `Total Tests: 69` (registration only;
  the suite itself was not run).
- Read-only inspection: `.github/workflows/ci.yml`, `Doxyfile`, `source/tests/test_docs.cpp`,
  `docs/superpowers/plans/2026-10-24-render-operations.md`,
  `docs/reviews/2026-10-24-render-operations{,-review}.md`,
  `docs/reviews/2026-10-24-render-operations/results.json`,
  `docs/resource_groups.md`, `docs/reviews/2026-10-24-rhi-command-recorder.md`,
  `source/app/startup_splash.cpp`, `source/app/main.cpp`, `source/backends/vulkan/
  graphical_user_interface/gui_dll.cpp`, `source/backends/vulkan/core/handles/handles.cpp`,
  `source/backends/vulkan/core/core.render_operations.cpp`,
  `source/promise/rhi/rhi.api_core.cppm`, `source/tests/test_dynamic_link.cpp`,
  `CMakeLists.txt` (device-probe targets).
- Not run (time box / instructions): CTest 69/69, render gates, device probes, clang-format /
  backend-boundary, any GPU capture. Their status below is taken from committed reports, not
  re-established here.
- Note: `test_docs` scans only top-level `docs/*.md`
  (`source/tests/test_docs.cpp:296-307`), so review files under `docs/reviews/` are outside
  its invariants; this report cannot break that gate.

## 2. Findings and risks (ranked)

### F1. ABI 53 deferred-recording semantics are still only covered by device probes outside CI (high)

The mechanism accepted in cae23b92 is the current headline RHI behavior, but the only
CI-visible test is the CPU probe, which the plan itself describes as a weaker double.

- `CMakeLists.txt:1667-1679`: `test_texture_group` / `test_resource_group` are deliberately
  built outside headless CTest ("Device-only ... deliberately outside headless CTest").
- `.github/workflows/ci.yml:143` runs only `ctest`; the ASan lane
  (`:352-354`) also lists `test_render_operations` but no device probe.
- `docs/superpowers/plans/2026-10-24-render-operations.md:54`: "CPU probe backend is an
  ABI-surface double, not equivalent save/restore/identity semantics (M1)."
- `docs/reviews/2026-10-24-render-operations.md:82`: "Device probes remain outside headless
  CTest by design."
- `docs/reviews/2026-10-24-render-operations-review.md:274-297` lists the resulting uncovered
  cases (execute with pending state + consumer, prepare/finish native restore, post-ABI53 GUI
  bind, pipeline token conflict).

Consequence: a regression in native-boundary flush/restore, or in group retention across a
pipeline switch, can land with a fully green CI. This is the strongest residual risk in the
tree.

### F2. The render-operations result is an opt-in reduction, not a proven performance win; two spec measurements remain absent (high, claims)

The reports are honest about this, but it is the part most likely to be misquoted later.

- `docs/reviews/2026-10-24-render-operations.md:68`: the "before deferral" baseline "could
  not be reconstructed"; the control is optimized-vs-`DEREN_RENDER_OPERATIONS_UNOPTIMIZED`
  on the same deferred stream.
- `docs/reviews/2026-10-24-render-operations.md:77`: CPU recording cost and allocation counts
  "required by the spec are not measured (I2)".
- Measured win is native setters only: 33 vs 41 (PBR) and 40 vs 52 (Gaussian), work equal,
  images byte-identical (`docs/reviews/2026-10-24-render-operations/results.json`); no
  frame-time claim is made.
- Bookkeeping is partial: `docs/superpowers/plans/2026-10-24-render-operations.md:41` (baseline
  unchecked, "not reconstructed") and `:51` (device coverage unchecked) while `:61-64` are
  ticked. That is consistent, but "accepted" plus three open boxes invites a later
  reader to assume the baseline exists.

### F3. GUI plugin leaves its recording segment open when `prepare_native()` fails (medium, code)

- `source/backends/vulkan/graphical_user_interface/gui_dll.cpp:288-289`:
  `if (native_target.prepare_native() != ok) return {};` - no `end_recording()`, no log.
- The sibling failure paths (`:281-284`, and the heap-bind path just above) both call
  `(void)target->end_recording();` and log.

Consequence: a failed prepare leaves the mergeable target mid-recording, so subsequent
frames can keep skipping the overlay even after the failure clears. Two-line fix; still
present at HEAD (a73dbdac changed docs only).

### F4. Mesh-task indirect validation is now a second copy of the same range logic (medium-low, code/tests)

7f38b720 fixed a real defect (the visibility lane was rejected because the draw-indirect
16 B minimum was applied to the 12 B `mesh_task_command`), but the fix duplicates the
contract:

- `source/promise/rhi/rhi.api_core.cppm:1513` (`validate_draw_indirect_range`) and `:1537`
  (`validate_mesh_task_indirect_range`) are two near-identical alignment/offset/overflow
  implementations differing only in the minimum record size.
- Callers: `source/backends/vulkan/core/core.api_core.cpp:4331` and `:4376`.
- Coverage is compile-time only: `source/tests/test_dynamic_link.cpp:88-93` static_asserts;
  no device or gate path exercises `draw_mesh_tasks_indirect`. The bug was found by the
  visibility gate, not by a test.

Consequence: a third indirect family (or a new call site) can repeat the same wrong-minimum
mistake while every test stays green. A per-family record-size parameter (or a
`static_assert` at each call site) would remove the copy.

### F5. One committed review still asserts device suites are red, dated the same day as ABI 53 (medium, docs)

- `docs/reviews/2026-10-24-rhi-command-recorder.md:56`: "The full device suites are not
  green." Its table (`:42-46`) is an ABI 48/49 texture-group failure.
- The file is dated 2026-10-24, the same date as the ABI 53 checkpoint, and its only
  qualifier is "ABI 48/49"; nothing at the top says "historical".
- a73dbdac fixed `docs/resource_groups.md:77-83` (now correctly documents ABI 53 queued
  group/push state) and `docs/dynamic_runtime.md`, but left this sentence.
- The render-operations review (`:256-263`, M10) already flags it as stale and reports the
  ABI 53 probes passing (95 and 158 checks).

Consequence: a reader (or agent) searching "device suites" gets a false red state that
contradicts the acceptance evidence.

### F6. Review artifacts are large and unmanaged; one commit adds ~9k debug lines and 16.5 MB of images (low-medium, repo hygiene)

- 2454 tracked files and ~196 MB under `docs/reviews/` (131 markdown reviews), inside a
  587 MB `.git` pack (`git count-objects -vH`).
- cae23b92 added four 4.1 MB PNGs; two pairs are byte-identical:
  `FAE60C0B...` for `deferred-opt.png`/`deferred-unopt.png`, `8555C829...` for the two
  gaussian arm PNGs. They duplicate each other and the frozen gate references, so half of
  that 16.5 MB is redundant evidence.
- The same commit added 4 x ~2325-line `*.debug.log` (allowed by `.gitignore:23`
  `!docs/reviews/**/debug.log`), with no retention rule or size gate.
- Untracked at HEAD: `.vscode/tasks.json` (0 bytes), and
  `docs/reviews/2026-10-19-ci-checkpoint.md` / `2026-10-20-ci-checkpoint.md`
  (deliberately kept untracked per `docs/reviews/2026-10-20-bistro-textured/progress.md:38`;
  the latter still cites 53/53 local tests, three CI generations back).
- `git count-objects` also reports 10 stale `tmp_obj_*` objects (411 KiB) in `.git`.

Consequence: low functional risk, but evidence volume now dominates the repository and the
untracked notes are the first thing lost with the working tree.

### F7. Test-only environment switches change production behavior (low, code/config)

- `source/backends/vulkan/core/core.render_operations.cpp:36`:
  `std::getenv("DEREN_RENDER_OPERATIONS_UNOPTIMIZED")` is read in normal production source,
  silently disabling simplification/cache skipping if the variable is set anywhere in the
  process environment.
- `DEREN_RENDER_OPERATION_STATS` similarly gates stats emission.
- Only documented in `docs/reviews/2026-10-24-render-operations/results.json`
  (`developer_controls`), not in the ABI/manual docs.

Consequence: a stray environment variable changes recording behavior in a shipping build
with no log line; low likelihood, cheap to fix (log once, or compile out in Release).

## 3. Recommended next actions (first three)

1. **Fix `gui_dll.cpp:288`** (end the segment + log on `prepare_native()` failure), and add a
   device probe for `prepare_native`/`finish_native` with pending state and a consumer after
   the boundary plus `execute(segment)` with pending state (closes F3 and the I3 probe gap).
2. **Make the deferred contract CI-visible**: either implement the probe backend's
   save/restore/identity equivalence (plan Task 2, `docs/superpowers/plans/2026-10-24-render-operations.md:54`,
   M1) so the headless test covers the restore semantics, or record the device-probe command
   lines and check counts in the acceptance report and add a documented, optional
   device-probe lane. Do not leave F1 as "manual only" indefinitely.
3. **Disposition I1/I2 in the plan and report**: keep the narrow "simplification removes 8/41
   and 12/52 native setters with identical work" wording, add CPU recording time and
   allocation counts around `apply_operations` (steady clock + allocator counter) to
   `results.json`, and leave `:41` explicitly "not reconstructed" rather than silently
   partial. In the same pass, annotate `2026-10-24-rhi-command-recorder.md:56` as
   ABI 49 historical (F5) and prune the duplicate arm PNGs (F6).

## 4. Verdict

Well-gated and unusually honest about its limits: the CI-visible code is solid and the
accepted ABI 53 checkpoint is correctly marked partial, but its risky half (native-boundary
and restore semantics) is still CI-invisible and its performance story is unmeasured.
