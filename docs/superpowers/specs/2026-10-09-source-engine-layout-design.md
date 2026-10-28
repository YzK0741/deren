# Source layout and engine naming

## Goal

Reflect the completed RHI boundary in module names and directories. Use `engine`
for backend-neutral code and collect project code under `source/`.

## Approved naming

- Backend-neutral modules: `deren.engine.*`; namespaces: `deren::engine`.
- Engine target: `deren_engine`, with a `deren::engine` CMake alias.
- Filters: `source/engine/filters/`, module `deren.engine.filters`.
- Vulkan implementation modules retain `deren.vulkan.*` and `deren::vulkan`.
- Contract modules retain `deren.promise.rhi` and `deren.promise.gui`.

## Directory mapping

| Current location | Destination |
| --- | --- |
| `main.cpp`, `chores.cpp`, `chores.cppm` | `source/app/` |
| `runtime/` | `source/engine/runtime/` |
| Backend-neutral directories under `vulkan/` | `source/engine/` |
| `vulkan/core/filter/` | `source/engine/filters/` |
| Remaining Vulkan implementation directories | `source/backends/vulkan/` |
| `promise/` | `source/promise/` |
| `application_configuration/` | `source/application_configuration/` |
| `gltf_loader/` | `source/gltf_loader/` |
| `utility/`, `vstd/` | Corresponding directories under `source/` |
| `tests/`, `shaders/`, `third_party/` | Corresponding directories under `source/` |
| `scripts/` | Stays at the repository root: it is delivery tooling, not project source (see the note below). |
| `docs/official-shaders/` | `source/reference/shaders/official/` |

All application, backend, contract, library, test, shader, vendor, and executable
tool sources belong under `source/` — the delivery scripts are the one exception and
stay in `scripts/` at the root. Root build entry points (`CMakeLists.txt`),
CI configuration, formatter settings, and Doxygen configuration remain at root.
Documentation and document templates remain in `docs/`; model assets and
snapshots retain their existing locations. Shader reference files are moved
without modifying their contents or adding them to shader compilation.

Backend ownership follows the actual CMake target source lists. In particular,
`vulkan/init_utils/` is native backend code; `vulkan/core/filter/` is engine code.
`render_layout` remains a separate portable library shared with the backend: the
CMake target is `renderer_layout` (`CMakeLists.txt:479`) and it builds the module
`deren.engine.render_layout` (`source/engine/render_layout/render_layout.cppm:22`),
which `deren_vulkan` links (`CMakeLists.txt:592`).

## Behavior and compatibility

This is a source organization change. Update imports, qualified engine names,
source lists, include paths, test fixtures, CI, documentation, and tools together.
Preserve RHI ABI 28, GUI ABI 2, exported C entry points, DLL file names, runtime
configuration, compiled shader destinations, and rendering behavior.

**2026-10-18 现状核对：** the ABI clause above records the constraint of THIS change
(do not bump an ABI just to move files), not the current numbers. Both numbers have
moved since, deliberately and separately from any layout work: the RHI ABI is 33
> 历史规划文档：本文写作时的 RHI ABI 为 33 / GUI ABI 为 3；当前值为 RHI ABI 59 / GUI ABI 5（见 `rhi.contract.cppm` 与 `gui.cppm`）。
(`source/promise/rhi/rhi.contract.cppm:300`) and the GUI ABI is 3
(`source/promise/gui/gui.cppm:18`). The exported C entry points and DLL names are
still what the loaders resolve by name (`source/utility/dynamic_link.cppm`), so the
part of the clause about entry points and file names still holds.

No legacy engine module aliases are introduced. The new source module names
become canonical; true Vulkan backend names remain distinguishable.

Path updates must distinguish source paths from generated/runtime paths:
`source/shaders/` contains shader inputs, while `<build>/shaders/` remains the
runtime output. Scripts must locate the repository root correctly after moving.
Frozen rendering reference hashes and reference shader contents are unchanged.

## Verification and delivery

Deliver separately reviewable local commits: engine naming; production source
layout; remaining code layout and documentation. Adjust boundaries where build
dependencies require an atomic move, and verify each batch before committing.

Reconfigure and build the existing Release tree with parallelism 10; run all
headless tests, both backend/native boundary gates, and the formatting check.
Boundary checks must still cover all engine sources and the renamed engine
archive; no directory move or stale object may produce a false zero result.
Exercise tool root discovery and shader/reference paths from the new locations.
Run existing graphical regression gates serially where the local environment
supports them, and report any unverified CI/Debug/GPU checks explicitly.

Stop development and report progress when the user's remaining short-term
Codex quota reaches 5%. Do not push or merge without authorization.

## 2026-10-18 现状核对

The layout this document approved is in place and was extended afterwards. Verified
against the tree, not against this text:

- `source/app/` holds `main.cpp`, `chores.cpp`, `chores.cppm` as mapped.
- `source/engine/` holds the backend-neutral modules (`runtime/`, `filters/`,
  `render_layout/`, `pass/`, `render_graph/`, `render_resource/`, `gaussian_splatting/`, …);
  `source/backends/vulkan/` holds the implementation (`core/`, `init_utils/`,
  `constant_init/`, `synchronization/`, `graphical_user_interface/`). The old
  `vulkan/init_utils/` split into two backend directories — `source/backends/vulkan/init_utils/`
  and `source/backends/vulkan/core/init_utils/` — so the "native backend code" ownership
  rule still holds, but neither path in the sentence above exists verbatim any more.
- `source/promise/rhi/`, `source/promise/gui/`, `source/application_configuration/`,
  `source/gltf_loader/`, `source/gaussian_loader/`, `source/utility/`, `source/vstd/`,
  `source/tests/`, `source/shaders/`, `source/third_party/` all exist as mapped.
- `docs/official-shaders/` became `source/reference/shaders/official/` and is joined by
  `source/reference/shaders/glsl.old/`; the two are reference inputs only and are not in
  the shader compile list (`VR_SHADER_SRC_DIR` is `source/shaders` and `VR_SHADER_INCLUDES`
  lists only `source/shaders/**`, `CMakeLists.txt:2007`, `2027-2090`).
- `scripts/` did NOT move under `source/`; it is still `scripts/` at the repository root
  with `windows/`, `posix/` and `measure/` beneath it. Tools therefore still resolve the
  repository root one level above themselves, not two.
- New engine targets appeared after this document and follow the same naming direction:
  `deren_engine`/`deren::engine` (`CMakeLists.txt:649-650`), `deren_assets`,
  `resource_cluster_core`, and the `render_graph_*` family (`CMakeLists.txt:1217-1277`).
