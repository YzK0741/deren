# Source layout and engine naming implementation plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan task-by-task. The user approved the design and explicitly requested execution in this session.

**Goal:** Make module names and source directories match the established engine/backend boundary.

**Architecture:** Rename backend-neutral C++ modules and namespaces to `deren.engine` and `deren::engine`. Move project code under `source/`, placing native Vulkan implementations under `source/backends/vulkan/`. Keep portable layout sharing and DLL contracts intact.

**Tech Stack:** C++23 modules, CMake/Ninja, Python, PowerShell, Slang.

**Spec:** `docs/superpowers/specs/2026-10-09-source-engine-layout-design.md`

## Global constraints

- Preserve RHI ABI 28, GUI ABI 2, exported C entry points, DLL file names, runtime configuration, compiled shader destinations, and rendering behavior.
- Use `deren.engine.*`, `deren::engine`, target `deren_engine`, alias `deren::engine`.
- Filters belong in `source/engine/filters/`, module `deren.engine.filters`.
- Preserve frozen rendering reference hashes and reference shader contents.
- Build parallelism 10. Stop development and report at 5% remaining short-term quota.
- Local commits only; no push or merge.

## Review focus

- Shared layout names must resolve from native backend namespaces after engine renaming: compile both halves.
- Boundary scripts must scan renamed source directories and archives: exercise synthetic violations as well as real zero gates.
- Moved tools must find the repository root regardless of working directory: invoke gates from outside the root.
- Source shader moves must not redirect runtime shader lookup: build all stages and run shader/resource tests.
- CI formatting, fixture generators, Doxygen, reference shaders and vendor includes must follow moved paths: run existing source/document checks and inspect CI invocations.

## Task 1: Engine names

**Files:** Engine modules in `runtime/` and backend-neutral `vulkan/` directories; consumers in `main.cpp`, `chores.*`, `tests/`; `CMakeLists.txt`, boundary scripts/baseline archive metadata, documentation.

**Interfaces:** Native modules remain `deren.vulkan.*`. Portable `render_layout` becomes `deren.engine.render_layout`; backend uses explicitly qualified portable layout references.

- [x] Rename module declarations/imports and engine namespaces by actual ownership, including `deren.vulkan.core.filters` → `deren.engine.filters`.
- [x] Rename the engine CMake target and add `deren::engine` alias; update archive/object gate names and synthetic fixtures.
- [x] Reconfigure/build Release, run headless tests and boundary gates, then commit.

## Task 2: Production source layout

**Files:** `source/app/`, `source/engine/`, `source/backends/vulkan/`, `source/promise/`, `source/application_configuration/`, `source/gltf_loader/`, `source/utility/`, `source/vstd/`; build/test/tool references.

**Interfaces:** DLL names and module identifiers from Task 1 remain unchanged. Root CMake remains the build entry point.

- [x] Move production code using the spec mapping and update all active source paths.
- [x] Update native-boundary source discovery and add a regression that catches Vulkan vocabulary in `source/engine/` before accepting the new zero result.
- [x] Reconfigure/build Release, run headless tests and boundary gates, then commit.

## Task 3: Remaining code and verification

**Files:** `source/tests/`, `source/scripts/`, `source/shaders/`, `source/third_party/`, `source/shaders/reference/official/`; `.github/workflows/ci.yml`, Doxyfile and docs.

**Interfaces:** Scripts resolve the repository root above `source/`. Shader outputs remain `<build>/shaders/`; model paths remain rooted in `gltf_model/`.

- [x] Move remaining source directories and reference shaders; update tool roots, fixtures, includes, build globs, CI and documentation.
- [x] Run headless tests, both boundary gates from the new tool paths, formatting and available graphical regression gates.
- [x] Inspect tracked source inventory and unchanged binary contracts/reference contents; obtain a fresh read-only whole-change review, fix actionable findings, and commit.

## Execution rulings

- Execute in the existing `wip/recording-face` checkout: the user asked to continue this workspace, its worktree was clean, and its configured build can be reused. No new worktree is necessary for this mechanical reorganization.
- The user's “就这么做吧” authorizes this concrete approved scope and execution; do not add another planning approval pause.
- Use existing build and regression tests for mechanical renames; add tests only where source-discovery behavior changes and a false-zero gate is possible.

Task 1 evidence: Release build exit 0; 20/20 CTest tests passed; backend boundary 0 symbols and 0 engine imports; native zero gate passed within CTest.

Task 2 evidence: new-directory scope regression failed before the gate change and passed afterward; Release build exit 0; 20/20 CTest tests passed; native and backend zero gates passed. Relative includes are recalculated from resolved original targets; source-reading tests follow app/ paths.

Task 3 evidence: Release build exit 0; clang-format-check passed; 20/20 CTest
entries passed (including 43 Python boundary regressions). Native vocabulary,
engine object references and executable Vulkan imports are all zero; backend
cross-boundary symbols and engine module imports are zero. Both tools passed
from outside the repository with absolute build paths. Fourteen frozen render
scenarios matched; GUI and RT GPU gates passed with clean validation.

Both manual shader tools compiled 35 outputs, all byte-identical to CMake.
All PowerShell and six Shell scripts passed syntax checks. Doxygen generated
HTML successfully. All 189 CMake and 16 CI literal source paths exist; 96 vendor
and reference shader blobs are unchanged; tracked code outside source is empty.
Fresh independent review found no remaining P1/P2 after corrections.

Review corrections: vendored include roots, standalone Vulkan-header detection
(red then green regression), whitelist fixture location, manual shader output,
main.cpp formatting scope and reference-shader documentation exclusion. The
full render gate also caught a Windows-style deformation fixture path; after
fixing it all fourteen frozen comparisons passed. RHI ABI 28 and GUI ABI 2 remain
unchanged. Remote CI and a new Debug build were not run for this local batch.
