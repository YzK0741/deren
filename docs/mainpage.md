# deren

**deren** is a real-time renderer in modern C++23 (C++20 modules / `.cppm`), built with CMake 4.3 +
Ninja on MSYS2 clang64. The renderer reaches the graphics API through a contract and never through
Vulkan types: the backend is a DLL the executable resolves **by name** at run time, so a second
backend (D3D12, null, ...) is a second DLL implementing the same contract - see
[The dynamic API](#the-dynamic-api) below.

Current version: **0.3.0** - single source is `project(VERSION)` in `CMakeLists.txt`
(surfaced by `--version`, the startup log banner and the instance's `app_info`); bump it there
and keep this line in sync.

## Highlights

The main capabilities and the design choices behind them. Follow the links for
implementation details, measured results and the limits of each feature.

- **Dynamic graphics API.** The engine uses a shared C++ RHI contract and loads the Vulkan
  backend DLL by name at run time. A versioned entry point checks ABI compatibility;
  optional capabilities are queried through extensions. The executable has no graphics-API
  imports. A second backend can implement the same contract; Vulkan is the implemented
  graphics backend today. See \ref md_docs_2dynamic__runtime "The dynamic API", and the normative
  [RHI API standard](rhi_api_standard.md) for every interface/virtual clause.
- **Gaussian splatting.** Trained PLY clouds retain their anisotropic shape, opacity and
  view-dependent spherical-harmonic color. GPU preparation, VRDX stable sorting, cached
  projection/color and indirect draws accelerate the frame; a CPU reference path supports
  comparison and fallback. See \ref md_docs_2gaussian__splatting "Gaussian splatting" for
  setup, validation and measured costs. These trained colors are separate from mesh PBR lighting.
- **PBR with image-based lighting.** glTF metallic-roughness materials use Cook-Torrance
  direct lighting (GGX with Smith visibility and Schlick Fresnel by default), directional
  and punctual lights, and split-sum IBL from diffuse irradiance, a prefiltered specular
  environment and a BRDF lookup table. Normal maps, occlusion and emissive feed the shared
  material and lighting functions. The scene uses a deferred G-buffer path; direct-light
  BRDF/diffuse presets remain selectable. See \ref md_docs_2shaders "Shaders".
- **Environment viewing.** Local HDR presets can be switched while running; diffuse/specular IBL
  and environment backgrounds share rotation and strength. Solid/blurred backgrounds, exposure and
  optional Khronos PBR Neutral tone mapping support material comparisons. See
  \ref md_docs_2environment__lighting "Environment lighting" for controls and asset attribution.
- **Declared pass inputs and outputs.** Each pass describes its resource uses once;
  the declaration layer resolves those uses against the render resources. A composable
  pass chain separates scene rendering, lighting and post processing, with shared named
  pipelines available to its stages. See \ref md_docs_2pass__io__design "Pass I/O design"
  and \ref md_docs_2runtime__split "Runtime split".
  The independent CPU render-graph core adds typed parameter snapshots and visitors, range-aware dependencies, culling and
  stable ordering plus single-queue abstract barriers that the frame's recording path consumes; see \ref md_docs_2render__graph "CPU render graph".
- **Temporal anti-aliasing on a 1x G-buffer.** A Halton(2,3) projection jitter, per-pixel
  camera motion vectors written by the G-buffer, and a reprojected, neighborhood-clamped
  history with a view-depth disocclusion guard. TAA is the engine's anti-aliasing because a
  1x G-buffer cannot be multisampled; it replaces a multisampled path rather than adding to
  one.
- **Cascaded shadow maps plus pass-graph-driven reuse.** One layered 2D-array depth map,
  three cascades by default, practical split scheme with lambda 0.75, per-pixel cascade
  selection blended across the boundary. A slot's cascades are re-rendered only when the
  fitted matrices, the caster world matrices, the uploaded skin matrices or a morph-scratch
  revision changed - the skip is byte-identical by construction.
- **Clustered light culling.** `source/shaders/lighting/light_cluster.slang` sorts up to 128 punctual lights
  into a 64 px-tile x 16-exponential-depth-slice grid once per frame with one atomic counter
  per cluster. The recorded 64-light comparison reduced
  deferred lighting from 0.25 to 0.08 ms with a byte-identical image.
- **Screen-space occlusion with no extra pass.** A golden-angle hemisphere spiral traced
  against the G-buffer depth and normals, folded into `shade_input.ao` so it scales the IBL
  ambient exactly like a baked AO map. No extra pass, no extra render target.
- **Deformation motion vectors.** The previous-frame skin matrices and previous-frame morph
  weights are both stored, so a skinned or morphed vertex reprojects where it actually was.
  See \ref md_docs_2deformation__motion__vectors "Deformation motion vectors" for the
  design, the measurements and the two half-steps it was built in.
- **Mesh shaders.** See \ref md_docs_2mesh__shaders "Mesh shaders" for the geometry
  path, its measurements and the migration that got there.
- **Visibility buffer (experimental).** An alternative opaque producer: a mesh-shader raster writes a
  packed 32-bit surface id plus the frame's shared G-buffer depth, and a compute resolve rebuilds the five
  G-buffer targets from it. It is off by default, falls back per frame for geometry it cannot name, and is
  gated by its own frozen reference set. See \ref md_docs_2visibility__buffer "The visibility buffer lane"
  for the pipeline, the admission rules and the measured cost.
- **Hierarchical geometry (subtilis).** The static opaque cluster-LOD lane is implemented: an offline-style
  builder produces a unique-parent cluster forest, an explicit upload ties it to a resource generation, and a
  deterministic CPU screen-error cut feeds the visibility lane's per-frame meshlet set (default off, with the
  complete LOD0 run as the fallback). See \ref md_docs_2subtilis "subtilis" for the current scope, the measured
  numbers and the closed phase status: DAG+HZB ships, the optional M4 software raster is a recorded
  negative result, and M2/M5 are deferred.
- **amnis texture residency.** `amnis` is the named, opt-in texture-residency feature: visibility and
  screen-footprint feedback drive whole-image, two-tier and progressive mip streaming, with immutable
  source sharing and bounded async uploads. It is independent of subtilis geometry cuts and changes no
  defaults. See \ref md_docs_2amnis "amnis texture residency" for the responsibility boundary, modes,
  subfeatures and measured CPU/memory limits. The current amnis phase closed on 2026-10-28;
  its closeout records the accepted opt-in baseline and deferred work.
- **Ray-traced shadows.** `deren.engine.pass.ray_traced_shadow` sits beside the cascade path; see
  the module documentation and `docs/reference/` for the source notes it was built from.
- **A toon character pipeline.** Per-material families (base / skin / face / hair / eye /
  cloth) select which shader lanes run; sixteen optional sidecar lanes (four in the material
  record - diffuse ramp, shadow LUT, specular ramp, matcap - and twelve more in three toon lane
  blocks, from the face SDF lightmap on) come from a `<model>.glb.toon.tsv` file so a
  model can be tuned without recompiling; the face terminator is a distance field
  thresholded against the light angle **in the head's own frame**, not against N.L. The
  MMD/VMD motion path (IK, retargeting, morph tracks, physics baking) lives entirely in
  `deren.engine.animation.mmd_motion`. A missing sidecar is an empty sidecar - the toon lanes
  simply stay off.
- **Heap-native scene descriptors.** Scene bindings live in a heap the renderer manages.
  Backend integrations such as VRDX sorting and the GUI may use traditional descriptors;
  the sorting wrapper restores heap bindings before subsequent stages, and the GUI's own
  recording segment starts with `command_buffer_begin_info::inherit_descriptor_heaps = false`
  (abi 45) because the Dear ImGui Vulkan backend binds its own classic descriptor sets.
  \ref md_docs_2descriptor__heap__migration "Descriptor heap migration" and
  \ref md_docs_2descriptor__heap__handover "Descriptor heap handover" are the record of
  how it got there and what it cost.
- **Reproducible image and performance checks.** Frozen poses and reference captures,
  CPU/GPU Gaussian comparisons, synchronization validation, resize stress runs and per-pass
  GPU timings make visual changes and costs reviewable. Boundary checks also verify the
  separation between engine and backend. Measurements in this manual retain their device,
  workload and limitations; see \ref md_docs_2gaussian__splatting "Gaussian splatting"
  for a worked acceptance example.

This page is the map: the dynamic API this build is organised around, what is worth reading first,
the order the rest of the manual is written in, and where the generated reference begins. The
reference half of this PDF (module / topic / class / file documentation) is mechanical - it is the
API surface, not the reasoning. The reasoning is in the pages below.

The project was named `vulkan_render` until 2026-10-02. Portable modules and namespaces
now use `deren.engine.*` and `deren::engine`; native Vulkan implementations retain
`deren.vulkan.*` and `deren::vulkan`. Project code is under `source/`, with the engine
in `source/engine/`, backend implementation in `source/backends/vulkan/`, and contracts
in `source/promise/`. `ENABLE_VULKAN_RENDERDOC_CAPTURE` is RenderDoc's own variable.

## How to read this manual

The pages are ordered as a reading path, not alphabetically: the dynamic API and the code rules
first, then the shader contract, the frame structure, the frontier subsystems, the usage guide, and
finally the migration history.

| # | Document | What it answers |
|---|----------|-----------------|
| 1 | \subpage md_docs_2conventions "Conventions" | Naming, file layout and the code rules the rest of the manual assumes. |
| 2 | \subpage md_docs_2dynamic__runtime "The dynamic API" | The contract every backend implements, the loaders that resolve one by name, the run-time package, and how the migration got there. |
| 3 | \subpage md_docs_2shaders "Shaders" | How shaders are written and bound: the pass chain, the shared scene set, the slot conventions. |
| 4 | \subpage md_docs_2pass__io__design "Pass I/O design" | The declared contract a pass states about what it reads and writes, and how it is resolved. |
| 5 | \subpage md_docs_2runtime__split "Runtime split" | How the runtime, the scene tree and the render environment divide the frame between them. |
| 6 | \subpage md_docs_2mesh__shaders "Mesh shaders" | The mesh-shader geometry path: what it replaced, what it measured. |
| 7 | \subpage md_docs_2visibility__buffer "The visibility buffer lane" | An experimental opaque producer: packed id raster plus a compute resolve, its gate and its measured limits. |
| 8 | \subpage md_docs_2subtilis "subtilis (hierarchical cluster LOD)" | The static opaque cluster-LOD lane: asset builder, explicit upload, CPU screen-error cut, measured gains, fallbacks and open milestones. |
| 9 | \subpage md_docs_2amnis "amnis texture residency" | The opt-in texture-residency feature: visibility and screen-footprint demand, whole-image/two-tier/progressive streaming, immutable source ownership, and measured CPU/memory limits. |
| 10 | \subpage md_docs_2deformation__motion__vectors "Deformation motion vectors" | Why a deforming mesh needs more than a camera motion vector, and what was stored. |
| 11 | \subpage md_docs_2gltf__loader__usage "glTF loader usage" | Loading models, animations, skins, morph targets and punctual lights. |
| 12 | \subpage md_docs_2gaussian__loader "Gaussian PLY loader" | Trained PLY conventions, validation, limits and loading diagnostics. |
| 13 | \subpage md_docs_2gaussian__splatting "Gaussian splatting" | Viewer setup, GPU preparation/sorting/cache, rendering rules, measurements and repaired failures. |
| 14 | \subpage md_docs_2slang__migration "Slang migration" | Moving the shaders to Slang and compiling to SPIR-V. |
| 15 | \subpage md_docs_2descriptor__heap__migration "Descriptor heap migration" | Replacing descriptor sets with a renderer-managed heap. |
| 16 | \subpage md_docs_2descriptor__heap__handover "Descriptor heap handover" | How the heap is handed to the passes, and the constraints that imposes. |
| 17 | \subpage md_docs_2migration__tradeoffs "Migration tradeoffs" | What each of those migrations cost, and what was given up for it. |
| 18 | \subpage md_docs_2compiler__tolerance "Compiler tolerance" | What the build needs from a toolchain, what a second one (GCC) measured, and what still stops it. |
| 19 | \subpage md_docs_2unified__image__layouts "Unified image layouts" | One layout vocabulary for render targets, uploads and read-back. |
| 20 | \subpage md_docs_2host__image__copy "Host image copy" | The VK_EXT_host_image_copy path: a texture uploaded without a staging buffer. |

Seven further pages sit in the manual's `INPUT` ahead of this one and are not part of the numbered
reading path: the passchain resource pages (\ref managed_resources "Passchain-managed resources",
\ref md_docs_2resource__clusters "Resource clusters",
\ref md_docs_2render__resource__inventory "Current pass resource inventory"), the CPU render graph and
its RHI mapping audit (\ref md_docs_2render__graph "CPU render graph",
\ref md_docs_2render__graph__rhi__mapping "Render graph to RHI mapping audit"), and the two
support libraries (\ref md_docs_2vessel "vessel", \ref md_docs_2stacktrace "Stack diagnostics").
The `Doxyfile`'s `INPUT` order is the chapter order; the list above is the reading path.

Three shorter paths, if you are not reading front to back:

- **Run it and see a frame:** the README at the repository root for controls and the config
  reference, then \ref md_docs_2gltf__loader__usage "glTF loader usage" for the model
  side.
- **Extend the renderer** (a new pass, primitive strategy or loader):
  \ref md_docs_2pass__io__design "Pass I/O design",
  \ref md_docs_2runtime__split "Runtime split",
  \ref md_docs_2shaders "Shaders".
- **Understand why the code looks like this:** the four migration pages (12-15 above), in
  order.

## The dynamic API

One contract, and every graphics backend is an implementation of it.

- **The contract** is the module `deren.promise.rhi` (`source/promise/rhi/`): the `api_core` virtual
  surface - create and release objects, record commands, walk frames, report abilities - plus the
  creation parameters (`create_info`). OWNERSHIP IS THE CONTRACT'S OWN: the `create_*` factories and
  `image::make_view()` return `rhi::unique_object<T>` (one reference, move-only RAII whose destructor
  calls `release()`), while a handle more than one owner may hold is `rhi::shared_object<T>` (one
  control block whose last copy drops the one factory reference, a nested `weak`, a non-owning
  `borrow()`, and an aliasing constructor for an engine owner keeping a borrowed RHI pointer alive) -
  `std::shared_ptr` no longer manages an RHI handle. Both sides of the boundary compile the SAME
  interface unit, so the two compilations cannot drift.
- **The C ABI is one symbol.** `source/promise/rhi/backend_entry.hpp` declares the single `extern "C"` entry
  the host resolves, `deren_make_api_core`. Its first argument is `rhi::abi_version` (59 today), so a
  mismatched backend is refused BEFORE an object exists, with the backend's own number in the returned
  error; it returns an owning `rhi::shared_object<api_core>` (the contract's own shared container), so there
  is no second destroy entry.
- **Capabilities are queried, not assumed.** `abilities()` says what the device can do and
  `query_extension<T>()` hands out the optional interfaces - the descriptor heap, host
  image copy, the device capabilities (which carry mesh-shader support, the ray-query fact, the push-block limit and the
  acceleration-structure numbers). The engine asks for what it needs and keeps compiling when an answer
  is "no".
- **The backends are DLLs resolved BY NAME** out of the executable's own directory, by absolute path
  (`deren.engine.backend_loader` over `deren.utility.dynamic_link`): `deren_vulkan.dll` exports one
  name and `deren_gui_vulkan.dll` exports one name. The handle is detached and the image is never
  unloaded, so a missing DLL is a named diagnosis rather than a loader error.
- **The GUI plugin has its own number and its own recording targets.** `deren_gui_vulkan.dll` answers
  `deren_make_gui`, whose first argument is `gui::gui_abi_version` (5 today). The device root crosses
  as the opaque `rhi::main_context` (abi 39), and `gui::create_info` carries two contract
  `mergeable_command_buffer_ref` targets the host created; the overlay records its ImGui segment into
  one of them and returns it, and the host executes that segment in a dedicated LOAD + secondary-contents
  rendering instance after the pass's own instance ends. The GUI never records into the frame's command
  buffer and no native type crosses the contract.
- **The run-time package is five images plus the C++ runtime.** Beside `deren.exe`:
  `deren_assets.dll` (glTF/GLB loading, linked through its import library so it loads before `main`),
  `deren_vulkan.dll`, `deren_gui_vulkan.dll` and `shared_utility.dll` (the process-wide log sink),
  with `libc++.dll` and the build's compiled `source/shaders/` directory.
- **Writing a second backend** means implementing `api_core`, exporting `deren_make_api_core` and
  answering with the abi number the contract declares. The executable is not rebuilt.

Three instruments keep that true, and all three measure artifacts rather than prose:
`scripts/checks/check_native_boundary.py` (the executable's import table, the engine objects' unresolved
symbols and the engine sources' vocabulary), `scripts/checks/check_backend_boundary.py` (the symbol ratchet
plus the whitelist) and the per-image checks in CI. Measured at this version: **0** graphics-API
imports in `deren.exe`, **0** unresolved `vk*` symbols in the engine's objects, **0** engine sources
naming the graphics API.

## What is in the box

Most modules are independent building blocks that meet only through narrow interfaces, so
you are free to recombine or rewire them.

- `deren.vstd` - the project's STL module (modified from libc++ and trimmed to the project's
  usage; consumed as `import deren.vstd;`, module version 0.1.1a - see `source/vstd/README.md`)
- `deren.vulkan.core` - instance / device / swapchain / VMA allocator / pipeline / descriptor
  plumbing
- `deren.engine.scene_tree` - pure-CPU scene storage (transform hierarchy of scene_node objects
  with abstract primitive leaves)
- `deren.engine.primitive` - the GPU primitives (normal / instanced / static draws) plus the
  material / camera / light UBO records of the GPU scene set
- `deren.engine.runtime` - the frame facade (per-frame-slot scene resources, granular frame
  phases: poll_events -> recreate_if_minimized -> pace_and_acquire -> begin_recording ->
  record_main_drawcalls -> end_recording -> submit_and_present, plus one-call
  render_frame()), drives the peer scene_tree / primitive modules, debug GUI overlay.
  Shadow + main pass commands are recorded into per-slot `mergeable_command_buffer` segments and
  merged into the frame's primary in one `command_buffer::execute(span)` batch; the main pass
  fans its leaf recording out over the shared task pool (task_pool batches), each recording
  worker getting its own render_environment (thread-local pipeline-bind state)
- `deren.engine.render_environment` - per-recording-session render state: the session's command
  buffer, the available named pipelines (pointer to the runtime's stable name table), the
  session's default pipeline and a deduplicated binder (std::function, injected by the
  runtime) that primitives call through draw(render_environment&). Holds no Vulkan module
  dependency.
- `deren.engine.animation` - animation::controller: glTF keyframe playback / skinning / morphs on
  the runtime scene tree (heavy animations fan per-source sampling over a small
  deren.utility:thread_pool), plus the MMD/VMD motion path in `deren.engine.animation.mmd_motion`
- `deren.gltf_loader` - pure-CPU glTF/GLB loading: meshes, keyframe animation, skins, morph
  targets, cameras and punctual lights (KHR_lights_punctual); world-AABB + loader
  diagnostics
- `deren.chores` - demo bootstrap helpers for main(): startup config analysis (config + argv
  merge, source/shaders/model location), pipeline setup, instancing stress grid, shader loading
- `deren.utility` - log/panic, handle distribution, thread pool (deren.utility:thread_pool), BVH, data
  blocks, frame_clock, and the `vessel` containers' mimalloc-backed allocator (`memory::allocator`)
  Binary file readers require an explicit `max_bytes` payload limit and return
  `std::expected<container, file_error>` with the failing stage and available system/stream cause.
  Empty files are valid, reads are binary and complete, and observed length changes are rejected;
  they do not provide a concurrent-edit snapshot. Shader bootstrap uses a 64 MiB limit.
- `deren.app_config` - TOML startup configuration merged with argv

Modular composition is the point, not a side effect:

- `deren.gltf_loader`, `deren.app_config` and `deren.utility` are **pure CPU with no Vulkan dependency** -
  standalone libraries that embed into any host application (`deren.gltf_loader` and its toon-material
  sidecar build into `deren_assets.dll`, so they can also be consumed as one shared image);
- `deren.engine.animation` is **format-neutral and runtime-agnostic**: it drives whatever scene
  storage a caller injects through the `backend` surface and initializes from any loader
  whose data satisfies the structural `source` concept (it imports no loader and no
  `deren.engine.runtime`);
- `deren.vulkan.core` / `deren.engine.runtime` are a configurable facade (`create_info`, granular
  per-frame phase calls) - the demo entry point (`main.cpp` + `deren.chores`) is a thin glue layer
  on top and can be replaced wholesale.

Use the modules as-is to extend this renderer (new pass / primitive strategy / loader) or
link only the ones you need into your own project.

Module reference is grouped under the `vulkan_core`, `runtime`,
`engine_scene_tree`, `engine_render_environment`, `engine_render_resource`,
`engine_pass`, `engine_animation`, `vulkan_gui`, `engine_math`, `gltf_loader`, `chores`,
`utility` and `app_config` groups; the GLSL / Slang shaders are collected under the
`shaders` group (see the shader reference page for the pass chain, the shared scene set and
the conventions).

## Rendering

PBR (Cook-Torrance + image-based lighting), directional shadows with manual
percentage-closer filtering, skybox, keyframe animation / skinning / morph playback, and a
Dear ImGui debug overlay that is on by default (`[gui] show = false` in config disables it).

The frame is also **measured**: with `[render] gpu_timings` (on by default) each pass
boundary writes a GPU timestamp into a per-frame-slot query pool, and the values are read
back after the slot completed and averaged over one wall second (logged once per window and
shown in the overlay); a whole-session average is logged when the runtime shuts down. Every
rendering feature below is steered by those numbers rather than by guesswork.

The renderer was evolved to a deferred pipeline one milestone at a time, and **the G-buffer
path is now the ONLY scene path** - the forward one was removed in commit `78b6737` ("the
G-buffer path is the only scene path, and the forward one is gone") - so everything below
describes what the engine does rather than what it is becoming. **M1 (done)** is the
G-buffer: the opaque pass stores the surface (albedo/metallic, world normal/roughness,
material id/AO/flags) in three 1x targets, with a channel debug view. **M2 (done)** is the
deferred lighting stage: `source/shaders/lighting/deferred.slang` shades every pixel from those targets and
adds the result into the HDR target (sky where no geometry wrote depth), through
`shade_surface()` - the SINGLE lighting entry point (`source/shaders/lighting/shading.slang`) that every
shading stage in the engine now calls - with emissive added by the base pass and the
material-surface gather shared in `source/shaders/common/surface.slang`. While the forward path still
existed the two were A/B references and agreed to 0.32/255 mean absolute luminance
difference on Sponza; that number is a HISTORICAL measurement, not a current comparison -
there is no longer a second path to compare against, the shared shading code simply IS the
path. **M3 (done)** is temporal anti-aliasing on that path: a Halton(2,3) projection jitter,
per-pixel camera motion vectors written by the G-buffer, and `source/shaders/post/taa.slang` resolving
the jitter against a reprojected, neighborhood-clamped history (with a view-depth guard for
disocclusions) - measured against the same frame without TAA, 1.05/255 mean difference with
9% less high-frequency energy, and, as the measurement was taken at the time, the
then-still-present forward path byte-identical. TAA is the engine's anti-aliasing because a
1x G-buffer cannot be multisampled. **M4 (done)** is cascaded shadow maps: the sun's shadow
pass fills a layered 2D-array depth map (1..4 cascades, three by default), each cascade
fitting its own light-space box to its own slice of the view range (practical split scheme,
lambda 0.75), with the fragment shader selecting its cascade per pixel from the view depth
and blending across the boundary - one cascade is byte-identical to the pre-M4 single-map
path. **M5 (done)** is clustered light culling: `source/shaders/lighting/light_cluster.slang` (the engine's
first compute pipeline, on the graphics queue) sorts up to 128 punctual lights into a 64
px-tile x 16-exponential-depth-slice grid once per frame with one atomic counter per
cluster, and the shading stage loops only its own cluster's list - measured with 64 lights,
the forward shading pass drops 1.28 -> 0.44 ms and the deferred lighting pass 0.25 -> 0.08
ms with a byte-identical image, since the cluster test is conservative. **M6 (done)** is
screen-space ambient occlusion in the deferred lighting stage: a golden-angle hemisphere
spiral traced against the G-buffer depth and normals, folded into the `shade_input` ao so it
scales the IBL ambient exactly like a baked AO map, with no extra pass or render target and
an off path that is byte-identical to the pre-M6 frame. **M7 (done)** is the collation pass:
the shadow map size became a config knob, the documented example config is parsed and pinned
by a unit test, and the configuration / GUI / reference documentation cover every milestone.
**Shadow-map reuse** (the first optimization that reads the pass graph instead of shrinking
it) followed: a slot's cascade maps are re-rendered only when the fitted matrices (a refit
counter) or the caster geometry (an XXH3-64 fingerprint of every caster's world matrix, of
the uploaded skin matrices and of a morph-scratch revision) changed since that slot last
rendered them, so the skip is byte-identical by construction - a skinned caster keeps a
constant world matrix, which is why the skin upload has to be part of the signal or an
animated model silently keeps a frozen map. Still ahead on this path: alpha-blended geometry
in the deferred path (it is composited outside the G-buffer and so writes no motion vector
at all), and anything else that moves inside its own object space. What a DEFORMING mesh
needs is done: the previous-frame skin matrices and the previous-frame morph weights are
both stored, so a skinned or morphed vertex reprojects where it actually was - see
\ref md_docs_2deformation__motion__vectors "Deformation motion vectors" for the design,
the measurements and the two half-steps it was built in.

Resource ownership and dynamic pass requirements are described in \ref managed_resources; the
[pass inventory](render_resource_inventory.md) lists the current GPU access methods.
