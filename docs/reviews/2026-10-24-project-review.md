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

---

# Second pass (2026-10-24, later the same day): gates actually executed

Status: read-only. Base HEAD `efca5b9c` ("fix: end GUI recording segment when prepare_native
fails; docs: project review"), which already contains the first pass above, the `b9006123`
test-data archive doc, and the fixes for the first pass's F3 (`gui_dll.cpp` now ends the
segment and logs) and F5 (historical note on the ABI 49 device-suite sentence). No source,
CMake, git or existing document was modified; this section is the only write. Its difference
from the first pass is that the gates below were **run**, not quoted, and that repo state,
CI wiring and claim provenance were measured.

Note on the moving tree: while this pass ran, concurrent work (not this review) modified three
tracked sources at 20:54:39 (`core.render_operations.cpp`, `rhi.api_core.cppm`,
`test_dynamic_link.cpp` - an in-flight deduplication of the indirect validators, i.e. the first
pass's F4). All gate results below are against the tree as built from `efca5b9c`; the working
tree diff is not part of this review.

## 5. Verification actually run

| Check | Command | Result |
| --- | --- | --- |
| Full Release suite | `ctest --test-dir build-release-dyn-clang64 --output-on-failure` | **69/69 passed**, 39.47 s [verified] |
| Docs invariants | `build-release-dyn-clang64/test_docs.exe` | **419 checks, 0 failed** [verified] |
| Format gate | `cmake --build build-release-dyn-clang64 --target clang-format-check` | **PASS** (`[1/2] Checking sources with clang-format (dry-run)`, no findings) [verified] |
| Boundary gate, documented flip form | `python scripts/check_backend_boundary.py --require-zero --build-dir build-release-dyn-clang64` | **exit 1** - 31 import sites, *all* under `tmp/` [verified] |
| Boundary ratchet (CI form) | `build-release-dyn-clang64/backend_boundary_report.json` | 0 cross-boundary symbols; whitelist 0 hit / 0 stale / 0 untracked [verified] |
| Engine imports of backend modules | grep `^import deren.vulkan` over `source/{engine,app,promise,utility}` | 0 sites [verified] |
| Markdown link targets in `docs/**/*.md` | local scan | 0 broken [verified] |
| Backticked `docs/...` paths | local scan | 2 missing, both in a historical review [verified] |
| Backticked `tmp/...` evidence paths | local scan | **15 missing** of 71 references [verified] |
| First-party TODO/FIXME markers | grep over `source/` minus `third_party` | 0 [verified] |
| Frame-slot teardown order | `core.cpp:838-853` | `vkDeviceWaitIdle` then `poll_submissions(true)` - safe [verified] |
| Disk footprint | per-directory `Measure-Object` | build trees ~16.2 GB, `tmp/` 1.5 GB, `.git` 599 MB [verified] |

Not run (deliberately): `check_render.ps1` (`-Full` / `-Visibility`), the ABI 53 device probes,
the docs build, and any ASan run. CTest ran the existing build tree; `deren_gui_vulkan.dll` was
rebuilt by the `efca5b9c` work at 20:52:05, before this run.

## 6. Repo and working-tree state (measured)

- **79 commits exist only in this working copy.** `origin/master` is `d9285550` (2026-10-20
  07:38), the unpushed range starts at `db1005fa` (2026-10-20), and `.git/FETCH_HEAD` is from
  2026-10-19 18:53 - the remote-tracking ref has not been updated since before the ABI 53 work
  began. Everything from the ABI 53 refactor through the two reviews of this day is local.
- **Nine of ten local branches have no upstream** (`git branch -vv`), the oldest from 2026-09-26;
  plus a second worktree at `C:/Users/23530/.codex/worktrees/baseline-no-subtilis/deren` in
  **detached HEAD** at `344c3ef8` that is **dirty** with four modified files (`main.cpp`,
  `visibility.cppm`, `visibility.slang`, `visibility_resolve.slang`) and 236 MB of build output.
  That worktree is the pre-deferral baseline the render-operations review says could not be
  reconstructed; its edits exist in no commit.
- Untracked at HEAD: `.vscode/tasks.json` (0 bytes) and the two CI checkpoints, the latter
  deliberately retained per `docs/superpowers/progress.md:38`. No stash; one dangling commit plus a
  handful of dangling blobs/trees (normal amend/rebase leftovers, nothing referenced).
- Workspace weight: `build-release-dyn-clang64` 11.9 GB, `build-asan` 3.4 GB, `build-subtilis`
  416 MB, `build-subtilis-asan` 436 MB; `tmp/` 1.5 GB across **21 full source snapshots**
  (`tmp/subviews-before`, `tmp/render-operations-before`, `tmp/push-unification-before`,
  `tmp/vulkan-descriptor-heap-repro-public`, ...); `.cache` 9 MB, `.superpowers` 6.5 MB,
  `docs/html` + `docs/latex` 51 MB, `debug.log.old` 5.8 MB.
- Test-data hygiene is addressed: `b9006123` archives the pre-2026-10-24 experiment trees into two
  zstd tars under `D:/deren-assets/archives/2026-10-24-old-test-data/` with SHA-256 for each and
  a per-file manifest, sources retained. The remaining exposure is that the durable copy lives on
  one `D:` volume while the repo's evidence references still point at `tmp/` (see 9).

## 7. Architecture

### What is strong, measured rather than asserted

- **One ABI surface, mechanically guarded.** Exactly one export (`deren_make_api_core`, read from
  the DLL's export table by the boundary report), one versioned handle (`rhi::abi_version` 53),
  one C++ root, append-only virtual discipline. The `struct_size` guard is real code: 118 uses
  across the tree, one helper (`covered_by`, `core.declarations.cppm:277`) and read-only-as-declared
  logic (`core.api_core.cpp:3170-3177`, `:3256-3258`). No defect found in this pass.
- **The layering holds.** The engine does not link the backend (`CMakeLists.txt:959-971`); the
  executable resolves the DLL by name; 0 engine-to-backend symbols and 0 backend-module imports
  from tracked sources.
- **Refusals are named and compiler-enforced.** `[[nodiscard]] error` on every refusing verb, only
  6 `error::unsupported` default bodies in the 132-virtual interface, no silent drops, and a
  completion path that validates device identity and sequence
  (`core.submission_lifetimes.cpp:54-103`).
- **No rot markers.** Zero first-party TODO/FIXME/HACK in `source/`; deferred work lives in the
  plan/spec docs, which are closed except where explicitly marked partial.

### Where the current shape costs

1. **Two 2000-5000 line units carry the whole renderer contract.** `core.api_core.cpp` is 5005
   lines / 240 member definitions; `rhi.api_core.cppm` is 2092 lines / 132 virtuals;
   `core.declarations.cppm` is 2200 lines. ABI 53 added the right siblings
   (`core.render_operations.cpp` 334, `core.resource_groups.cpp`, `core.submission_lifetimes.cpp`
   144), but the root file remains where behaviour concentrates, is not clang-tidy'd, and is
   executed by no CI test (8).
2. **`render_operations.cppm` keeps two discriminants for one variant.** Each queued op is a
   `std::variant` alternative that also overrides `virtual operation_kind kind()` and carries a
   virtual destructor (`:39-48`, `:69-74`), so every op pays a vptr duplicating the variant tag,
   and `append_copy` re-spells the 19 alternatives as a 19-case switch over enum values
   (`:112-174`) - the compiler cannot catch a missed case. A free `kind()` over `std::visit` (or
   the variant index) deletes the vptr, the switch and the base class.
3. **Per-application allocation and locking on the recording hot path.**
   `core.render_operations.cpp:37-47` builds up to three `operation_stream`s per
   `apply_operations()` and re-copies every operation (`bytes.assign` + two refs per op,
   `render_operations.cppm:107`), on a path that runs at `end_recording` *and* at every native
   boundary; `:36` reads `std::getenv` per call; `:50-58` takes `contract_pipelines_mutex` /
   `contract_buffers_mutex` **inside the per-operation validation loop** (`:90`, `:98`, `:110`,
   `:115`), i.e. once per bound handle on a path shared by multi-threaded mergeable recording.
   This is the concrete locus of the still-open I2 (CPU cost / allocation count unmeasured), so
   the honest claim remains "fewer native setters, cost unknown".
4. **Three hand-maintained target lists and a 2287-line CMakeLists.** `VR_TEST_TARGETS`
   (`CMakeLists.txt:1295`), the CI ASan `--target` list and the workflow mirror are kept equal only
   by `test_docs` invariant 6 - a good mitigation of a design that manufactures the drift. The
   clang-tidy list has the same shape (now validated before use, a fix earned by a red master).

## 8. Tests and CI: what the green gates cannot see

The 69-entry suite, the ASan job, the shader-source cross-check, the boundary ratchet and the
docs invariants are a materially better gate set than most projects this size. The residual gaps
are about reach, not decoration:

- **No CI job executes one line of the Vulkan backend.** `deren_vulkan` is compiled in both CI jobs
  and linked by no CTest target; the six tests that reach it only do so under `--with-device`, which
  CI never passes. 18.5k lines - the whole ABI 53 behavioural change - are compile-gated only. This
  is F1 above, now measured from the link graph rather than inferred.
- **ASan cannot see it either.** `deren_vulkan` is built with `-fno-sanitize=all`
  (`CMakeLists.txt:500-501`) under the documented uninstrumented-DLL policy, and the sanitizer job
  builds test targets only. The memory-safety evidence covers loaders/utility/config, not the
  resource-group and submission-lifetime registries ABI 53 added.
- **The ABI 53 probes are outside CTest** (`CMakeLists.txt:1667-1679`):
  `test_resource_group` (472 lines), `test_submission_completion` (224), `test_texture_group`. They
  are the only tests of the ownership/completion machinery; CI compiles and never runs them.
- **The boundary import gate is not wired into CI.** The CMake target invokes the script without
  `--require-zero` (`CMakeLists.txt:1288-1293`) and the engine-imports-backend failure lives only
  inside that branch (`check_backend_boundary.py:831`, `:887-893`). CI enforces the symbol ratchet
  and not the import graph, although the script's own usage text calls `--require-zero` "the flip
  gate".
- **The same gate gives a different answer locally.** `scan_backend_imports` walks the repo root
  pruning only `.git`, `third_party` and `build*` (`:577-579`), so the 21 `tmp/` source snapshots
  are scanned. Result: the documented gate **fails here** with 31 import sites, every one of them
  under `tmp/` (`tmp/subviews-before/...`, `tmp/render-operations-before/...`,
  `tmp/vulkan-descriptor-heap-repro-public/application/...`), while tracked sources have 0. A clean
  CI checkout passes because it has no `tmp/`; a developer's tree does not.
- **No clang-tidy on `render_operations.cppm` or any `source/backends/**` file.** The 36-entry list
  includes `rhi.api_core.cppm` but not the new recording module, which is pure CPU and the
  second-most important new header.
- **No docs build in CI**, and `test_docs` deliberately does not check that a markdown link's target
  exists (`test_docs.cpp:30-34`). The two stale references in 9 are exactly that class.

## 9. Docs and claims: the provenance gap

The Subtilis closeout, the withdrawn 5.22 ms / ~26x number, "M4 NOT accepted", the scoped Gaussian
numbers and the UE-comparison document's self-declared status are all honest and consistent. The
remaining problem is that the project's headline numbers cannot be re-derived from the repository:

1. **No device identity on the headline measurement.**
   `docs/reviews/2026-10-23-subtilis-textured-results.json` records asset + executable SHA-256,
   resolution, rounds and frames - but no GPU model, driver version or shader-bundle hash, and the
   acceptance doc names no device. `README.md:121` and `:174` then repeat 10.44/5.64 ms as the
   shipping gain. The bundle-hash omission is recorded as a waived limitation; the device omission
   is recorded nowhere.
2. **The raw evidence is referenced but absent.** The same doc says raw logs/configurations/images
   "remain under `tmp/subtilis-textured-acceptance-resumed/`"; that path does not exist, and `tmp/`
   is gitignored. 15 of the 71 backticked `tmp/...` references across `docs/` point at paths that no
   longer resolve. `tmp/` is simultaneously the evidence archive and the thing that breaks the
   boundary gate.
3. **The pinned shader compiler is not the compiler this build used.**
   `build-release-dyn-clang64/CMakeCache.txt:VR_SLANGC_EXECUTABLE=C:/VulkanSDK/1.4.357.0/Bin/slangc.exe`
   reports `2026.13.1-1-g84792eb15`, while `README.md:312` pins `2026.18.2` (the version CI verifies
   by checksum) and `docs/mesh_shaders.md:39` documents a task-stage crash specific to 2026.18.2.
   The README permits the SDK's `Bin/`, so the build is not in violation - but every number and
   frozen-hash capture from this tree came from a different shader compiler than CI's, and no
   results file records which.
4. **Evidence duplication.** `cae23b92` added four 4.1 MB PNGs; the two arm pairs are byte-identical
   (`FAE60C0B...`, `8555C829...`), so 16.6 MB of the 9,957-line commit is the same two images twice,
   on top of the frozen gate references (F6).
5. **Retired vocabulary still in README.** `README.md:152` and `:169` call the per-slot recording
   "secondary command buffers". The public secondary role was retired in ABI 37
   (`docs/reviews/2026-10-18-retire-secondary-command-buffers.md`); the runtime now records
   `mergeable_command_buffer` segments (`runtime.frames.cppm:1088`, `:2603-2614`) which the backend
   may implement with a secondary-level Vulkan object internally - defensible about the
   implementation, misleading about the contract.
6. **Two stale links** in `docs/reviews/2026-10-18-docs-audit.md` to `docs/megalights.md` and
   `docs/reference/megalights_stochastic_lighting.md`, both removed.

## 10. Risks, ranked (second pass)

R-numbers are new; overlaps with the first pass's F-numbers are named. [verified] = measured here;
[suspected] = inference from code shape that was not exercised.

| # | Risk | Evidence | Sev |
| --- | --- | --- | --- |
| R1 | **79 commits of reviewed work exist only in this working copy**, and a dirty detached worktree holds the only copy of the pre-deferral baseline edits. One disk failure or a careless `git worktree prune` loses them. | `git status`, `git branch -vv`, `git worktree list`, `git -C ... status` [verified] | **Critical (process)** |
| R2 | **CI executes zero backend code; the ABI 53 semantics are covered only by device probes CI never runs**, and the same DLL is excluded from ASan by policy. (= F1, measured) | link graph, `CMakeLists.txt:500-501,1667-1679`, `ci.yml:143,352-379` [verified] | **High** |
| R3 | **The boundary gate gives a different verdict locally than in CI, and its import half is unenforced in CI.** `--require-zero` fails here on 31 `tmp/` snapshots; the CI target omits the flag. A real engine-to-backend import would pass CI. | `check_backend_boundary.py:577-579,831,887-893`; `CMakeLists.txt:1288-1293`; exit 1 [verified] | **High** |
| R4 | **Measurement provenance does not meet the project's own standard** (no device, no bundle hash on the headline number; raw captures absent; 15 dead `tmp/` evidence links). | results JSON keys, link scan [verified] | **High (claims)** |
| R5 | **The frozen-hash render gate is not reproducible in CI**: local `-march=native` + slangc 2026.13.1 vs CI `-march=x86-64-v3` + 2026.18.2, and `check_render.ps1` runs nowhere in CI. | `CMakeCache.txt`, `README.md:312`, `ci.yml:126-147` [verified / suspected: that it has already shifted a hash] | **Medium-High** |
| R6 | **The deferral's CPU/allocation cost is unmeasured and the code shape suggests it matters** (3 streams + full re-copy per apply, `getenv` per call, per-op global mutex). (= F2/I2) | `core.render_operations.cpp:36-58,85-120` [verified lines / suspected magnitude] | **Medium** |
| R7 | **Concentration risk**: 5005-line and 2092-line units carry the contract and its implementation, neither clang-tidy'd; every change pays in review cost and rebase pain. | line counts [verified] | **Medium** |
| R8 | **`render_operations.cppm`'s duplicated discriminant + 19-case copy switch** will drift when the next op kind is added, and the compiler will not report it. | `:39-48,69-75,112-174` [verified] | **Medium** |
| R9 | **Workspace weight and stale source copies** (~16.2 GB builds, 1.5 GB `tmp/` with 21 snapshots). Beyond disk, the stale copies are read by the boundary scan (R3) and invite grep/doc drift. | per-directory sizes [verified] | **Medium** |
| R10 | **Adjacent-test duplication of the indirect validators** (F4): two near-identical range validators differing only in record size, exercised by no device path; the mesh-task bug was caught by a gate, not a test. | `rhi.api_core.cppm:1513,1537`; `core.api_core.cpp:4331,4376` [verified lines] | **Medium-Low** |
| R11 | **Production behaviour reachable from the environment** (F7): `DEREN_RENDER_OPERATIONS_UNOPTIMIZED` and `DEREN_RENDER_OPERATION_STATS` are read in shipping source, undocumented, and change recording silently. | `core.render_operations.cpp:36`; `core.api_core.cpp:5179-5180` | **Low** |

## 11. Recommended next actions

1. **Push, then name the state.** `git push origin master` (79 commits) and tag the ABI 53
   checkpoint (`c7382586`) and this review's HEAD. Then commit the dirty baseline worktree's four
   files to a named branch or delete the worktree explicitly. This is the only unrecoverable item.
2. **Give the backend one CI-executed path.** Cheapest: register the device probes inside
   `VR_TEST_TARGETS` with a device check that returns ctest's "skipped" (77) when no GPU is present,
   so the existing `test_docs` list-equality check keeps covering them. Durable: implement the probe
   backend's save/restore/identity equivalence (first pass F1/M1) so contract semantics run headless.
3. **Fix the boundary gate's scope and wire it.** Prune `tmp` (or scan only paths that `git ls-files`
   reports) in `check_backend_boundary.py:577-579`, and pass `--require-zero` from the CMake target
   so CI fails on a real engine-to-backend import. Both are small; the second is the difference
   between a report and a gate.
4. **Make provenance a rule and apply it retroactively.** Results files record executable and
   shader-bundle SHA-256, GPU model, driver, slangc version, build flags (`native` on/off), and the
   raw-capture directory (durable `D:/deren-assets`, not `tmp/`); add the missing device line to the
   textured acceptance doc as a recorded limitation, the way the bundle-hash gap already is.
5. **Restore claim hygiene**: correct the "secondary command buffer" wording (`README.md:152,169`),
   fix the two megalights references, keep one PNG per distinct image, and state in README that the
   render gate is local-only and toolchain-pinned.
6. **Measure or descope I2**: time `apply_operations()` and count allocations per frame on the two
   scenes (`core.render_operations.cpp:37-47`); if the cost is non-trivial, stop re-copying into
   `combined` and validate the two streams in place.
7. **Simplify the discriminant** in `render_operations.cppm` (free `kind()` + `visit`), and add the
   module to the clang-tidy list.
8. **Prune the workspace** after the archive is accepted: `tmp/` snapshots, `build-subtilis*`,
   `.cache/clangd`, `debug.log.old`; add scratch patterns to `.gitignore` so `git status` stays
   honest.

## 12. Verdict (combined)

**Engineering discipline 8/10; verified coverage of the current checkpoint 6/10; process and
reproducibility 5/10.** The ABI boundary, the `struct_size` discipline, the error vocabulary, the
zero-rot state and the docs' honesty about negative results are better than typical for a solo
renderer of this size. The risk is concentrated exactly where ABI 53 moved: recording, resource
groups and completion tokens, which no continuous job executes, and whose only witnesses are device
probes outside CTest running against a DLL the sanitizers cannot instrument. Above all, four days of
reviewed work have never left this machine, and the pre-deferral baseline lives in a dirty detached
worktree.

No critical code defect was found in either pass. Every critical/high finding is about verification,
provenance and preservation - not about behaviour observed to be wrong.

## Post-review disposition (this checkpoint)

Actionable findings fixed after the two review passes:

- **F3 (GUI prepare_native):** `gui_dll.cpp` now calls `end_recording()` and logs before returning when
  `prepare_native()` fails (`efca5b9c`).
- **F4 (validator duplication):** the two indirect-range validators now share
  `rhi::validate_indirect_range(size, offset, count, stride, record_size)`; draw uses 16 B and mesh-task 12 B,
  with compile-time coverage for both (this checkpoint).
- **F5 (stale device-suite claim):** `docs/reviews/2026-10-24-rhi-command-recorder.md` carries a historical
  note and the red sentence is scoped to the ABI 49 tree (`efca5b9c`).
- **F6 (evidence sprawl, partial):** the two redundant unoptimized-arm PNGs were removed (8.3 MB); both arm
  hashes remain in `results.json`. Log retention is still unbounded (follow-up).
- **F7 (env switch):** `DEREN_RENDER_OPERATIONS_UNOPTIMIZED` logs once when armed; both developer controls are
  documented in the ABI 53 manual section (this checkpoint).
- **Boundary gate (second pass F3):** the import scan prunes workspace `tmp/` snapshots, and the
  `check-backend-boundary` CMake target now passes `--require-zero`; the gate reports 0 imports and 0 symbols
  and is enforced by the target (this checkpoint).
- **F1 (CI-visible deferred contract, partial):** added `scripts/windows/check_device_probes.ps1` with expected
  counts and a recorded `device-probes.json` lane (5/5 probes PASS); headless CI still cannot run device probes.
- **F2 (CPU/allocation measurement):** explicitly out of scope for the current acceptance checkpoint; no
  performance claim is made. The spec measurement remains a follow-up before any performance statement.
- **Process finding (unpushed commits / dirty baseline worktree): NOT silently fixed.** The dirty detached
  worktree at `.codex/worktrees/baseline-no-subtilis/deren` (HEAD 344c3ef8, 5 modified files) was captured
  read-only to `D:/deren-assets/archives/2026-10-24-baseline-no-subtilis/` (dirty.patch, status, hashes) because
  it was the only copy of those pre-deferral edits. Pushing the 79 local commits and committing/removing that
  worktree still requires the user's decision; do not delete it without checking the archive.
