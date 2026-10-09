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
| `tests/`, `scripts/`, `shaders/`, `third_party/` | Corresponding directories under `source/` |
| `docs/official-shaders/` | `source/shaders/reference/official/` |

All application, backend, contract, library, test, shader, vendor, and executable
tool sources belong under `source/`. Root build entry points (`CMakeLists.txt`),
CI configuration, formatter settings, and Doxygen configuration remain at root.
Documentation and document templates remain in `docs/`; model assets and
snapshots retain their existing locations. Shader reference files are moved
without modifying their contents or adding them to shader compilation.

Backend ownership follows the actual CMake target source lists. In particular,
`vulkan/init_utils/` is native backend code; `vulkan/core/filter/` is engine code.
`render_layout` remains a separate portable library shared with the backend.

## Behavior and compatibility

This is a source organization change. Update imports, qualified engine names,
source lists, include paths, test fixtures, CI, documentation, and tools together.
Preserve RHI ABI 28, GUI ABI 2, exported C entry points, DLL file names, runtime
configuration, compiled shader destinations, and rendering behavior.

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
