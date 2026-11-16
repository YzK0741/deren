<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/images/branding/deren-logo-dark-720.png">
    <source media="(prefers-color-scheme: light)" srcset="docs/images/branding/deren-logo-light-720.png">
    <img src="docs/images/branding/deren-logo-light-720.png" alt="deren logo" width="600">
  </picture>
</p>

**deren** is a real-time renderer built around current C++ and graphics APIs. The engine is portable: it talks to graphics implementations through RHI and GUI contracts and loads them as DLLs. The current baseline combines C++26 modules, deferred PBR, toon shading, Gaussian splatting, Slang shaders, and a backend-independent RHI with a Vulkan backend.

> The project was renamed from `vulkan_render` to `deren` on 2026-10-02. Portable modules use `deren.engine.*` / `deren::engine`; native Vulkan implementations keep `deren.vulkan.*` / `deren::vulkan`.

![The Gaussian viewer: NVIDIA's Bouquet of Flowers (CC BY 4.0), 562,974 splats](docs/screenshots/gaussian-viewer.jpg)

[Gaussian splatting details](#gaussian-splatting).

<p align="center">
  <img src="docs/screenshots/DamagedHelmet.png" width="49%" alt="DamagedHelmet with PBR, IBL and ray-traced reflections" />
  <img src="docs/screenshots/FlightHelmet.png" width="49%" alt="FlightHelmet with PBR and IBL" />
</p>

<p align="center">
  <img src="docs/screenshots/GameToon.png" width="48%" alt="Laevatain rendered with GameToon" />
  <img src="docs/screenshots/GameToonFace.png" width="48%" alt="Laevatain face close-up rendered with GameToon" />
</p>

<sub>DamagedHelmet and FlightHelmet use deferred PBR with Studio Neutral HDR lighting and Khronos PBR Neutral tone mapping; DamagedHelmet adds ray-traced reflections. The Laevatain shots use the GameToon profile with its captured light rig. See [`docs/screenshots/README.md`](docs/screenshots/README.md) for capture settings.</sub>

## Features

- **Engine runtime and modular boundaries**: `deren.engine.runtime` holds the scene-wide GPU resources and drives a frame through explicit phases (`poll_events`, `pace_and_acquire`, `begin_recording`, `prepare_scene_recording`, `record_graph`, `end_recording`, `submit_and_present`). CPU-only modules such as `deren.gltf_loader`, `deren.application_configuration` and `deren.utility` do not depend on Vulkan; the backend is loaded through `deren.engine.backend_loader`.
- **Scene and draw model**: a caller-held scene tree is walked once per frame into world matrices; GPU drawables live in `deren.engine.primitive`. Leaves either use default pipeline semantics or request a named pipeline, so several pipelines and material strategies coexist in one scene. Parallel recording workers each build their `render_environment`. A BVH over leaf bounds culls the scene against the camera and is reused while the scene and camera are static; `static_draw_primitive` merges static sub-meshes into one vertex/index buffer with a per-chunk draw table.
- **Vulkan 1.3 backend**: dynamic rendering (no render passes or framebuffers), a descriptor heap instead of descriptor sets, timeline semaphores, ray queries, mesh/task shaders, host image copies, and buffer device addresses. The RHI/GUI boundary is process-level and backend implementations are loaded by name.
- **Deferred rendering**: a G-buffer pass writes surface targets; the lighting stage shades them into an HDR target with IBL, punctual lights, cascaded shadows and optional ray-traced sun shadows. Post-processing adds exposure, ACES tone mapping, bloom and FXAA; TAA is available separately. The analytic `furnace` mode is an exactness check for the lighting path.
- **Lighting and effects**: metallic-roughness PBR with five texture slots and glTF alpha modes; GGX/Smith by default with selectable direct-light BRDFs; prefiltered environment maps and a BRDF LUT; 1-4 cascaded shadow maps with PCF; clustered light culling; hemisphere-sampling SSAO; ray-traced reflections for PBR and toon materials.
- **subtilis geometry LOD**: visibility buffer + Cluster DAG LOD + HZB occlusion/recovery is the shipping path. DAG+HZB remains the accepted optimization; the software-raster lane is experimental, opt-in and default-off. **M2 GPU cut** (`[render] subtilis_gpu_cut`) is implemented but **off by default**: a persistent-workgroup compute shader selects the cut and writes the indirect command for static, non-skinned, non-morphed meshes on the 80-byte vertex layout, with hysteresis, HZB and software raster disabled. `test_subtilis_cut_gpu` checks parity against the CPU oracle. On textured Bistro at 1280x720 the GPU path dispatched 251 of 2906 items and produced a byte-identical image to the CPU cut, but the frame remained GPU-bound (46.2 fps vs 46.0-46.3 fps); the value is CPU headroom and scale, not a current FPS win. M5a/M5b add an offline page table and a synchronous residency manager; M5c requests cut pages, installs them under a fixed budget on the next frame, and falls back to LOD0 until they are resident. Device-to-device page copies and eviction are not implemented yet. See [the subtilis overview](docs/subtilis.md) and [the M2/M5 plan](docs/subtilis_m2_m5_plan.md).
- **amnis texture residency**: an opt-in texture-residency system independent of the geometry cuts. Visibility and screen-footprint feedback request whole-image, two-tier or progressive mip streaming; bounded async uploads and low-resolution fallbacks keep frames moving. See [`docs/amnis.md`](docs/amnis.md).
- **Gaussian splatting**: `.ply` files load as Gaussian clouds, are packed onto the GPU once, and are drawn by a dedicated stage between lighting and blended geometry. The GPU path prepares, sorts, finalizes and rasterizes without CPU readback.
- **glTF, animation and skinning**: `.gltf` / `.glb` loading with hierarchy, skins, morph targets and keyframe animations; a format-neutral animation controller samples into the scene tree and updates skin/morph buffers per frame.
- **Profiling and capture**: there is no native overlay. `[debug] gpu_timings` records pass boundaries with `vkCmdWriteTimestamp` without waiting on the GPU, and a capture is a managed decision: the host forwards the untouched argv to the game's `preinit`, the demo parses `--capture-frames`, `--capture-camera`, `--capture-sweep`, `--capture-animation-sweep` and `--capture-windowed` into `startup_options`, and a frame is written to PNG only when the game asks for it through `scene.request_screenshot()`.
- **Engineering**: C++26 modules, exceptions and RTTI disabled, `-Wall -Wextra -Werror`, automatic `clang-format`, LTO and dead-code elimination in Release, and a per-module CMake target graph.

## Build helpers

The repository ships platform **setup** scripts and unified Python **build / run / documentation** entry points under `scripts/build_helpers/` (Python 3.8+). Setup stays a shell script because it installs the toolchain that provides Python. The same entry points exist on **Windows** (MSYS2 clang64) and **POSIX** (Linux / WSL / macOS), but only Windows is built by CI; POSIX is source-tree support, not a verified configuration.

Commit and CI rules are in [`CONTRIBUTING.md`](CONTRIBUTING.md). Use `scripts/commit.ps1` and `scripts/push.ps1` instead of direct `git commit` / `git push`. Unfinished work can be backed up on a `wip/*` branch with `python scripts/push_wip.py` (no local gate, no CI job, never merged; see [the WIP channel](CONTRIBUTING.md#the-wip-channel-wip)).

**Setup**

```bash
# Windows (run in an MSYS2 shell): check clang64, install missing pacman packages,
# find the Vulkan SDK, create config.toml from the example
sh scripts/setup/windows.sh

# POSIX: detect distro, install glfw/glm/toml++/Vulkan via apt/dnf/pacman/brew
# (source-tree support only - no CI job builds the project on POSIX)
sh scripts/setup/posix.sh
```

**Build / run / docs**

```bash
# configure + build RelWithDebInfo (the default)
python scripts/build_helpers/build.py
# one configuration / clean rebuild
python scripts/build_helpers/build.py --type Release --clean
python scripts/build_helpers/build.py --build-dir build-local Release

# run (extra arguments after -- are forwarded to the executable)
python scripts/build_helpers/run.py
python scripts/build_helpers/run.py --type Debug -- --version
python scripts/build_helpers/run.py --model path/to/model.glb -- --demo gui

# Doxygen HTML + LaTeX manual
python scripts/build_helpers/build_docs.py
```

RelWithDebInfo builds go to `build/` (the default: optimised, with line tables), Debug to `build-debug/` and the opt-in Release to `build-release/`; pass `--build-dir` to either helper to override. Windows-only capture, gate and probe scripts live in `scripts/gates/`.

Python also drives the config generators:

```bash
python scripts/config/make_default_config.py     # copy config.example.toml as-is
python scripts/config/make_config.py             # interactive: every setting, with types and defaults
python scripts/config/make_config.py path/to/dir # write config.toml into a directory
```

## Version

**0.7.0** — the single source of truth is `project(VERSION)` in `CMakeLists.txt`. CMake injects `DEREN_VERSION_{MAJOR,MINOR,PATCH}` and surfaces the version through `--version`, the startup banner and the instance's `app_info`. Version-update rules and the synchronized copies are listed in [`AGENTS.md`](AGENTS.md); do not bump the version as a side effect of another change.

Each independently reusable module set carries its `module version` annotation at the separate top main interface unit. That annotation is authoritative, so this README does not mirror the full list. Modules evolve independently: MAJOR for breaking interface changes, MINOR for additive features, PATCH for fixes; an appended `a` marks an internal revision that does not consume a semver slot.

## Source layout

C++ code and shader sources live under `source/`:

- `engine/`: scene, animation, runtime, resources and passes; `engine/filters/` holds filters.
- `backends/vulkan/`: native Vulkan implementation and GUI plugin.
- `promise/`: backend-independent RHI and GUI contracts.
- `app/`, `configuration/`, `gltf_loader/`, `utility/`, `vstd/`: application and supporting modules.
- `tests/`, `shaders/`, `third_party/`: tests, shader inputs and vendored code.

The engine CMake target is `deren_engine` (`deren::engine`). Shader outputs go to `<build>/shaders/`. Documentation and screenshots live in `docs/`; configuration lives at the root; models live under `asset/<name>/` (see [`asset/README.md`](asset/README.md)).

```
source/app/main.cpp          # demo entry point
source/engine/               # runtime, passes, scene, animation, render resources
source/backends/vulkan/      # Vulkan backend (deren_vulkan.dll, deren_gui_vulkan.dll)
source/promise/              # RHI and GUI contracts
source/shaders/              # Slang sources
scripts/                     # setup, build helpers, gates, config, checks, tools
asset/                       # sample models and clouds
docs/                        # guides, screenshots, generated docs (gitignored)
```

## Gaussian splatting

A `.ply` in the `model` key is a Gaussian splat cloud, not a mesh: the extension selects `deren::gaussian::load_ply`, the cloud is packed onto the device once, and a `gaussian` stage draws it over the opaque depth. No model is loaded with it; the frame's only other leaf is a placeholder built by the application.

Measured on the screenshot above (RTX 4060 Laptop, 1280x720): **562,974 splats**, SH degree 3, **135.1 MB** of static tables uploaded once, zero validation errors. Per-frame work runs on the GPU (the CPU prepares instance/camera metadata in about 0.002 ms), and the steady state is **~410 fps**. The session-average GPU frame is **2.50-2.67 ms**, split across prepare, sort, finalize and raster. The per-stage method and its limits are in [`docs/gaussian_splatting.md`](docs/gaussian_splatting.md).

The cloud is checked in under `asset/flowers` (CC BY 4.0, Git LFS). The splat pipeline is engine-level: the managed host
does not expose a splat entry point yet, so the measurement above comes from the `gaussian` pass tests and the previous
native preview, which has been removed.

## Documentation

The project uses **Doxygen**; every module, class, interface and shader is annotated in-source with `@defgroup` / `@brief`. Shader sources are part of the generated reference: `Doxyfile` maps `*.slang` to the C++ parser, so each shader gets a file page under the `shaders` group. Generated HTML and LaTeX are **not** committed.

```bash
python scripts/build_helpers/build_docs.py
```

Output: `docs/html/` (open `docs/html/index.html`) and `docs/latex/refman.pdf`. The LaTeX step needs a TeX distribution (`pdflatex` / `makeindex`). The build script handles the intermittent `epstopdf` failure by regenerating graphs and retrying once.

Development docs:

- [glTF loader usage guide](docs/gltf_loader_usage.md)
- [shader reference](docs/shaders.md): pass chain, shared scene set, push constants, conventions
- [subtilis visibility buffer](docs/visibility_buffer.md) and [hierarchical cluster LOD](docs/subtilis.md)
- [subtilis glTF DAG converter](tools/subtilis/README.md): offline `DEREN_subtilis` package builder
- [subtilis M2/M5 plan](docs/subtilis_m2_m5_plan.md): GPU cut status and streaming design
- [mesh shaders](docs/mesh_shaders.md)
- [Doxygen front page](docs/mainpage.md), [dynamic runtime](docs/dynamic_runtime.md), [conventions](docs/conventions.md)
- [measurement instruments](scripts/measure/README.md): the analysis tools that read saved captures and logs

## Dependencies & Build

### Requirements

- CMake ≥ 4.3 and a compiler with C++26 / C++20 modules support. The reference and CI toolchain is MSYS2 clang64 with clang + libc++.
- [Vulkan SDK](https://vulkan.lunarg.com/sdk/home): `find_package(Vulkan REQUIRED)` resolves against it, and it is one source of `slangc` (`Bin/slangc.exe`). VMA is vendored, not taken from the SDK.
- System packages: `glfw3`, `glm`, `tomlplusplus` (header-only; MSYS2 `mingw-w64-clang-x86_64-{glfw,glm,tomlplusplus}`).
- Everything else is vendored under `source/third_party/`: spirv-reflect, Dear ImGui, xxHash, fastgltf + simdjson, stb_image, mimalloc, Boost.Graph and VRDX.
- **Windows Release runtime set**: `build/deren.exe` imports `libc++.dll` and needs `deren_assets.dll`, `deren_vulkan.dll`, `deren_gui_vulkan.dll`, `shared_utility.dll`, plus the build's compiled shader directory. CI uploads exactly this package.

### Platform

- **Windows x64** is the shipped configuration. Setup uses `scripts/setup/windows.sh` (MSYS2); the Release build runs on a machine with only the GPU driver installed.
- **POSIX (Linux / WSL / macOS)** is source-tree support, not a verified configuration. No CI job configures or builds the project there. Two known obstacles remain: `deren.vstd` is tied to MSYS2 clang64's libc++, and several test translation units include `<windows.h>` unguarded.
- Measured reference platform: **Windows 11, x64**, MSYS2 **clang64**.

### Toolchain

| | version | why |
|---|---|---|
| CMake | **≥ 4.3** | enforced by `cmake_minimum_required` |
| Clang + libc++ + lld | MSYS2 **clang64**, clang **23.1.4** measured | C++26 modules; `deren.vstd` is a trimmed fork of libc++'s generated `std` module |
| Ninja | 1.13.2 measured | generator used by the scripts and CI |
| slangc | **required**, pinned **2026.18.2** | every shader stage is built from `.slang`; CMake fails without it. Pass `-DVR_SLANGC_EXECUTABLE=<path>` or put it on `PATH`. |
| Python | **≥ 3.8** | build helpers, config generators, boundary gate |
| PowerShell | **7** (`pwsh`) | gates and render regression |
| `llvm-nm` | clang-tools-extra, or GNU `nm` | backend-boundary check |

Compiler dialect: C++26, `-fno-rtti`, `-fno-exceptions`, `-Werror`; local Release builds use `-march=native` (CI sets `-DVR_NATIVE_ARCH=OFF`).

### Hardware

Mandatory — the renderer refuses to start without these:

- Vulkan **1.3** device (dynamic rendering is used every frame).
- **`VK_KHR_unified_image_layouts`** (hard requirement).
- **`VK_EXT_host_image_copy`**, with `GENERAL` in the copy-destination layout list (image upload and read-back).
- `VK_KHR_swapchain` and **`bufferDeviceAddress`** (descriptor heap writes and acceleration-structure builds).

Conditional features are queried at startup and degrade by name: ray queries, ray-tracing pipeline/SBT, opacity micromaps, bindless descriptor heap, and mesh/task shaders.

Measured reference device: **NVIDIA GeForce RTX 4060 Laptop GPU, driver 616.92**. The log's capability block reports what the device actually offers; check it before reporting a rendering problem.

### Disk & data

- Each build tree is about **3.2 GB**.
- Sample assets ship in `asset/`. Larger regression character models are not in the repository; put those files under `chars/` next to the executable or point `model` elsewhere.

### Manual build

```bash
cmake -S . -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

`clang-format` runs automatically before compilation; a `clang-format-check` target is available for CI.

## Run

The executable has one launch mode: a managed project. The program walks upward to locate `source/shaders/` and `asset/`.

```bash
./build/deren.exe run /path/to/MyGame      # or build/deren run ...
./build/deren.exe --version                # and the CPU-only tools below
```

The old controls (left-drag orbit, wheel zoom, arrow-key pan, F1 overlay, F12 screenshot) belonged to the native preview
and are gone with it: the scene, its camera and its input bindings belong to the managed game, and a screenshot is written only
when the game requests one.

### Launch a project

Run `deren run /path/to/MyGame` from any working directory. Project launches are C#-driven: the manifest must name the built game assembly, and the game entry owns scene creation, asset loading, node construction and behavior attachment. Put a `deren.toml` in the project root:

```toml
[project]
name = "Helmet Demo"
main_scene = "scenes/main.toml"

[paths]
asset_library = "assets"

[scripts]
assembly = "Game.dll"
```

The entry scene supplies render/path settings only. It must not contain `[[scene.objects]]`; the C# game creates those nodes itself:

```toml
[render]
window_title = "Helmet Demo"
```

For this example, copy the existing `asset/DamagedHelmet` directory, including its `asset.toml`, into `MyGame/assets`. The game resolves asset IDs through the [asset library](asset/README.md). Relative paths are based on the project root, including those in the entry scene. The manifest's asset-library path takes precedence over the scene's path if both are set. The C# game creates every scene node itself; the parser still reads `[[scene.objects]]` for tooling, but the host imports nothing from it.

Project launches validate the manifest and entry scene strictly: an unknown key is an error rather than a silent fallback. The entry scene comes from `[project] main_scene`. There is no native preview mode any more: the executable accepts exactly `deren run <project-directory>`, every startup setting lives in the project's TOML files, and the remaining arguments are handed to the game's `preinit`.

Capture a project with `deren run /path/to/MyGame --capture-frames 120`.

#### C# scripts

The managed host is required on supported 64-bit targets and is built by default; configure with the matching .NET 10 SDK installed. Windows x64 is the shipped configuration. With `VR_BUILD_TESTS=ON`, the build includes a two-helmet demo. Hold A or D to move the left helmet, press Space to toggle the right helmet's rotation, or press E to spawn and remove a third helmet.

One configure and one build produce every piece - the executable, `Deren.Managed.dll`, the demo `Game.dll` and the managed-host tests:

```powershell
cmake -S . -B build -G Ninja
cmake --build build
./build/deren.exe run ./build/source/scripting/test_game
```

For your own project, reference `Deren.Managed.dll` when building your scripts. Set `[scripts] assembly` in `deren.toml` to the built DLL path relative to the project root. The game assembly provides the `game_entry` implementation; it creates the scene tree, loads assets and attaches behaviors to the nodes it creates. Derive behaviors from `deren.behaviour` and override `on_create`, `on_update`, or `on_destroy` as needed. See the [demo game entry](source/scripting/test_game/game_host.cs) and [behaviors](source/scripting/test_game/helmet_controls.cs). Running a scripted project requires the .NET 10 runtime.

Bind named actions to one or more keys in `deren.toml`:

```toml
[input.actions]
move_left = ["a"]
move_right = ["d"]
toggle_spin = ["space"]
```

Use `input.is_down`, `input.was_pressed`, and `input.was_released` in `on_update()`. All scripts in a frame read the same snapshot. A quick tap can report both edges. Key names include lowercase letters, digits, `space`, `left`, `right`, `up`, `down`, `enter`, `tab`, and left/right variants of `shift` and `control`.

### Startup configuration

A launch reads one settings file: the project's entry scene named by `[project] main_scene` in `deren.toml`. It covers paths, window and render settings, shadows, TAA, render scale/upscale, ray tracing, SSAO, clustered lights, IBL, the subtilis switches and the toon profiles. See [`config.example.toml`](config.example.toml) for the annotated key list. There is no `--config` flag and no positional override.

### Reproducible captures

The host passes argv through untouched, so the demo game turns these flags into `startup_options` in `preinit`:

```bash
# render 120 frames, save a screenshot, then quit
./build/deren run ./build/source/scripting/test_game --capture-frames 120

# force the view: yaw,pitch,distance[,target x,y,z]
./build/deren run ./build/source/scripting/test_game --capture-frames 120 --capture-camera 31,0,10,0,-18,0

# move the camera and the animation by frame index
./build/deren run ./build/source/scripting/test_game --capture-frames 40 --capture-sweep 0.5 --capture-animation-sweep 0.02
```

The same values can be set programmatically, which is what a project without a CLI parser does:

```csharp
options->capture_frames = 120;
options->camera_set = 1;                        // yaw, pitch, distance in degrees
options->camera_yaw_degrees = 31;
options->camera_distance = 10;
options->target_set = 1;                        // look at a point instead of the scene centre
options->sweep_yaw_deg_per_frame = 0.5f;        // and/or move the camera by frame index
options->animation_seconds_per_frame = 0.02f;   // and/or advance the animation by frame index
```

The sweep fields advance by frame index, so two runs of one capture are identical. The PNG is written when the game's screenshot request is consumed; it goes to `[paths] screenshot_dir` and the log line prints its full path.

## Captures and comparisons

There is no built-in pixel gate. A capture is a `deren run ... --capture-frames N` invocation (above); comparing two of them is a file diff of the PNGs, which is what [`scripts/measure/diff.py`](scripts/measure/diff.py) prints (signed per-channel means, changed-pixel counts, and a 4x4 tile table).

The old capture harness (`scripts/gates/check_render.ps1` and the scenario scripts around it) drove `deren.exe --config`, a flag the application stopped reading when it moved to the managed host. It and the instruments that wrapped it were retired in #90; the numbers those runs recorded stay in the feature docs under `docs/`.

Windows builds are GUI-subsystem executables: no console window. Logs go to `deren-debug.log`; the previous session is rotated to `deren-debug.log.old`. Panic and crash evidence goes to `deren-crash.log`.

## Shaders

CMake requires `slangc` and regenerates every `source/shaders/*.spv` from its `.slang` source during `cmake --build`. Shared `.glsl` includes are tracked as dependencies, so editing one recompiles the shaders that use it. The compiled directory is mirrored next to the executable, and that is what the runtime loads. `.spv` files are build outputs and are not tracked.

Manual escape hatch (compiles in place, same as the build):

```powershell
powershell -ExecutionPolicy Bypass -File source/shaders/compile_shaders.ps1
```

```bash
sh source/shaders/compile_shaders.sh
```

## License

The engine source code is [MIT](LICENSE), copyright 2026 YzK0741.

The MIT license does **not** cover the third-party sample assets under `asset/`. Each asset keeps its terms, listed in [`asset/README.md`](asset/README.md):

- `asset/DamagedHelmet/` and `asset/FlightHelmet/`: see each folder's `LICENSE.md`.
- `asset/flowers/`: NVIDIA's Bouquet of Flowers, CC BY 4.0; see [`asset/flowers/README.md`](asset/flowers/README.md).
- `asset/laevatain/`: Laevatain / GameToon preview model and Toon sidecar, all rights reserved by Hypergryph Network Technology Co., Ltd. No license is granted; redistribution and reuse are not granted, and whether the files may be redistributed at all is unresolved. See [`asset/laevatain/LICENSE.md`](asset/laevatain/LICENSE.md).
- `asset/tangtang/`: Tangtang / GameToon preview model and Toon sidecar, under the same Hypergryph notice and the same unresolved redistribution status. See [`asset/tangtang/LICENSE.md`](asset/tangtang/LICENSE.md).

Consult those notices before redistributing any file under `asset/`.
