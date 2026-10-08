# Recording-face refactor: goals, progress, blocking state, and next steps

> Working note (2026-10-07). **STATUS: twelve batches have LANDED.** The build compiles, and
> every gate in section 6 passes on this machine: build 0, `ctest` 19/19, `clang-format-check` 0, the
> boundary gate 0 symbols, the spike 95 checks / 0 failed, `test_runtime_dyn` 10 / 0, and the render
> gate **matched 14 / mismatched 0 (exit 0)** - re-run after EACH batch, not once at the end.
>
> PLUS ONE GATE THAT IS NOT IN THE LIST, because no scenario reaches it: the ray-traced shadow path is
> smoked by hand (`[render] rt_shadows = true`, 40 frames, validation on) and it reports **0 VUIDs** - it
> did not before the third batch, and the sixth batch's new escape slots are proven by it (the SBT query
> travels through `api_basis` there, and the smoke run still builds its table).
>
> **IT IS RED AS OF THE TWELFTH BATCH, AND THE BASELINE SAYS IT IS NOT THE BATCH'S FAULT**: the run panics
> with `device_lost` ("waiting the frame slot's timeline failed") when the trace executes, and **HEAD
> (`ddbda33`) reproduces the identical failure** when checked out and rebuilt - with ~3.5 GB of host memory
> free, the environmental condition this session already met twice (`Failed to create image: -2`,
> `LLVM ERROR: out of memory`). The batch's own instruments are green (the probe's read-back values are
> byte-identical, the render gate is 14/14). Re-run this smoke when the host has memory before attributing
> anything to a diff.
>
> THE NUMBERS THE EFFORT IS MEASURED BY: engine-side `vkCmd*` call sites **116 -> 3**, **none of them
> in `vulkan/pass/`** (the THREE left are documented escape-bucket sites - see 2.15/2.16/2.17); the
> ESCAPE's own demand, measured for the first time in the seventh batch: **105 -> 69 engine-side call
> sites**. Engine files including a Vulkan header **62 -> 35** (and two modules, eadback and
> ay_traced_shadow, dropped their include outright); abi **20 -> 25**, pinned in both tests. (The call-site count is not monotone: the fifth batch's probe fix
> ADDED three dynamic-state calls - trap 12 - because honesty about an instrument means counting what it
> measures.)
>
> THIS NOTE IS THE COMMIT-MESSAGE-LENGTH VERSION of all six batches: section 2 is what changed,
> section 5 is the failure class the first batch closed, section 8 is what is left AND the measurement
> that ranks it. Sections 1, 3, 4 and 7 are the parts that stay true for the next batch.
>
> Reading order for someone picking this up: section 1 (why), section 2 (what changed, per file),
> section 4 (the recipes the migrated files follow), section 5 (what was wrong and how it was found),
> section 6 (how to verify), section 7 (traps that already cost time here), section 8 (what is left).

## 1. Goals: the rulings, in the order they were given

The end state the user asked for, stated so it can be checked rather than interpreted:

1. **One recording type: `command_buffer`.** `command_list` is DELETED, not deprecated. Everything a
   caller records with is declared once, on the one handle a caller holds:
   `use`, `copy_image_to_buffer`, the GPU timing pair (`begin_gpu_timing`, `mark_gpu_timing`),
   `begin_rendering` / `end_rendering`, `bind_pipeline` / `bind_vertex_buffer` / `bind_index_buffer`,
   `draw` / `draw_indexed`, `dispatch` / `draw_mesh_tasks` / `draw_mesh_tasks_indirect`, the five
   dynamic-state verbs (`set_viewport`, `set_scissor`, `set_cull_mode`, `set_depth_write`,
   `set_depth_bias`), `barrier` (group and single image), `copy_image`, `copy_buffer`,
   `clear_color_image`. No borrowed-view type, no second spelling of any verb.
2. **Ownership is a smart pointer; a raw pointer never expresses ownership.**
   `api_core::make_command_buffer(command_buffer_desc const&) -> std::shared_ptr<command_buffer>`;
   the control block's deleter is the contract's own drop (`release()`), so no call site spells
   `release()` and no second lifetime rule exists. `render_environment` holds
   `std::shared_ptr<rhi::command_buffer>`.
3. **The engine and the pass layer name no native resource.** `resolved_io` and `pass_host` hold
   `rhi::command_buffer*`; `api_core::begin_commands()` returns `command_buffer*`;
   `submit(command_buffer&)`, the heap face's `heap_push_info.commands`, the escape's
   `native_command_buffer(command_buffer&)`, `mesh_shader::dispatch_mesh(command_buffer&)`,
   `build_acceleration_structure(command_buffer&)` and `trace_rays(command_buffer&)` all speak it.
4. **The pass layer holds no `VkDevice`.** Each pass keeps `rhi::api_core* built_against` - the
   contract's virtual base - and asks it to build what it needs. `pass_context::device` survives
   ONLY because the pipeline builders still take a device; when they stop, that field goes with them.
5. **Then the sweep.** Engine-side `vkCmd*` call sites: 116 -> 0. Engine files including a Vulkan
   header: 62 -> at most 7 (the escape bucket: the frame-level raw steps, the third-party overlay,
   the function-pointer-resolved mesh / ray-tracing entries, the probe's own path).

## 2. What has landed in the working tree (uncommitted)

### 2.1 Contract: `promise/rhi/`

| change | detail |
|---|---|
| `command_list` deleted | the type, its `interface_id`, both constructors, its destructor |
| the series moved | every verb above is now a pure virtual on `struct command_buffer : object` |
| `recording()` deleted | its return type no longer exists; the buffer IS the recorder |
| `begin_commands()` | now returns `command_buffer*` (the frame slot's borrowed buffer) |
| `submit(...)` | now takes `command_buffer&` |
| `make_command_buffer()` | NEW virtual returning `std::shared_ptr<command_buffer>` (the abi 21 half) |
| `<memory>` | added to the module's global module fragment (it was not visible; `shared_ptr` is now spelled in the interface) |
| `interface_type::command_list = 10` | RETIRED in place: the enumerator and its number stay (never renumber), with a comment saying the face was absorbed |
| `rhi.extension.cppm` | `dispatch_mesh` / `build_acceleration_structure` / `trace_rays` / `native_command_buffer` take `command_buffer&`; `heap_push_info.commands` is `command_buffer*`; the forward declaration is now `struct command_buffer;` |

Measured diff sizes at the time of writing: `rhi.api_core.cppm` +121/-215, `rhi.extension.cppm`
+7/-7, `rhi.contract.cppm` +1/-1.

### 2.2 Backend: `vulkan/core/`

- **`frame_commands`** (the frame's own buffer, which the CORE owns) now derives from
  `command_buffer` instead of `command_list`, and gained the four BORROWED lifecycle verbs:
  `release()` refuses with a ONE-TIME named log (the `frame_image_slot` precedent - a caller that
  wrapped this view had a bug), and `begin_recording` / `end_recording` / `execute` answer
  `unsupported` by name, because the frame loop owns those steps. Its ~20 series implementations were
  already there and did not change.
- **`owned_command_buffer`** (a buffer the CALLER owns, made by the factory) now reuses the existing
  `frame_commands` implementation by deriving from it, and lost its redundant `frame_commands list`
  member. Its own lifecycle (`release`, `begin_recording`, `end_recording`, `execute`) is unchanged.
  This is why no 20 delegating overrides exist: the series has exactly one implementation.
- **`core::make_command_buffer()`** (declaration + definition) calls the existing
  `create_command_buffer()` and wraps the result: empty on refusal, otherwise
  `std::shared_ptr<rhi::command_buffer>(raw, [](rhi::command_buffer* p) noexcept { if (p) p->release(); })`.
- Type-only changes in the same two files: `heap_commands(core&, command_buffer*, ...)`,
  `core::begin_commands()`, `frame_escape::native_command_buffer(command_buffer&)`,
  `core::submit(command_buffer&)` and its identity comparison (still against `&commands_view`).

### 2.3 Engine

- `vulkan/render_environment/render_environment.cppm`: the session's target is
  `std::shared_ptr<deren::promise::rhi::command_buffer>`; `#include <memory>` added; the three
  injected callbacks (`bind`, `set_depth_write_fn`, `set_cull_mode_fn`) take that `shared_ptr`, so
  their call sites (`this->bind(this->command_buffer, ...)`) are unchanged.
- `vulkan/pass/pass.cppm`: `resolved_io::list` and `pass_host`'s list are `command_buffer*`;
  `pass_context::device` carries a comment saying it exists only until the pipeline builders stop
  taking a device.
- 15 passes (`cluster`, `compute_skin`, `deferred`, `fxaa`, `geometry_buffer_debug`, `goo_rim`,
  `mask_bake`, `megalights_temporal`, `megalights_trace`, `post`, `ray_traced_shadow`, `shadow`,
  `taa`, `toon_screen_rim`, `upscale`): each `.cppm` declares
  `deren::promise::rhi::api_core* built_against = nullptr;` in place of its `VkDevice device` member
  and imports `deren.promise.rhi`; each `.cpp`'s create path now guards on `context.face == nullptr`,
  rebuilds when `this->built_against != context.face`, and assigns `this->built_against = context.face`.
- `runtime/runtime.frames.cppm` (5 sites: `begin_commands()` x4 including the `if`-init form, plus
  `native_handle()` which now hands the buffer itself to the escape), `runtime/runtime.cpp` (1),
  `vulkan/readback/readback.cpp` (1): all speak `command_buffer`.

### 2.4 Tests

- `tests/probe_backend.cpp`: `probe_command_list` and `probe_command_buffer` are merged into ONE
  `probe_command_buffer` that implements both the series and the lifecycle; the probe implements
  `make_command_buffer` with a NO-OP deleter (its buffer is a static member, so the control block must
  not delete it); `native_command_buffer` / `begin_commands` / `submit` signatures converted.
- `tests/spike_backend_boundary.cpp` and `tests/test_dynamic_link.cpp`: `foreign_command_list` is
  deleted and folded into a single `foreign_command_buffer` that answers `invalid_argument` to
  everything, which is what both questions (`submit`'s list, `execute`'s buffer) need.
- `command_list` now appears in no test file.

### 2.5 Deliberate leftovers

`command_list` survives in exactly two places, both on purpose: a backend comment stating that the
type is GONE, and the retired `interface_type` enumerator (`rhi.contract.cppm:374`).

### 2.6 What THIS batch added on top of 2.1-2.4

| area | what landed |
|---|---|
| the 15 passes of the old section 5 | `deferred`, `geometry_buffer_debug`, `goo_rim`, `toon_screen_rim`, `taa`, `scene`, `shadow`, `transparent`, `character_forward`, `cluster`, `megalights_temporal`, `megalights_trace`, `ray_traced_shadow`, `post`, `fxaa`, `upscale`: every barrier (single and `barrier_group`), rendering scope, `set_cull_mode`, draw, dispatch, image copy, push endpoint and secondary lifecycle now goes through `io.list` / the contract begin info. Their `.cppm` frame structs carry the contract spellings (`segments` of `segment_buffer{command_buffer*}`, `cascades` of `command_buffer*`, `secondary` as a `shared_ptr`, all four `make_environment`s taking the session's shared_ptr). |
| `mask_bake.cppm/.cpp`, `compute_skin.cppm/.cpp` | their record entries take `rhi::command_buffer&` (their push callbacks too) and dispatch through the contract; the compute bind and the skin job's global memory barrier were raw through `pass::native_commands` in the FIRST batch and are contract verbs in the second (see 2.7). The runtime's structure hooks reach the frame's contract buffer by IDENTITY (`frame_command_buffer()` checked against the native they were handed) instead of widening the ray-tracing module. |
| `runtime/*` | `frame_command_buffer()` (non-owning shared_ptr over the core's borrowed view) + `native_frame_commands()`; `frame_services::cmd`, the three `ensure_*_sampled`, `make_*_environment`, the push/mesh endpoints and `record_shadow_cascade` all speak the contract; the secondaries (`secondary_command_buffers`, `main_segments`, `shadow_recording`) are `std::shared_ptr` from `make_command_buffer()`; `record_shadow_content` takes the session's shared_ptr. |
| `render_environment.cppm`, `primitive.cpp`, `readback.cpp` | the four mesh/push endpoints take `rhi::command_buffer&` (the runtime converts once, inside); the primitives pass `*env.command_buffer`; the read-back's one-shot buffer IS the recorder. |
| `vulkan/core/*` | `frame_commands` lost its `final` (owned buffers derive from it); `frame_commands::execute` RECORDS (`vkCmdExecuteCommands` on the frame's own buffer, provenance-checked) instead of refusing; `begin_rendering`'s attachment array is 8, not 4 (the scene carries five colours + depth); `owned_command_buffer::begin_recording` derives the descriptor-heap inheritance from the core's own heap state. |
| `tests/*` | the merged `foreign_command_buffer` / `probe_command_buffer` (lifecycle + series, `make_command_buffer` with a no-op deleter), `test_pass`'s contract stand-in for `cmd`, and the identity/provenance assertions the deleted borrowed view used to carry. |

### 2.7 What the SECOND batch added (the pipeline migration, the vocabulary, abi 21)

| area | what landed |
|---|---|
| the contract | `pipeline_desc::compute_code` (appended, `struct_size`-guarded: `first_stage == compute` selects it and the attachment fields are ignored); `barrier_group::stage` (`stage_hint`: which shader stage a pair's shader side ran at); `barrier_group::has_memory` + `memory_barrier` and the appended `buffer_use::acceleration_structure_read` (the one global barrier in the engine); `abi_version = 21`. |
| the backend | `core::create_compute_pipeline` (the heap flag, the NULL layout, one stage from `compute_code`, entry point "main", `bind_point = COMPUTE`) and the `sanitize_pipeline_desc` guard for the new field; `barrier(barrier_group)` honours the stage hint through `with_stage_hint` (shader bits replaced, transfer bits and the access half kept) and records the global memory barrier in the same `vkCmdPipelineBarrier2`; `buffer_masks_for` gained the acceleration-structure READER. |
| `pipelines.cppm` | the six `vkCreateComputePipelines` builders (`mask_bake`, `compute_skin`, `cluster`, `heap_probe`, `two_set_compute`, `resolve_pipeline`) became contract factories: build a descriptor, `face.create_pipeline`, take the native through the escape. `build_rt_shadow_ray_tracing` stays raw (no ray-tracing pipeline spelling - section 8). |
| the pass framework | `owned_pipeline` gained `contract` beside `pipeline`; `resolved_io` gained `pipeline_handles` (index-aligned with `pipelines`, NULL meaning "no contract spelling"); `declaration_pipelines_ok` fills both; the runtime's `resolve_pipeline` hands both over. **SUPERSEDED BY 2.9: the fourth batch deleted the raw lane and left ONE run of contract handles, because the second lane had no consumer once the runner's fallback went.** |
| the bind sites | the RUNNER binds through `bind_pipeline` wherever a contract handle exists and keeps the raw bind only for the ray-tracing assembly; `cluster.cpp` no longer binds at all (it declares its pipeline and the runner binds it); `mask_bake.cpp` / `compute_skin.cpp` bind their own pipelines through the contract (contract-only: a job whose pipeline has no handle is a wiring bug and is reported). |
| the two measured sites | `ray_traced_shadow.cpp`'s two barriers are `barrier_group{... .stage = stage_hint::ray_tracing}`; `compute_skin.cpp`'s build-ordering barrier is `barrier_group{.has_memory = true, .memory = {shader_write -> acceleration_structure_read}}`. `pass::native_commands()` keeps ONE user class: the resolved ray-tracing launch entry point. |
| the include sweep | the four files that named no native type at all (`frame_constants.cppm`, `character_forward.cpp`, `render_start_demo.cpp/.cppm`) dropped `#include <vulkan/vulkan.h>`: 62 -> 58 engine files. Section 8.4 explains why the rest is a migration rather than a delete pass. |

| the include sweep | the four files that named no native type at all (`frame_constants.cppm`, `character_forward.cpp`, `render_start_demo.cpp/.cppm`) dropped `#include <vulkan/vulkan.h>`: 62 -> 58 engine files. Section 8.4 explains why the rest is a migration rather than a delete pass. |

### 2.8 What the THIRD batch added (the ray-tracing pipeline, the lane audit, constant_init)

| area | what landed |
|---|---|
| the contract | `shader_stage` gained the five ray-tracing VALUES (`ray_generation`, `miss`, `closest_hit`, `any_hit`, `intersection` - appending a value is not an abi change); new `ray_tracing_stage` (stage + SPIR-V + name), `ray_tracing_group` (the stage INDICES a general/hit group binds, `shader_group_none` for the empty slots, `triangles` for a hit group's geometry) and `shader_group_none`; `pipeline_desc` gained `ray_tracing_stages` / `ray_tracing_groups` / `max_ray_recursion`, where NON-EMPTY stages is the whole switch that selects the third path. |
| the backend | `core::create_ray_tracing_pipeline`: one module per stage, the group-type DERIVED from which slots are filled, the heap flag, the entry point resolved through `vkGetDeviceProcAddr` (the loader exports no extension command), `bind_point = RAY_TRACING_KHR`. `create_shader`'s stage switch learned the five new values (a switch that fell through to the vertex default would have been a lie). |
| `pipelines.cppm` | `build_rt_shadow_ray_tracing` became a contract factory: the stages in the SBT's own order (raygen, miss, closest hit, any hit) and the three groups (0 = general raygen, 1 = general miss, 2 = triangles hit naming stages 2 and 3), with the count still returned to the caller. `vkCreateRayTracingPipelinesKHR` and every `VkRayTracing*` struct left the file. |
| the runner | the raw-bind FALLBACK IS GONE: every pipeline this renderer builds is a contract pipeline now, so `apply_pass_behaviour` binds only through `bind_pipeline`, and a declared pipeline with no contract handle is a named log rather than an unbound draw. |
| the lane audit (the bug this batch found) | `resolve_declaration`'s "the pass's OWN pipeline" branch filled only the RAW lane, so EVERY pass that owns its pipeline (deferred, cluster, taa, fxaa, upscale, post, goo_rim, the debug view, the two megalights stages, shadow, rt_shadow) published a null contract lane. The runner's fallback had hidden it; deleting the fallback turned it into `vkCmdDraw(): a valid GRAPHICS pipeline must be bound` (5+ VUIDs in a 40-frame run). The fix is a new `frame_pass::pipeline_handle()` (the contract lane of the same object `pipeline()` answers) overridden by those 13 passes, plus `post_composite_pass::named_pipeline` and `post_composite_pass::resolve` filling their contract lanes - and `declaration_pipelines_ok` now ASKS `named_pipeline` FIRST, which is what makes that accessor real (it was declared, overridden and never called). |
| `constant_init` | a DEVICE-QUERY section: `make_properties_2(next)` / `make_features_2(next)` (the two chain heads) and the four chained blocks (`make_acceleration_structure_properties`, `make_ray_tracing_pipeline_properties`, `make_ray_query_features`, `make_mesh_shader_features`), used by the acceleration-structure module and the runtime's three device queries. The project's own rule is "the engine never hand-fills these structs at call sites"; a query chain is where that rule pays for itself, because the whole chain is tagged members. |
| the second real bug | `vulkan/acceleration_structure/acceleration_structure.cpp`'s property query chained a `= {}` struct whose `sType` was never set - and `VK_STRUCTURE_TYPE_APPLICATION_INFO` IS ZERO, so validation reported an APPLICATION_INFO in the pNext chain (2 VUIDs). It is trap 8's second measurement, and it was invisible until this batch because NO gate enables `rt_shadows` (section 6's smoke run is what found it). |

### 2.9 What the FOURTH batch added (the raw pipeline lane retires, and the sweep it unlocks)

| area | what landed |
|---|---|
| the pass framework | `frame_pass::pipeline()` (raw `VkPipeline`) is DELETED: one accessor, `pipeline_handle()`, answering the contract object. `owned_pipeline` is now `{ rhi::pipeline* contract; }`, `resolved_io` has ONE pipeline run (`pipeline_storage` / `pipelines`, a span of contract handles) instead of the raw and contract pair, and `resolve_declaration` publishes from `pipeline_handle()` in one branch. 13 pass pairs lost their `pipeline()` override; the 9 guards that tested the raw lane now test `== nullptr`. |
| the shadow callback | `shadow_frame::record_cascade` carries the CONTRACT handle, and the runtime's session records with contract verbs: `bind_pipeline`, `set_viewport`, `set_scissor`, `set_depth_write`, `set_depth_bias` replaced five raw `vkCmd*` calls (that is why the census's call sites fell 53 -> 48). |
| the include sweep | 18 files dropped `#include <vulkan/vulkan.h>` because the raw lane was the LAST Vulkan token in them: 12 `.cppm` (cluster, deferred, fxaa, geometry_buffer_debug, goo_rim, megalights_temporal, megalights_trace, post, shadow, taa, toon_screen_rim, upscale) and 6 `.cpp` (cluster, fxaa, megalights_temporal, megalights_trace, taa, upscale). Engine files with a Vulkan header: **58 -> 40**. Each was MEASURED before the line was deleted (code-only token scan, comments stripped; no `vk*` call, no VMA, another include left in the global fragment). |
| THE BUG THIS BATCH EXPOSED | `core::frame_commands::set_depth_bias` called `vkCmdSetDepthBias(cb, clamp, slope_factor, constant_factor)` - ALL THREE ARGUMENTS WRONG, under a comment claiming it was "Vulkan's own argument order" (Vulkan's is `constant, clamp, slope`). It had exactly ONE caller and that caller did not exist until this batch: the shadow path used to call `vkCmdSetDepthBias` directly with the right order. Routing the shadow bias through the verb took the render gate from 14/14 to **matched 2, mismatched 12**, and fixing the order took it back to 14/14 - a one-line backend bug found by the only instrument that could see it (trap 11). |

### 2.10 What the FIFTH batch added (the device lane and the raw RAII lane retire together)

| area | what landed |
|---|---|
| the last raw builder | `build_heap_probe_graphics` became a contract factory (`make_graphics_pipeline`), so `vkCreateGraphicsPipelines`, the two raw `vkCreateShaderModule` calls and the hand-built `VkGraphicsPipelineCreateInfo` left the engine. It was the ONE user of the whole raw lane, which is why the lane could then go. |
| the raw lane | `make_shader_module_raw`, `shader_module_handle` (and its `release_contract_shader`), `pipeline_handle`'s `raw_device` / `(VkPipeline, VkDevice)` constructor / `destroy_raw` (the engine's only `vkDestroyPipeline`) are DELETED. `pipeline_handle::native` survives as an ESCAPE READ that one caller still needs (the RT pass's SBT handle query) and nobody owns. |
| the device lane | `pass_context::device` and `VkDevice` on ~26 builder declarations/definitions are gone (the WHOLE batch of builders takes `rhi::api_core&` alone - not one of them read the device any more). `frame_services::device` went with it (no reader). The 17 pass call sites and both probe callers dropped the argument. |
| where a device is still needed | exactly ONE pass: the ray-traced shadow, for two ALLOCATED ENTRY POINTS (`vkGetDeviceProcAddr` for `vkCmdTraceRaysKHR` and for the SBT handle query). It reaches them through the escape via the new `pass::native_device(face)`, the sibling of `pass::native_commands` - the contract's own documented answer, and the reason `pass_context` needs no device at all. |
| THE BUG THIS BATCH EXPOSED | the probe's own draw path lost three dynamic-state calls the contract's graphics pipelines REQUIRE: the raw builder had baked the 4x4 viewport, the scissor and `CULL_MODE_NONE` in as STATIC state, while `create_pipeline`'s graphics recipe declares all three DYNAMIC (the render_environment states them per draw). Render gate: **0 matched / 14 mismatched** and 66 VUIDs (`Dynamic viewport(s) ... were not provided`, the same for scissor, and `VK_DYNAMIC_STATE_CULL_MODE ... never called vkCmdSetCullMode`). Three `vkCmd*` calls in the probe fixed it - and that is why the batch's call-site count went 48 -> 51 (see trap 12). |

### 2.11 What the SIXTH batch added (`api_basis`: the basic handle travels as a tagged token, abi 22)

| area | what landed |
|---|---|
| the contract's token | `rhi::api_basis`: **no interface at all** - no virtual, no ownership, no data beyond `s_type`. It is NOT a base of `api_core` and NOT a base of the backend's own type; a backend COMPOSES one and hands it out. `structure_type` gained the appended VALUE `vulkan_device_basis` (a value moves no abi; the three escape slots below are what moved it). |
| `vulkan_escape` (abi 22) | three APPENDED slots: `get_basis()` (the token), `device_proc(api_basis&, char const*)` (resolve an allocated entry point against the token's device), `shader_group_handles(api_basis&, pipeline const&, first, count, out)` (the SBT query - the one device fact a pass cannot ask the contract for). All three answer null/false rather than reading a foreign token. |
| the backend | `core` gained `basis_token` (an empty struct whose whole content is its tag) + `get_basis()` + `owns_basis()` (a TAG check, because this build is `-fno-rtti`), and `frame_escape` implements the three slots: `device_proc` resolves through `vkGetDeviceProcAddr` on `logical_device`, `shader_group_handles` through `vkGetRayTracingShaderGroupHandlesKHR` on the pipeline's native handle. The handle itself never leaves the backend. |
| the pass layer | `pass::native_device(face)` (a `VkDevice` returned into pass code) is REPLACED by `pass::device_basis(face)` + `pass::device_proc(...)` + `pass::shader_group_handles(...)`, and `ray_traced_shadow.cpp` now names **no `VkDevice`, no `VK_SUCCESS` and no `vkGetDeviceProcAddr` at all** - it holds the launch function pointer and the SBT regions, which is exactly the part that stays native. |
| the probe | `tests/probe_backend.cpp`'s device-less `probe_escape` answers `nullptr` / `false` to the three new slots - the honest answer for a probe with no device, and the same answer that makes an engine path take its documented "unavailable" branch. |

### 2.12 What the SEVENTH batch added (the escape's first real shrink, and what actually blocks it)

The batch started from a MEASUREMENT of the escape's remaining demand: **105 engine-side call sites**
(`runtime/**` + `vulkan/**` outside the backend; the probe/spike fakes use it deliberately and are not
demand), in five classes. This batch took the part that the contract ALREADY had a shape for.

| area | what landed |
|---|---|
| the contract was already ready | `heap_image_write_info` has always been contract-spelled (`image const* resource` + `image_view_desc const* view` + `descriptor_type`), and the engine already had a CONTRACT overload `contract_write_heap_image(face, offset, rhi::image const&, rhi::image_view_desc const&, descriptor_type)` beside the transitional native one. Four IBL writes were already using it. |
| the white fallback texture | its heap write went through the CONTRACT overload (`*owned_textures.back()` + `image_view_desc{}` + `sampled_image`), so the hand-built `VkImageViewCreateInfo` and the `native_image()` read that fed it are gone. **The render gate matched 14/14 byte-for-byte afterwards**, which is the proof that `image_view_desc{}` (all remaining layers/mips) and the hand-written struct described the same view. |
| DEAD NATIVE CACHE | `runtime::texture_array_views` (`std::vector<VkImageView>`) is DELETED: nothing ever read an element - every use was `.size()` (which is `owned_texture_views.size()`) - and its two pushes were the file's only `native_image_view()` reads. The indices that were counted off it are now counted off the contract vector (`size() - 1` where the push follows, with the ordering note where each is computed). |
| dead accessors | `mask_bake_job::pipeline()` and `compute_skin_job::pipeline()` (raw `VkPipeline`, **no callers**) are DELETED, the same shape `frame_pass::pipeline()` had before batch 4. |
| the include sweep | `vulkan/pass/compute_skin.cpp` now names NO Vulkan type, macro or entry point at all and dropped its include: engine files with a Vulkan header **40 -> 39**. |
| the numbers | engine-side escape call sites **105 -> 102**; the heap/recording class **68 -> 65**. |
| WHAT BLOCKED THE REST (measured, not guessed) | (1) `pipelines::pipeline_handle::native` still has TWO readers - the two PROBES' raw `vkCmdBindPipeline`, because a probe records into its own raw pool/command buffer, so `command_buffer::bind_pipeline` cannot be called there yet; retiring that lane is the probes' rewrite. (2) The probe's `vkCmdDrawMeshTasksEXT` -> `mesh_shader::dispatch_mesh` is blocked by the same fact: `dispatch_mesh(command_buffer&, ...)` needs a contract command buffer, which the probe does not have. (3) The remaining 65 heap/recording sites hang off the FRAME RESOURCE TABLE's raw lanes (`resolved_binding::view/image`), which is the frame sweep. (4) RT's launch needs SBT REGIONS the contract's `trace_rays(commands, w, h, depth)` does not carry - a genuine contract GAP, not a migration. |

### 2.13 What the EIGHTH batch added (the probes record through the contract - and what that turned up)

| area | what landed |
|---|---|
| the probes' recording | BOTH probes (`run_heap_probe`, `run_heap_graphics_probe`) record through the contract now: `make_command_buffer` + `begin_recording` (a helper, `runtime::make_probe_commands`) instead of a hand-made `VkCommandPool`/`VkCommandBuffer`; the heap bind and the push through `descriptor_heap::bind` / `push_data` instead of the raw helpers; the rendering scope, the dynamic state, the pipeline bind and the draw through the record series; `draw_mesh_tasks` instead of a resolved `vkCmdDrawMeshTasksEXT` (`vkGetDeviceProcAddr` gone); `barrier(image_barrier)` instead of the raw UNDEFINED->GENERAL transition; `wait_idle()` instead of a fence. **Census: `vkCmd*` 51 -> 41.** |
| THE BACKEND BUG THIS EXPOSED | `heap_commands` (the helper behind `descriptor_heap::bind` / `push_data`) refused EVERY command buffer except the frame's own borrowed view (`commands != &owner.commands_view` -> `invalid_argument`). The heap verbs' `commands` field is a CONTRACT `command_buffer*` with no frame restriction, and `frame_commands::native()` answers for both shapes (the frame's slot buffer or an owned buffer's, per `target`) - so the guard was stale, and its consequence was **silent**: `[[nodiscard]] bool pushed = heap->push_data(...)` at the shadow cascade (converted in this batch) and the probes' own bind would have been DROPPED with no image difference to notice. Fixed: any buffer this backend handed out is accepted, by the same cast convention the escape's other native accessors use. |
| the probe readbacks | VERIFIED IDENTICAL to the pre-rewrite values, line for line: the compute probe still reads `0xffffffff` (texture red 0xffff, alpha 0xffff; material default record 0xffff), and the MESH/GRAPHICS arms still read `rgba 255,255,255,255` at the known slot with the deliberately WRONG slot (16897) reading `rgba 0,0,0,255` - the negative proof that the index selects the descriptor. |
| what stays raw in the probes, and why | TWO sites, both documented rather than worked around: (1) `submit_probe_commands` - `api_core::submit()` is spelled, in its OWN contract doc, for "the list `begin_commands()` handed out" (the FRAME's), and the backend refuses anything else by name, so an ISOLATED probe buffer reaches the queue through the escape (with the WAIT staying `wait_idle()`); (2) the graphics probe's HOST-READ barrier - the contract's `image_use` census ends with "no HOST-ACCESS masks ... the two host-visible barrier sites stay in the escape bucket (runtime.probes.cppm / ray_tracing.cpp)", so this is that site. The missing vocabulary is now recorded in the declaration itself: a `submit_nowait(owned buffer)`-shaped verb would let both probes drop their last native handle. |
| dead code | `pipelines::pipeline_handle::get_pipeline()` DELETED - its last two readers were the probes, which now bind the CONTRACT object. `native` stays: `begin_pipeline` (the runner's raw path for the geometry pipelines, a frame-sweep site) still reads it. |
| the shadow cascade | its push moved to `descriptor_heap::push_data` with the CONTRACT secondary, so `record_shadow_cascade` no longer derives a native handle at all - and that conversion is what turned the `heap_commands` bug from a latent trap into a caught one. |
| ONE ENVIRONMENTAL RED HERRING, RECORDED | the first full gate run came back 12/14 with two scenarios panicking on `Failed to create image: -2` (out of device memory) - with the build itself reporting `LLVM ERROR: out of memory` in the same window. Attribution, not assumption: the SAME revision at HEAD failed the same way, and the scenario passed when run alone after the pressure cleared (the final full run is 14/14). NO code in this batch is implicated; the lesson is that a host under memory pressure can fail a scenario in a way no diff explains, so attribute before bisecting. |

### 2.14 What the NINTH batch added (the runner's raster bind, and the last native pipeline lane)

| area | what landed |
|---|---|
| `pipeline_handle` | its `native` field, its `get_pipeline()` accessor and the EIGHT `vulkan_escape::native_pipeline()` reads that filled it are DELETED: the runner's raster bind was the last reader. The constructor is `(rhi::pipeline*)` alone, and the cached dynamic state is the CONTRACT's vocabulary now (`rhi::viewport` / `rhi::rect`). |
| `begin_pipeline` | takes `rhi::command_buffer&` and states the bind + the two pieces of state as the record series' verbs (`bind_pipeline` / `set_viewport` / `set_scissor`). The runner's `env.bind` lambda no longer derives a native handle at all - the session it is given IS the contract buffer. |
| the runner's resync and depth-write | the per-pass `resync_viewport` block states `rhi::viewport`/`rhi::rect` and calls the two verbs on `io.cmd` (its `VkCommandBuffer` derivation is gone with the last raw `vkCmdSetViewport`/`vkCmdSetScissor`), and the transparent session's `set_depth_write_fn` is `command_buffer::set_depth_write` instead of `vkCmdSetDepthWriteEnable`. |
| the moved state sites | `runtime.cpp` (3 sites), `runtime.frames.cppm` (2) and the two post-ish passes (`toon_screen_rim.cpp`, `goo_rim.cpp`) assign the contract PODs to the cached state instead of `VkViewport`/`VkRect2D`. |
| the numbers | `vkCmd*` **41 -> 35** (7 distinct spellings left, all in the frame loop's barrier/clear/rendering sites and `readback`); engine-side escape calls **101 -> 94**; `vulkan/pipelines/pipelines.cppm` now names NO Vulkan type, macro or entry point, so it dropped its include: engine files with a Vulkan header **39 -> 38**. |
| WHAT REMAINS IN THE FRAME LOOP | the ~20 `vkCmdPipelineBarrier2` sites (hand-written stage/access mask pairs, which need a mask -> `image_use`/`buffer_use` role translation per site - the contract's own `image_use` census was DERIVED from exactly those recipes, so the mapping is documented but per-site), plus `vkCmdBeginRendering`/`vkCmdEndRendering` (`runtime.cpp` 2 + the frame loop 2), one `vkCmdClearColorImage` (the furnace clear), one raw `vkCmdSetCullMode` in each environment's cull callback (the `render_environment` setter still speaks `VkCullModeFlags` - converting it touches the passes' env too), and `readback.cpp`'s one-shot copy + its buffer barrier. |

### 2.15 What the TENTH batch added (the frame loop's barriers - `vkCmd*` 35 -> 6)

| area | what landed |
|---|---|
| the frame loop's barriers | ALL 17 hand-built `VkImageMemoryBarrier2` + `vkCmdPipelineBarrier2` sites in `runtime/runtime.frames.cppm` became CONTRACT calls, each one recipe -> one role pair: `undefined_to_transfer_dst_transition` -> (undefined, transfer_destination), `transfer_dst_to_sampling_transition` -> (transfer_destination, shader_read), `shadow_map_sampling_transition` -> (depth_attachment, depth_read), `undefined_to_depth_sampling_transition` -> (undefined, depth_read), `color_attachment_transition` -> (undefined, color_attachment), `hdr_sampling_transition` -> (color_attachment, shader_read), `undefined_to_sampling_transition` -> (undefined, shader_read), `present_transition` -> (color_attachment, present). The multi-image sites (`record_scene_attachments`, `ensure_gbuffer_targets_sampled`, the post G-buffer off batch) are ONE `barrier_group`, the shape the raw call had. Every `escape().native_image(...)` that fed them is gone: the resources are the CONTRACT images. |
| clears, scopes, cull | the furnace cube's clear is `clear_color_image(*furnace_cube_image, {1,1,1,1}, 6 layers)`; both missing-set fallbacks clear through the same verb (their hand-built attachment + begin/end pairs are deleted); `runtime.cpp`'s two `vkCmdBeginRendering` pairs are `begin_rendering(rendering_info)` built from the contract views; `record_scene`'s empty instance closes with `end_rendering()`; both environment cull callbacks map `VkCullModeFlags` -> `rhi::cull_mode`. |
| `ray_tracing.cpp` | `structure_set::build`/`update` TAKE THE CONTRACT BUFFER now (`rhi::command_buffer& commands`), deriving the native handle ONCE for the allocated build entry points - which is what lets the two build-ordering memory barriers ride the contract: the mask-bake one reuses the pair `compute_skin.cpp` established, and the top-level one needed a NEW APPENDED VALUE, `buffer_use::acceleration_structure_write` (a value, so no abi bump; the backend's `buffer_masks_for` maps it to AS_BUILD + `ACCELERATION_STRUCTURE_WRITE_KHR`). The frame loop's two call sites pass `*frame_command_buffer()`. |
| `readback.cpp` | LEFT RAW, and that is a decision with a reason: `readback::read(VkBuffer, ...)` is a raw-handle utility BY DESIGN (raw fence, raw submit, `native_buffer_of`), its barrier's source is the deliberately conservative ALL_COMMANDS/MEMORY_WRITE pair that the contract's buffer roles cannot spell (`undefined` would emit a weaker source), and `copy_buffer` needs the SOURCE as a contract `buffer` its API does not take. |
| dead parameters | `barrier_image_to_sampling(VkCommandBuffer, VkImage)` became `barrier_image_to_sampling()`: it reads the frame's HDR image from its own state, and the two parameters were already ignored inside (`static_cast<void>`). A signature that promises arguments and reads none is a lie the next reader has to check. |
| ONE BEHAVIOUR CHANGE, DECLARED | the `ensure_*` helpers no longer consume their "written" flag when a barrier is REFUSED (the contract call can fail by name; the raw call could not) - a later sampler retries instead of the frame silently keeping an unpublished image. Every other refusal is logged. |
| the numbers | `vkCmd*` call sites **35 -> 6** (3 spellings left: the probe's host-read barrier, `readback`'s buffer barrier + copy, and `ray_tracing.cpp`'s two remaining barriers - see below), engine-side escape calls **94 -> 69**, includes unchanged at 38. |
| WHAT THE 6 ARE | the three DOCUMENTED escape-bucket sites (the probes' HOST_READ barrier, `ray_tracing`'s HOST_WRITE micromap barrier, and `readback`'s conservative buffer barrier + copy), plus the two `ray_tracing` micromap barriers whose roles (`micromap_write`/`micromap_read`) the contract's `buffer_use` does not spell - adding them is a vocabulary decision, not a migration. Everything else in the frame is contract. |

### 2.16 What the ELEVENTH batch added (the SBT region becomes an RHI type; abi 23)

| area | what landed |
|---|---|
| the contract's type | `shader_binding_table_region`: three numbers - the device address of a region's first record, the bytes it spans, and the stride between records. It lives in `:extension` (NOT `:api_core`) because it is the vocabulary of an ABILITY's verb and `:api_core` imports `:extension` rather than the reverse - the same reason `descriptor_type` and the heap write PODs are there. It is a FROZEN by-value POD, and `address == 0` is the "no records" spelling (Vulkan DEREFERENCES the region pointer, so an empty table is a zeroed region and never nullptr). |
| the verb's SHAPE | `ray_tracing::trace_rays` now takes the four regions (raygen, miss, hit, callable) plus width/height/depth. THE OLD SHAPE WAS REPLACED, NOT APPENDED TO, and that is what moved `abi_version` 22 -> 23: it was `trace_rays(commands, width, height, depth)`, which cannot describe a launch at all, and it had NO implementer and NO caller - a second overload would have left a verb nobody can carry out standing next to the one they can. |
| what the engine gained | `rt_shadow_pass`'s four region members are the CONTRACT type now (they were `VkStridedDeviceAddressRegionKHR`, which put a Vulkan type in a pass's own state for no reason - the data is a device range, not a driver structure), and the ONE conversion to the driver's struct happens at the launch, field for field. |
| what did NOT change, and why | THE BACKEND STILL DOES NOT SERVE THE VERB: `core::abilities()` does not announce `ray_tracing`, and a set bit is a promise about service - its other three verbs (create/build/address of an acceleration structure) are served by the engine's own `vulkan/ray_tracing` module through the escape today. So the launch stays the ONE raw site in that pass and `pass::native_commands` keeps its single user; the pass's wrapper and `pass::native_commands`' own doc now say exactly that (the vocabulary is no longer the reason - the SERVICE is). Serving the ability is a design step (implement all four methods in the backend, i.e. move acceleration-structure creation out of the engine module), not a migration. |
| micromap roles | `buffer_use` gained `micromap_write`/`micromap_read` (two VALUES - no abi change), mapped in the backend to MICROMAP_BUILD/MICROMAP_WRITE_EXT and ACCELERATION_STRUCTURE_BUILD/MICROMAP_READ_EXT, and `ray_tracing.cpp`'s SECOND micromap barrier rides the contract now (`vkCmd*` 6 -> 5). THE FIRST ONE STAYS RAW: its source is HOST_WRITE, and the enum carries no host role - the split the contract's own `image_use` census records ("the host-visible barrier sites stay in the escape bucket"). |

### 2.17 What the TWELFTH batch added (read-back becomes CONTENT, not a handle: `image::get_content`; abi 24-25)

| area | what landed |
|---|---|
| the launch rides the RECORDING FACE | `command_buffer::trace_rays(raygen, miss, hit, callable, w, h, d)` (tier-1 append, abi 24), served by the BACKEND against the `vkCmdTraceRaysKHR` pointer it resolves ONCE at startup (`core::ray_trace_launch`). The `ray_tracing` ability LOST its copy of the launch (a verb reachable only through an ANNOUNCED ability is unreachable on a backend that serves the recording face without having frozen that ability's acceleration-structure shapes - which is this backend's state, and those shapes are still the S1 design surface). `vulkan_escape::device_proc` - whose last caller was that launch - is DELETED, and `pass::native_commands` / `pass::device_proc` with it. |
| the SBT and its numbers | `shader_binding_table_properties` (handle size / handle alignment / base alignment) joins the region type in the contract, so `pass_context` no longer holds `VkPhysicalDeviceRayTracingPipelinePropertiesKHR`; and the upload hook speaks the contract (`buffer_flags` in, `rhi::buffer*` out, `uint64_t` address). The result: **`vulkan/pass/ray_traced_shadow.{cppm,cpp}` names NO Vulkan type at all and dropped its `#include <vulkan/vulkan.h>`**. |
| read-back is CONTENT now | `image_content{extent, bytes_per_pixel, bytes}` + `bytes_per_pixel(image_format)` + `image::get_content(region)` (abi 25): the backend performs the copy with `vkCopyImageToMemoryEXT` (`VK_EXT_host_image_copy`, already a REQUIRED capability here) and answers the image's bytes - **no staging buffer, no copy command, no submission**. The probe's read-back moved from "query the ability, size a vector, build the region, call" to ONE call, and its log values are byte-identical (the instrument: `255,255,255,255` for the right slot, `0,0,0,255` for the wrong one). |
| the FRAME image, HONESTLY refused | `frame_image_slot::get_content()` answers `error::unsupported`, WITH the measurement in its note: a swapchain image can only be host-copied when the surface listed `VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT` (the swapchain usage must be a subset of `supportedUsageFlags`), and **on every surface this renderer has been run on that bit is ABSENT** - the constructor already logs it. So the screenshot's copy-command path stays, and the reason is the SURFACE, not a gap in the backend. |
| the dead utility retired | `vulkan/readback/**` (the staging + export-fence + raw-submit read-back) had **no callers left** - the runtime stopped importing it in S2 batch 2 - so the module is DELETED, and with it 7 of the engine's `vk*` references (`vkCmdCopyBuffer`, `vkCmdCreateFence`/`Destroy`/`Reset`/`Wait`, `vkQueueSubmit`, `vkCmdPipelineBarrier2`). |
| bug found BY THE INSTRUMENT | the AS-to-AS build-ordering barrier (`ray_tracing.cpp`'s top-level ordering) was reusing `buffer_use::acceleration_structure_read` - a value whose backend access is `SHADER_READ` (it was measured for "a build reads compute-written vertices"). The raw pair that site replaced had `ACCELERATION_STRUCTURE_READ_KHR`, so the two builds were **not ordered against each other**. The value now carries BOTH access bits (a DESTINATION mask can only ever order more), with the measurement in the mapping's comment. |
| the numbers | `vkCmd*` 5 -> **3**; engine files 82 -> **80**; engine files with a Vulkan header 37 -> **35**; engine-side `Vk*` types 64 -> fewer by `VkStridedDeviceAddressRegionKHR`/`VkPhysicalDeviceRayTracingPipelinePropertiesKHR`/`VkBufferUsageFlags`/`VkDeviceAddress`/`VkBuffer` across the RT pass and the hook. |
| THE ONE RED, AND WHY IT IS NOT THIS BATCH | the `rt_shadows` smoke run (the ONLY config with RT on - none of the fourteen frozen scenarios enable it) panics with `device_lost` when the trace executes. **HEAD (ddbda33) reproduces the identical failure** (measured: `git checkout` to HEAD, full rebuild, same 0xC0000409 + `waiting the frame slot's timeline failed`), with only ~3.5 GB of host memory free - the same environmental condition this session already met twice (`Failed to create image: -2` + `LLVM ERROR: out of memory`). The instruments that DO cover this batch are green (probe bytes identical, render 14/14). Attribution recorded rather than guessed: the launch's recorded arguments were verified (command buffer, four regions, dims) and the SBT's own handles read back non-zero; the run is red at HEAD with the same config. |

### 2.18 What the THIRTEENTH batch added (the native-boundary gate, and the frame's recording lifecycle)

| area | what landed |
|---|---|
| THE GATE (this is the point) | `scripts/check_native_boundary.py` implements the three checks the exit plan is judged by, in order of strength: **P-Import** (`objdump -p deren.exe` must not name `vulkan-1.dll`), **P-Nm** (`llvm-nm --undefined-only` over `vulkancorekit`'s objects must find no `vk*`), **P-Census** (the engine's sources must name no `Vk*`/`VK_*`/`vk*`/`escape()->native_*`/`#include <vulkan/`). The ENGINE SCOPE is parsed out of `CMakeLists.txt` - `deren_vulkan`'s own sources plus the static libraries it links - so a file that joins the DLL leaves the gate's scope in the same edit, and a deleted module's STALE OBJECT is skipped rather than reported. `--require-zero` turns it into a failing gate; without it, it prints the remaining work. |
| the baseline it reports | **33 engine sources** with graphics-API vocabulary, **6 engine objects** with unresolved `vk*`, **1 import** (`vulkan-1.dll` in `deren.exe`) - the last one is the goal itself, and the second is what the linker would complain about the day `Vulkan::Vulkan` leaves the engine's link libraries. |
| the frame's recording lifecycle | `runtime::begin_recording` and the frame's close now call `command_buffer::begin_recording()` / `end_recording()` instead of `vkBeginCommandBuffer`/`vkEndCommandBuffer` with a hand-built `VkCommandBufferBeginInfo`. THE BACKEND'S TWO VERBS WERE REFUSALS UNTIL NOW ("the FRAME LOOP begins the frame's recording, not a pass") and now SERVE the frame's own buffer: the API's state machine behind a recording belongs to the recording face, and the rule the refusal protected is still true and now the caller's to keep (a pass never reaches these verbs - the runner opened the recording it records into). The usage bits map exactly as the owned-buffer form maps them, and `render_pass_continue` is refused BY NAME for a primary. |
| the numbers | engine objects with `vk*` **6 -> 5** (the frame loop's two entry points are gone), that file's vocabulary 39 -> 36, gate findings 40 -> 39. **NO abi bump**: the contract's two verbs already existed - this batch changed who serves them, not their shape. |
| verification | render gate **14/14** (the frame's open/close is exactly what it exercises: fourteen scenarios, each screenshotted), `ctest` 19/19, `clang-format-check` 0, the boundary gate 0 symbols, spike 95/0, runtime_dyn 10/0, and the probe's read-back values unchanged. |

### 2.19 What the FOURTEENTH batch added (two small, self-contained shavings)

| area | what landed |
|---|---|
| the device wait | `runtime.cpp`'s shadow-array shrink called `vkDeviceWaitIdle(native_device_of(face))`; it calls the contract's `wait_idle()` now. The engine was borrowing a device handle from the backend in order to tell the backend's device to be idle - which is exactly the call the contract already had a verb for. |
| device ADDRESSES are numbers | `mask_bake_request` / `compute_skin_request` carried `VkDeviceAddress` fields. THAT TYPE IS `uint64_t` (the API typedefs it), so the only thing the spelling bought was a graphics-API name in a pass's own struct - and the contract already spells a device address as `uint64_t` in `shader_binding_table_region`. Both request structs and the bake's address-splitting lambda are `std::uint64_t` now; no conversion exists anywhere, because there is nothing to convert. |
| the numbers | `runtime.cpp`'s object 4 refs -> 3 (`vkDeviceWaitIdle` gone); `vkCmd*`-style token lists unchanged otherwise; engine objects still 5, gate findings 39 -> 39 (this batch moved VOCABULARY, not link references, in the two pass files). |
| verification | render gate 14/14, ctest 19/19, clang-format-check 0. NOTE that the mask/skin bake paths these two files serve are RT-only (`rt_mask_bake`/`rt_skin_bake`) and therefore run in the rt_shadows smoke alone - which is red for the environmental reason 2.17/§1 record. The change is a type substitution between two names of the SAME type, so the compile is the whole risk; that is why it is grouped here rather than billed as a migration. |

### 2.20 What the FIFTEENTH batch added (the DEVICE'S FACTS become a tier-2 ability; no abi bump)

| area | what landed |
|---|---|
| the shape, and why it is an ability | `rhi::device_capabilities` is the SEVENTH ability (`extension_kind::device_capabilities = 1u << 6`, `interface_type::device_capabilities = 0x106`). It went through the EXISTING extension mechanism - `abilities()` announces it with a bit, `query_extension()` hands the object back - rather than becoming a `facts()` method on `api_core`, and the difference is measured: a new ability moves NO existing vtable, so this batch needs **no `abi_version` change**, while a `api_core` method would have renumbered the tier-1 interface. It is announced UNCONDITIONALLY because every method is answerable the moment the device exists (a device with no mesh shader ANSWERS `false` - that is an answer, not an unserved ability). |
| the seven methods | Each one is read by the engine today, and each one replaced an engine-side query: `mesh_shader()` and `ray_query()` (each was "is `VK_KHR_*` in the escape's enabled list" AND a `vkGetPhysicalDeviceFeatures2` chain - re-derived at SIX call sites, several per frame), `max_push_constants_size()` (was a whole `VkPhysicalDeviceProperties` fetched for one integer, at two call sites), `graphics_queue_family()` (was a WALK of the device's queue families with `vkGetDeviceQueue` per graphics family, comparing each queue against the escape's own handle, to RECOVER a family index the backend had chosen), `shader_binding_table()` (was a second `VkPhysicalDeviceProperties2` chain in `runtime.constructor.cppm`), and `acceleration_structure_scratch_alignment()` / `max_acceleration_structure_instances()` (were a third chain, in the acceleration-structure module). NOT ONE OF THE SEVEN QUERIES ANYTHING: they read the members the backend's constructor filled while it decided what to enable - so an answer cannot disagree with the enabling it describes. |
| the trap THIS batch paid for | the view needs `capabilities_view.owner = this` like every other view in that constructor, and the omission is NOT silent: every method answered its zero/false default, `mesh_shader()` came back false, and the app could not start at all - the render gate reported it in the words of the thing it broke ("pipeline 'pbr' has no mesh stage to build from, and its vertex form is gone"). Recorded in section 7. |
| the numbers | engine objects referencing a Vulkan symbol **6 -> 5**, references **13 -> 7** (the whole `vkGetPhysicalDeviceFeatures2`/`Properties`/`Properties2`/`QueueFamilyProperties`/`vkGetDeviceQueue` family is gone); `runtime/runtime.constructor.cppm`'s vocabulary 55 -> 46, `runtime/runtime.cpp`'s 28 -> 23; `device_extension_enabled` (its two callers) and `physical_properties_of` are deleted, and with them the two `VK_*_EXTENSION_NAME` macros the engine half used to name. |
| verification | render gate **14/14**, ctest 19/19, clang-format-check 0, boundary gate 0 symbols, spike, runtime_dyn 10/0. |

### 2.21 What the SIXTEENTH batch added (the mesh dispatch stops re-resolving what the backend already resolved)

| area | what landed |
|---|---|
| the claim that kept the engine resolving it | `runtime.declarations.cppm` carried two `PFN_vkCmdDrawMeshTasks*` members, and the paragraph that justified them said the contract's mesh verb "records into a contract list" while "these two call sites hold a RAW `VkCommandBuffer`". BOTH HALVES WERE FALSE, and neither had been checked: the mesh path records through the same contract `command_buffer` every other draw uses (the probes do it; the pass framework hands the verb the frame's borrowed buffer), and the BACKEND resolved the same two entry points at ITS startup to serve `draw_mesh_tasks()` / `draw_mesh_tasks_indirect()`. The engine's pair was a second resolution of ONE entry point, kept alive by the raw handle the call borrowed back - the exact shape this effort removes. |
| what replaced it | `runtime::draw_mesh_tasks` gates on the capability (`device_capabilities::mesh_shader()`, asked through the ability that owns it) and records with `command_buffer::draw_mesh_tasks(x,y,z)`; `draw_mesh_tasks_indirect` writes its slot and calls the contract's `draw_mesh_tasks_indirect(argument_buffer, offset, count, stride)`. Gone with them: two `PFN_` members, two `reinterpret_cast`s, `mesh_indirect_table` (the derived `VkBuffer` - whose null is what silently sent every dispatch down the direct path in the first version of that seam), every `native_handle()` borrow in the mesh path, and `sizeof(VkDrawMeshTasksIndirectCommandEXT)` at three sites. |
| the record layout becomes the contract's | the caller WRITES the argument buffer, so the shape of one record cannot live only in the driver's structure: `rhi::mesh_task_command` (three counts) and `rhi::mesh_task_command_size` = 12 join `mesh_task_command`'s own doc, with a contract `static_assert` tying the size to the stride callers pass - and the BACKEND asserts its `VkDrawMeshTasksIndirectCommandEXT` against both (it is the only side that names the two). A new POD + a constant move no vtable: **no abi bump**. |
| the instrument that says it worked | the route the seam has logged since it was built: **`mesh indirect: the meshlet dispatches go through the contract's draw_mesh_tasks_indirect (argument buffer bound, ...)`** and, at shutdown, **`mesh indirect: 40 meshlet dispatch(es) went through the INDIRECT entry point, 0 through the direct call`** - so the migrated verb carries every meshlet dispatch and the fallback is untouched, which a screenshot alone could not say. |
| the numbers | engine objects with a Vulkan symbol **5 -> 4** (the constructor's `vkGetDeviceProcAddr` is gone), gate findings 39 -> 38; the engine's remaining 5 references are 3 `vkGetDeviceProcAddr` (the AS/micromap allocation entry points - the S1 shape) and the probes' two (plan X4). |
| verification | render gate **14/14**, ctest 19/19, clang-format-check 0, boundary gate 0 symbols, spike 95/0, runtime_dyn 10/0, probe read-back values unchanged. |

### 2.22 What the SEVENTEENTH batch added (the acceleration-structure probe - plan S1's P0)

| area | what landed |
|---|---|
| why a NEW instrument instead of the smoke | the repository's only end-to-end ray-tracing instrument is `render-check/rt_smoke.toml`: the full Sponza scene, a 1080x960 frame and the whole pass chain. That is the right acceptance for the RENDERER and the wrong instrument for a MIGRATION - it needs hundreds of MB (so it fails on a memory-short host for reasons unrelated to the code under test, measured at HEAD), and it answers one bit for a whole frame. |
| the probe | `tests/test_acceleration_structures.cpp`: a three-vertex triangle and ONE instance, a hidden 64x64 window, **validation ON**, and no scene data at all. It drives THE SAME MODULE SURFACE the migration touches - `bottom_level_structures::add/record_build/record_update` and `top_level_structure::begin/add/record_build` - so it keeps its meaning on both sides of the change. |
| what it checks separately | the CREATE + SIZES path (two entries, one refittable), the module's documented SKIP (a no-triangle source keeps the caller's index alignment and answers a null handle), the BUILD path (a recorded build that is actually SUBMITTED and WAITED on, which a record-only check cannot say), the REFIT path (`record_update`, which reuses the build's scratch - the path most likely to break when scratch stops being the engine's), and the top level (one instance). |
| it also measured plan X4's second item | the probe creates a CALLER-OWNED contract command buffer and submits it through the escape with a fence, because the contract has no submit verb for a buffer the caller owns - the same raw pair `runtime.probes.cppm` needs. That gap is now visible in an instrument instead of only in the plan. |
| the first green run | `[test_acceleration_structures] 31 checks, 0 failed -> PASS`, **0 VUIDs** with validation on, and its own lines report the facts: `mesh_shader=true ray_query=true max_push_constants=256 graphics_queue_family=0` (the ability from 2.20 answering on a real device), `geometry vertex_address=0xca40000 index_address=0xca40100 triangles=1`, `bottom level built: 2 geometries, 2 triangles, 3968 bytes of scratch`, `built, submitted and waited: 3 structures, 1 instances`. |
| how it is wired | SPIKE-SHAPED like `test_runtime_dyn` (a real device, so it is NOT in the headless ctest set - ctest stays 19/19), `--with-device` required, `add_dependencies(... deren_vulkan)` so the DLL it resolves exists. |

### 2.23 What the EIGHTEENTH batch added (the acceleration structure becomes TIER-1 furniture; abi 25 -> 26)

| area | what landed |
|---|---|
| THE DECISION, AND WHY IT REVERSES THE OLDER ONE | on the user's ruling, an acceleration structure is NOT an ability: an ability answers "can this backend serve this optional feature", and to a renderer that has acceleration structures they are a RESOURCE the caller creates, reads an address from and destroys - exactly what `buffer` and `image` are. So `acceleration_structure` joins `rhi.api_core`: the object (with `release()`, `device_address()`, `size_bytes()`, `write_instances()`), the description (`acceleration_structure_desc` with its `struct_size` guard, `type`, `flags`, geometries OR instance capacity), the two geometry/instance PODs, and `interface_type::acceleration_structure = 12`. |
| the verbs | `api_core::create_acceleration_structure(desc)` (**appended**, tier-1 factory) and the RECORDING FACE's `command_buffer::build_acceleration_structure(target)` / `refit_acceleration_structure(target)` (**appended**). It was these two APPENDED SLOTS on two existing interfaces that moved the number - the new types and the new `interface_type` value move nothing on their own, which is the same rule the host-image-copy batch recorded. |
| WHO OWNS THE MEMORY | the BACKEND, and that is the whole point of the tier-1 shape (option B): the storage the structure lives in, the scratch a build needs, its alignment and the per-geometry offsets never appear in the contract, so a caller cannot depend on them and a second backend with no explicit acceleration structures at all can answer the two recording verbs however it must. |
| the `ray_tracing` ABILITY IS RETIRED | its three acceleration-structure verbs are DELETED and its struct is gone: their operands were never more than FORWARD DECLARATIONS in the extension file, so no backend could ever have served it (its own note admitted as much), and the launch had already left (abi 24). The BIT stays (bit 3) and so does the `interface_type` value - both marked RETIRED in place, because a bit and a number are never reused. `all_abilities()` is 0x77 now, and its static_assert says which bit is gone; "can this device trace rays" is `device_capabilities::ray_query()`. |
| WHAT IS NOT DONE YET, AND IS NOT PRETENDED | `core::create_acceleration_structure` answers `nullptr` with a ONE-TIME NAMED LOG ("this backend does not serve the tier-1 acceleration-structure interface yet ... plan S1 P1b") - the same honest refusal `create_swapchain` gives - and the two recording verbs answer `unsupported`. The engine still builds its structures through `vulkan/acceleration_structure`, which is why every gate below is unchanged: **P1b is the batch that implements the backend and moves that module over, and it is where the storage/scratch/size-query logic lands.** |
| what the fakes had to say | `probe_backend` (null structure, `unsupported` builds), `test_pass` and `spike_backend_boundary` / `test_dynamic_link` (their foreign command buffers refuse the two recording verbs by name). NONE of them ever implemented the retired ability's verbs - which is the measured proof that the retirement takes nothing away. |
| verification | render gate **14/14**, ctest 19/19, clang-format-check 0, boundary gate 0 symbols, spike 95/0, runtime_dyn 10/0, and the S1 probe (2.22) run again on the new abi: **31 checks, 0 failed, 0 VUIDs**, reporting `as_probe: this executable compiled abi 26` and the same facts as before. |

### 2.24 What the NINETEENTH batch added (the backend SERVES the tier-1 interface; S1's P1b-1)

| area | what landed |
|---|---|
| the implementation | `core::create_acceleration_structure` is REAL now: the caller's contract-layout geometry becomes the driver's structures, `vkGetAccelerationStructureBuildSizesKHR` sizes it, the backend's own factory allocates the storage (`buffer_usage::acceleration_structure_storage`) and the scratch (`..._scratch`, sized ONCE at creation so recording a build never allocates), `vkCreateAccelerationStructureKHR` + `vkGetAccelerationStructureDeviceAddressKHR` produce the handle and the address. A top level also gets its own host-visible instance buffer, written through `write_instances()`. |
| the two recording verbs | `frame_commands::build_acceleration_structure` / `refit_acceleration_structure` record their own memory barrier (HOST_WRITE/TRANSFER/AS_WRITE -> AS_BUILD) and then `vkCmdBuildAccelerationStructuresKHR` in BUILD or UPDATE mode. The refit refuses BY NAME when the structure was not created with `allow_update`, and an empty top level refuses its build rather than recording one that reads zero instances. `frame_commands` is the base of `owned_command_buffer`, so the frame's buffer and a caller-owned one share the implementation. |
| the entry points | all five resolved ONCE at startup when the device has RT (`core.constructor.cppm`), with the host-image-copy rule applied to them: "the extension is enabled" has to mean all five resolve, so a missing name is a NAMED STARTUP PANIC rather than a create-time surprise. |
| the probe now drives it | `tests/test_acceleration_structures.cpp` was rewritten from the engine module's surface to the TIER-1 one, because that is what this batch changes: it creates a refittable BLAS, a non-refittable one and a top level, writes one instance referring to the BLAS's address, records both builds + the allowed refit, and checks the three refusals (capacity, a bottom level's `write_instances`, a refit of a structure that never declared `allow_update`). |
| the measured run | `[test_acceleration_structures] 30 checks, 0 failed -> PASS`, **0 VUIDs with validation on**: `created blas address=0xe970000 size=2944 bytes, tlas address=0xe972500 size=2048 bytes`, `built, refit, submitted and waited: 2 bottom level + 1 top level`. |
| what is still NOT done | the ENGINE still builds its structures through `vulkan/acceleration_structure` (that module owns its own storage/scratch/recording and its three `vkGetDeviceProcAddr` references), so the two implementations coexist for exactly one more batch: P1b-2 moves the module onto this interface and deletes its machinery. |

### 2.25 What the TWENTIETH batch added (the engine's acceleration-structure module moves onto the interface; S1's P1b-2)

| area | what landed |
|---|---|
| the module became a FRONT END | `vulkan/acceleration_structure/**` no longer owns a size query, a storage buffer, the shared scratch with its aligned per-geometry ranges, the instance buffers, the entry points or the recording: `add()` builds ONE `rhi::acceleration_structure_geometry` and calls `create_acceleration_structure()`; `record_build`/`record_update` record ONE verb per structure; `top_level_structure::record_build` writes its records through `write_instances()` and then records the build. What it still owns is the SCENE-SHAPED knowledge: which geometries and instances exist, the refit flag, the growth of a top level's capacity, and the instance table the SHADER reads. |
| what that deleted | `entry_points` (five `PFN_`s and their `vkGetDeviceProcAddr` loads, in both classes), `native_buffer_of`/`buffer_address_of` for the structures, `device_facts`/`facts_of`, `align_up`, `to_instance_transform`, the `micromap_attachment` deque (and with it the "container whose elements never move" rule - there is no pointer into a container any more), the `build_infos`/`range_ptrs`/`update_infos`/`update_range_ptrs` arrays, and the per-slot `instances`/`storage`/`handle`/`structure_size`/`scratch`/`scratch_size`. |
| the objects replace the handles | `handle(index)` became `structure(index)` (a `rhi::acceleration_structure*`), and `structure_size(slot)` now asks `size_bytes()`. The ONE behavioural consequence that mattered upstream: `runtime::write_rt_structure_binding` used to resolve `vkGetAccelerationStructureDeviceAddressKHR` through `vkGetDeviceProcAddr` and fill a `VkAccelerationStructureDeviceAddressInfoKHR` to read the TLAS address - it asks `->device_address()` and `->size_bytes()` now. |
| ONE THING IS REFUSED, LOUDLY, AND IT IS NOT PRETENDED | the contract's geometry has no opacity-micromap attachment, so a source that carries one is refused BY NAME at `add()` ("cannot ride the tier-1 interface yet - plan S1's P4") rather than built without it: a geometry that silently lost its micromap would traverse every micro-triangle as opaque, a wrong picture with no symptom. `rt_mask_bake`/`rt_skin_bake` are RT-only configs (the fourteen frozen scenarios do not enable RT), so this removes a feature from a path that has no instrument today, and P4 restores it as a tier-1 object. |
| the numbers | engine objects referencing a Vulkan symbol **4 -> 2** (`acceleration_structure.cpp`'s object is CLEAN, and `runtime.cpp`'s too - its `vkGetDeviceProcAddr` was the TLAS address query); `acceleration_structure.cpp`'s vocabulary 42 -> 5, `runtime.cpp`'s 23 -> 18, `ray_tracing.cpp`'s 34 -> 33. What is left in the whole engine: the probes' host barrier + raw submit (plan X4) and `ray_tracing.cpp`'s micromap pair (`vkGetDeviceProcAddr` + the HOST_WRITE barrier, plan P4). |
| the instrument | the S1 probe was extended to drive BOTH layers (the tier-1 interface directly, and the module on top of it): **39 checks, 0 failed, 0 VUIDs with validation on**, reporting `the module built and refitted through the tier-1 interface: 2 geometries`. It also checks the micromap refusal. |

### 2.26 What the TWENTY-FIRST batch added (micromaps get the same tier-1 treatment; S1's P4)

| area | what landed |
|---|---|
| the object | `rhi::micromap` (release only), `micromap_desc` (triangle count, attributes + stride, the per-micro-triangle records, the index array, the format), `micromap_usage` + `micromap_triangle`, `micromap_format` (the two values, named the contract's way with a backend static_assert on each), and `interface_type::micromap = 13`; `api_core::create_micromap()` and the recording face's `command_buffer::build_micromap()` are **two APPENDED SLOTS**, which is what moved **abi 26 -> 27**. |
| what the backend owns, and why that is the point | the storage (MICROMAP_STORAGE), the scratch, and THE SETUP BUFFERS THE BUILD READS - including the **256-byte ADDRESS alignment** the API requires of the attributes and the triangle array. That alignment is a requirement on an ADDRESS, so it cannot be a caller's problem: the backend allocates each setup buffer with one alignment of slack, reads its address and writes the payload at the first aligned offset inside it. The engine used to do exactly that by hand. |
| the attachment is ONE HANDLE | `acceleration_structure_geometry::opacity_micromap` (a contract `micromap*`), because the backend that built the micromap also owns the index array the traversal reads and the usage record the build declares - so the four fields the engine used to carry across (an index address, a stride, an index type, a usage record) are gone from the contract. The driver's `VkAccelerationStructureTrianglesOpacityMicromapEXT` is chained into the TRIANGLES data (not into the geometry: validation named that when the engine first attached it in the wrong place), and the attachments live in a `reserve()`d vector on the structure object. |
| what the engine lost | `make_micromap()`'s entry points, size queries, setup buffers, mappings, storage, scratch and alignment; `release_micromaps()`'s `vkDestroyMicromapEXT` loop (an `object_manager` releases itself); the raw `vkCmdBuildMicromapsEXT` batch and the raw `HOST_WRITE -> MICROMAP_BUILD` barrier; `native_buffer_of` and `addressable_flag` (dead once the backend allocated those buffers); and `structure_set::micromap_resource` collapsed from seven members, four addresses and a usage record to ONE handle plus a count. `geometry_source`'s micromap fields collapsed the same way. Because the module's `add()` can now carry the attachment, **the P1b-2 refusal is gone**. |
| the numbers | engine objects referencing a Vulkan symbol **2 -> 1** - `ray_tracing.cpp`'s object is CLEAN, so the only one left in the entire engine is `runtime.probes.cppm` (the probes' host barrier and raw submit, plan X4). `ray_tracing.cpp`'s vocabulary 33 -> 16, `acceleration_structure.cppm` 12 -> 8. |
| the instrument | the S1 probe grew a REAL micromap section (one micro-triangle, UNKNOWN, built BEFORE the geometry that consults it): **42 checks, 0 failed, 0 VUIDs with validation on**, reporting `an opacity micromap was created, built and consulted by a geometry`. |

## 3. What was tried and reverted (do not repeat)

A blanket "replace every native type in the pass layer with the contract type" was attempted and
reverted for four members, because their value comes from the PUBLISHER (the runtime), which supplies
natives, while the contract handles ride the parallel `image_handle` / `view_handle` / `buffer_handle`
fields:

- `resolved_binding::view` / `buffer` / `image` (natives; the handles are the `*_handle` fields),
- `family_entry::views` / `images`, `own_per_image`, `views_of(...)` (natives, published by the runtime),
- `pipeline_storage` / `pipelines` / `pipeline()` and `owned_pipeline::pipeline` (natives; each pass
  returns `VkPipeline`).

Converting these is part of the pass MIGRATION (when the publisher starts holding contract objects),
not a rename. The pass-layer conversions that DID stay are the ones above: the command buffer, the
device-to-`api_core` marker, and the `command_buffer*` fields.

## 4. The migration recipe (what the landed passes do)

`post`, `fxaa` and `upscale` are the worked examples; their shape is what the rest should follow:

```cpp
// a barrier: the PAIR of roles, plus the resource's contract handle
io.list->barrier(rhi::image_barrier{
    .resource = io.barrier_images[0].image_handle,          // or io.targets[0].image_handle
    .from     = rhi::image_use::color_attachment,
    .to       = rhi::image_use::shader_read});
// a batch (one call, several images): what taa uses
io.list->barrier(rhi::barrier_group{.images = std::span(barriers.data(), barrier_count)});

// a rendering scope: the contract's rendering_info, whose attachments are contract VIEWS
if (io.list->begin_rendering(rendering_info) != rhi::error::ok) { return; }
io.list->set_cull_mode(rhi::cull_mode::none);
io.list->draw(3, 1, 0, 0);
io.list->end_rendering();

// a copy (taa, the only user so far): both operands are contract handles
io.list->copy_image(rhi::image_copy{
    .source = io.targets[0].image_handle, .destination = io.own[1].image_handle, /* regions */});
```

The 20 barrier recipes and their role pairs are the reference for picking `.from` / `.to`; the
authority is `scripts/recording_face_census.py`, which prints the measured pairs, the from/to role
histograms and the recipe counts:

```
undefined -> {color_attachment, depth_attachment, shader_read, depth_read, shader_write,
              transfer_destination, present}
color_attachment -> {shader_read, transfer_source, present, color_attachment(dependency)}
shader_read -> {shader_write, transfer_destination, depth_attachment}
shader_write -> {shader_read, transfer_source, shader_read_write}
transfer_source -> {color_attachment, shader_read}
transfer_destination -> {shader_read}
depth_attachment -> shader_read
```

Guard change that comes with every migration: a check that used to be
`io.targets[0].image/view != VK_NULL_HANDLE` becomes `... .image_handle / .view_handle != nullptr`.

`transparent.cpp` is the next file and its sites are known:

| site | today | after |
|---|---|---|
| `:62-68` | two `VkImageMemoryBarrier2` into one `VkDependencyInfo` | `barrier(barrier_group{...})` with pairs `shader_read -> depth_attachment` (`sampling_to_depth_attachment_transition`, on `depth_image`) and `color_attachment -> color_attachment` (`color_attachment_dependency`, on `target_image`) |
| `:73-91` | secondary inheritance + `vkBeginCommandBuffer` on a native secondary | the secondary's own `begin_recording(...)` once that handle is a contract buffer |
| `:93-100` | record the leaves into the native secondary | unchanged recording, into the contract secondary |
| `:108-112` | `VkRenderingAttachmentInfo` + `vkCmdBeginRendering` | `begin_rendering(rendering_info{...})` with contract views, `secondary_contents = true` |
| `:114` | `vkCmdExecuteCommands(io.cmd, 1, &secondary)` | `io.list->execute(*secondary)` |
| `:116` | `vkCmdEndRendering(io.cmd)` | `end_rendering()` |
| `:123-126` | `shadow_map_sampling_transition` barrier | `barrier(image_barrier{ depth_attachment -> shader_read, io.targets[1].image_handle })` |

## 5. What that failure class WAS, and the four bugs it hid

The compiling half of the batch was mechanical: `resolved_io::cmd` became the contract handle, so every
raw call site that recorded on it had to move to the contract series. Two independent facts about the
build itself:

1. The intermittent `unable to open output file '...pcm'` / "the file is opened with a user-mapped
   section" failure is NOT a code error: it hit 2-3 times in a row on this machine and clears on a retry.
   Retry the build on that signature ONLY; treat any other error as real.
2. `ninja` itself cannot run under a restricted file sandbox on Windows (it spawns the child, the child
   finishes, and ninja waits forever on a handle it never gets - measured with a two-line ninja file).
   The build needs the unrestricted mode; that is an environment fact, not a project one.

FOUR BUGS THE COMPILER COULD NOT SEE, each found by a gate rather than by reading, and each now fixed:

1. **`owned_command_buffer` shadowed the base's `owner`.** The type derives from `frame_commands` since
   this refactor (it used to hold a `frame_commands list` member), and it declared `core* owner` of its
   own. `create_command_buffer`'s `answer->owner = this` therefore set the DERIVED member while every
   INHERITED verb (`begin_gpu_timing`, `mark_gpu_timing`, the frame-scoped `use`, `barrier`'s provenance)
   read the base's null one. The spike caught it as a null dereference in `begin_gpu_timing`
   (`cmpb $0x1, 0xf68(%rdx)` with `rdx = 0`); the fix is to delete the derived member. This is why "one
   implementation, reached by inheritance" needs ONE copy of every base field.
2. **`.header = {}` defeated a default member initializer, and the chain was refused by name.** The
   shadow cascade's begin info wrote `.header = {}` explicitly, which VALUE-INITIALISES
   `structure_header` and takes its own defaults (`s_type = unknown`) instead of
   `vulkan_command_buffer_inheritance_info`'s NSDI. `validate_structure` then refused the chain, so every
   shadow secondary failed to begin - "shadow secondary command buffer begin failed - cascade N skipped".
   Omitting the member is what keeps the NSDI; `= {}` on the WHOLE aggregate is fine (the spike does it).
3. **Four publications filled only the RAW lanes of `resolved_binding`.** `scene_color`,
   `rt_shadow_visibility`, `shadow_map` and `furnace_cube` were published with `.view`/`.image` and no
   `.image_handle`/`.view_handle`. The scene pass's rendering scope takes the CONTRACT view handles, so
   the backend refused the whole scope (a null view is `invalid_argument`) and the scene recorded NOTHING
   - while `run_report` still counted `recorded = 1`, because the runner counts "record() was called",
   not "commands were recorded". THE RENDER GATE FOUND THIS: 13 of 14 scenarios came back as the same
   uniform colour with zero VUIDs and no log line, which is the signature of "a valid scope that was
   never opened". `scene_color` is the alias whose target this frame's TAA switch chooses, so its
   publication repeats that branch to hand out the matching handles.
4. **`extent_rule::none` answered the type's default instead of `{0, 0}`.** `rhi::image_extent{}` is
   `{width 0, height 1, depth 1}` (the contract's "a 2D image has at least one row"), while the field's
   own note promises `{0,0}` for "this pass sizes its own work". `test_pass` caught it; the rule now sets
   width and height explicitly.

Two more gaps were found and are NOT bugs - they are missing vocabulary, reported rather than papered
over (see section 8): the RASTER/RAY-TRACING stage override the ray-traced shadow's barriers need, and a
GLOBAL memory barrier (no operand), which `compute_skin`'s build-ordering barrier is. Both keep their raw
spelling through the contract's own escape and are the measurements a future field must cite.

## 6. How to verify a batch (the established gate set, all green on the phase-A batch)

```
cmake --build build-release-dyn-clang64                      # 0 (retry ONLY the pcm signature)
ctest --test-dir build-release-dyn-clang64                   # 19/19
cmake --build build-release-dyn-clang64 --target clang-format-check   # 0
python scripts/check_backend_boundary.py --config dynamic --require-zero   # exit 0: 0 boundary symbols, 0 imports
build-spike-clang64/test_backend_boundary_spike.exe --with-device         # 95 checks, 0 failed, self-exits
build-release-dyn-clang64/test_runtime_dyn.exe --with-device              # 10 checks, 0 failed, self-exits
pwsh -File scripts/windows/check_render.ps1 -Full -BuildDir build-release-dyn-clang64 -Compare frozen
                                                             # exit 0, matched 14 / mismatched 0, zero VUIDs
python scripts/recording_face_census.py                      # the two sweep numbers + the recipe counts
```

The render gate's verdict is its EXIT CODE and the 14 matched hashes, never the `changed` count (the
local reference set may be stale on purpose). The census convention is: a line counts when it is not
a comment and contains `vkCmd<Name>(`; it prints the per-verb table plus the files behind both numbers,
so the two sweep numbers are comparable across batches.

Expected abi: **21** after the `make_command_buffer` virtual lands (20 today). A `std::shared_ptr` in the
interface is why `rhi.api_core.cppm` needs `<memory>` in its global module fragment.

## 7. Traps that have already cost time here

**TRAP (the fifteenth batch): a new ability's view needs its `owner` assigned in the constructor, and the
failure is a WRONG ANSWER rather than a crash.** `core::frame_device_capabilities` reads the core's cached
facts, so with `capabilities_view.owner == nullptr` every method answered its default - `mesh_shader()`
returned false - and the render gate reported it in the terms of what it broke: "pipeline 'pbr' has no mesh
stage to build from, and its vertex form is gone" (the vertex form is deleted, so the app cannot start).
Every other view in that constructor (`escape_view`, `heap_view`, `address_view`, `host_copy_view`) sets it
one line each; a new view must join them, and the instrument that catches a miss is the render gate, not the
build.

1. **Do not edit these files with shell string manipulation.** Three separate accidents came from
   PowerShell string handling in one session: a comment split by an escape sequence (a backtick-
   quoted `rhi::api_core` inside a double-quoted string became a carriage return), 15 pass `.cppm`
   files given a mangled block (PowerShell hashtable text) instead of the intended line, and anchors
   that silently matched nothing. Use the edit tool for edits; when a script is unavoidable, build
   the anchors from the file's own line endings and verify every anchor's expected count BEFORE
   writing anything.
2. **Line endings.** The repository stores LF; the working tree can be CRLF. A script that rewrites a
   whole file turns a 30-line change into a 1350-line diff. Normalise the touched files to LF and
   check `git diff --numstat` against `git diff --ignore-all-space --numstat`.
3. **A blanket type replacement is not a rename.** See section 3: check who SUPPLIES the value. The
   publisher decides whether a native or a contract handle is the right type; converting the
   publisher is the migration.
4. **The parenthesised `};`** - when moving a class body, do not carry its closing brace with it (this
   produced 20 errors that all read "unknown type name").
5. **`api_core` growth means every implementation must answer.** Adding a pure virtual
   (`make_command_buffer`) makes `impl final : api_core` abstract in `tests/probe_backend.cpp`; the
   probe needs its override with a no-op deleter, because its buffer is a static member.
6. **`command_list` is gone.** If a search finds it, the two hits in section 2.5 are the only ones
   that should exist.
7. **A derived member that shadows a base one is a silent split-brain.** Bug 1 of section 5: the
   constructor set one field and the inherited verbs read the other. When a type starts DERIVING from
   something it used to hold, delete the fields it duplicated - nothing warns you, and the symptom is a
   null dereference in a verb nobody called during the build.
8. **`<member> = {}` is NOT "use the default member initializer".** It value-initialises the member and
   takes THAT type's defaults (bug 2). For a tagged structure whose NSDI names the `s_type`, either omit
   the member or spell the whole value; `T X = {}` on the aggregate is the safe form, and a refusal is
   named by `validate_structure` rather than silently dropped.
   THE SECOND MEASUREMENT OF THE SAME TRAP (third batch): a structure chained into a query's `pNext` had
   `sType` left at ZERO by its `= {}`, and **`VK_STRUCTURE_TYPE_APPLICATION_INFO` IS ZERO** - so the
   driver read an APPLICATION_INFO where an acceleration-structure property block was meant, and
   validation said exactly that (`VUID-VkPhysicalDeviceProperties2-pNext-pNext`, twice). The FIX PATTERN
   is the project's own convention, not a repaired line: the chain head and its members are built by
   `constant_init` factories (`make_properties_2` / `make_features_2` / `make_acceleration_structure_properties`
   / `make_ray_tracing_pipeline_properties` / `make_ray_query_features` / `make_mesh_shader_features`), so
   NO CALL SITE NAMES AN `sType` and none can forget one. When a tagged struct is built by hand anywhere,
   that is the bug this section is about - measured twice now, on two different chains.
9. **A gate that counts "record() was called" is not a gate that the frame was recorded.** The
   renderer's `run_report` said `recorded = 1` while the scene's scope was refused whole (bug 3). The
   only instrument that caught it was the 14-hash render gate; when a frame comes back UNIFORM with
   zero VUIDs, suspect an early `return` on a refused contract call, not a shading bug.
10. **Every new publication must fill BOTH lane sets.** `resolved_binding` carries the raw handles and
    the contract ones, and a publisher that fills only the raw set compiles and then refuses the scope
    that needed the other (bug 3). Audit with a sweep over `pass::resolved_binding{` / `single(`, not by
    reading the one site you happen to be editing. (The pass framework then went the OTHER way in the
    fourth batch: one lane, the contract's - a lane that no caller needs is a lane that can go stale.
    The audit is what says which of the two a lane is.)
11. **A contract verb's argument order is NOT the native's, and a verb with no callers is untested
    code.** `frame_commands::set_depth_bias` forwarded `(clamp, slope, constant)` to a Vulkan function
    that takes `(constant, clamp, slope)` - wrong in all three - with a comment asserting it was
    Vulkan's own order. Nothing could see it: the only site that wanted a depth bias called
    `vkCmdSetDepthBias` directly. Moving that site onto the verb turned the render gate from 14/14 into
    matched 2 / mismatched 12. RULE: when a migration routes a site through a verb for the first time,
    read the verb's IMPLEMENTATION against the native signature it ends in - the compiler checks the
    types, and every one of these three is a `float`; and write a second caller's worth of suspicion
    into the test you reach for (here: the 14-hash gate, the only instrument that reads the number
    back).
12. **A factory's pipeline carries the FACTORY's state, not the static state you used to bake in.**
    `make_graphics_pipeline` goes through `create_pipeline`, whose graphics recipe declares viewport,
    scissor and cull mode DYNAMIC for every recipe - the render_environment states them per draw. The
    heap probe's raw builder had declared all three STATIC, so its draw needed none of them; the day the
    probe moved onto the factory, its draw became three VUIDs per frame (`Dynamic viewport(s) (0x1) are
    used by pipeline state object, but were not provided via calls to vkCmdSetViewport()`, the same for
    scissor, plus `VK_DYNAMIC_STATE_CULL_MODE state is dynamic, but the command buffer never called
    vkCmdSetCullMode`) - 66 in a 40-frame run, and the render gate came back **0 matched / 14
    mismatched**. RULE: a migration onto a factory must port the STATE LIST with the descriptors, and
    the state list is exactly what a static pipeline hides; grep the recipe for the dynamic states it
    declares, and for every draw site the new pipeline serves, make sure the three (or n) are set. It is
    the same trap as 11 one level up: the factory's contract with its callers is state, not just
    formats.
13. **A basic handle travels as a TAGGED, INTERFACE-FREE token - not as `void*`, not as the native type.**
    The ruling (sixth batch): an empty `api_basis` whose only content is `s_type`
    (`structure_type::vulkan_device_basis`), COMPOSED by the backend and handed out by `core::get_basis()` -
    not a base of `api_core`, and not a base of the backend's own type, so a signature that takes the contract
    does not also take "the thing handles hang off". Two consequences worth keeping: (a) the token carries NO
    handle, so nothing in it can go stale and an implementer owes the type nothing (no virtual, no ownership,
    no interface); (b) the check is the TAG, because `-fno-rtti` means no `dynamic_cast` - and the tag is the
    stronger question anyway, since a receiver wants to know "does this stand for a Vulkan device", not "what
    is its most-derived type". `void*` would have accepted the same call and checked nothing.

## 8. What is left (ranked, with the measurements each step needs)

**THE PASS LAYER IS AT ZERO `vkCmd*` SITES, HAS NO DEVICE IN ITS CONTEXT, AND EVERY PIPELINE IT BINDS IS A
CONTRACT OBJECT.** The census reads **5 sites in 3 files**, none of them a pass, and every one of them is
a site this note NAMES as the escape bucket rather than an unmigrated call:
`runtime/runtime.probes.cppm` (1 - the host-visible barrier the contract's own `image_use` census assigns
to the escape bucket; the batch before that had 11 sites here), `vulkan/ray_tracing/ray_tracing.cpp`
(3 - the HOST_WRITE micromap barrier, the micromap write -> read barrier whose roles `buffer_use` does not
spell, and a build entry-point resolution), and `readback.cpp` (2 - its conservative buffer barrier and
its copy; the class is a raw-handle utility by design - see 2.15). The include count is **38** (the
census's own scope: `runtime/**` + `vulkan/**` MINUS `vulkan/core/**`).

DONE IN THE SECOND, THIRD AND FOURTH BATCHES (all gated, see section 6):

1. **The pipeline migration, both halves.** Compute: `pipeline_desc::compute_code` +
   `create_compute_pipeline` (heap flag, NULL layout, `bind_point = COMPUTE`), the six
   `vkCreateComputePipelines` builders turned into contract factories. Ray tracing:
   `ray_tracing_stages`/`ray_tracing_groups`/`max_ray_recursion` + `create_ray_tracing_pipeline` (one
   module per stage, the group type derived from the slots, the entry point resolved through
   `vkGetDeviceProcAddr`, `bind_point = RAY_TRACING_KHR`). The runner's raw bind FALLBACK IS GONE, and
   `pass::native_commands()` survives with exactly ONE user class: the resolved launch entry points
   (`vkCmdTraceRaysKHR`), which no contract verb spells.
2. **The two vocabulary gaps, each with its measurement.** `barrier_group::stage` (`stage_hint`) is how
   the ray-traced shadow's two barriers name the stage that actually RAN; `barrier_group::has_memory` +
   `memory_barrier` (and the appended `buffer_use::acceleration_structure_read`) is how the skinning
   job's global barrier is spelled. Both are `struct_size`-guarded appends - which is why `image_barrier`
   itself, a pointer-first POD with no guard, was NOT the place for a field: a caller compiled against
   the old layout would have had its pointer's low half read as a size.
3. **abi 21 is pinned**: the contract's constant and both test pins (`test_dynamic_link.cpp`,
   `test_runtime_dyn.cpp`) moved together, and the constant's own note records what moved (the appended
   `make_command_buffer` vtable slot) and what did NOT (every POD of these batches).
4. **The lane audit, closed both ways.** `frame_pass::pipeline_handle()` is the ONE pipeline accessor
   (the raw `pipeline()` is deleted), `resolve_declaration` publishes one run of contract handles,
   `declaration_pipelines_ok` asks `named_pipeline` first (which is what makes that accessor real rather
   than dead), and the four publications that filled only a raw lane are gone. The measurements: the
   runner's fallback removal produced `vkCmdDraw ... a valid GRAPHICS pipeline must be bound` while a
   second lane existed (third batch), and the raw lane's deletion let 18 files drop their Vulkan include
   (fourth batch).
5. **The shadow session records through the contract** (bind, viewport, scissor, depth write, depth
   bias), which is what took the frame loop from 34 call sites to 29 - and which uncovered trap 11.
6. **The device lane is gone, and the raw RAII lane with it** (fifth batch): `pass_context::device`,
   `frame_services::device` and `VkDevice` on every builder are deleted; `build_heap_probe_graphics`
   (the last raw builder) is a contract factory; `make_shader_module_raw`, `shader_module_handle`,
   `release_contract_shader`, `pipeline_handle::raw_device` / its raw constructor / `destroy_raw` are
   deleted. One pass still needs a device, and it reaches it the escape's way (`pass::native_device`).
7. **The basic handle travels as a tagged token** (sixth batch): `api_basis` (`s_type` only, no
   interface), `vulkan_escape` +3 slots (abi 22), and `ray_traced_shadow.cpp` names no
   `VkDevice`/`VK_SUCCESS`/`vkGetDeviceProcAddr` any more.
8. **The escape's demand is measured and shrinking** (seventh batch): 105 -> 102 engine-side call sites,
   the heap/recording class 68 -> 65, one dead native cache and two dead accessors deleted, and the
   contract ALREADY carried the shapes for all of it (`heap_image_write_info` + the heap verbs) - what
   blocks the rest is the frame resource table's raw lanes and the probes' raw command buffers, not the
   contract (see 2.12's "what blocked the rest").
9. **The probes record through the contract** (eighth batch): `vkCmd*` 51 -> 41, the probes' pools,
   fences, queue submissions, dynamic state, rendering scope and mesh dispatch all went through the
   record series or the abilities, `get_pipeline()` is deleted, the shadow cascade's push is the
   ability's verb - and the backend bug that exposed (`heap_commands` refusing every buffer but the
   frame's own, silently dropping heap binds/pushes for owned buffers) is fixed (see 2.13).

WHAT REMAINS:

1. **The frame-level sweep** (`runtime.frames.cppm` LAST, the other five files first): the frame loop's
   own raw steps still want the record series - the runner's viewport/scissor, the `clear_hdr` clear,
   the furnace/`clear_hdr` barriers, the shadow hand-back, `readback.cpp`'s one-shot copy,
   `ray_tracing.cpp`'s structure barriers, `pipelines.cppm`'s `begin_pipeline`, and the push (the
   descriptor-heap push has no contract verb yet, so `contract_push_heap_data` is the escape-bucket
   remainder). The bind is DONE, so what is left there is state and barriers, not pipelines.
2. **THE RAY-TRACED SHADOW'S REMAINING NATIVE FACTS ARE THE LAUNCH, AND THAT IS A DECISION RATHER THAN A
   LEAK.** `ray_traced_shadow.cpp` still names `VkBuffer`, `VkCommandBuffer`, `VkDeviceAddress`,
   `VkStridedDeviceAddressRegionKHR` and the SBT usage bit - the REGIONS a `vkCmdTraceRaysKHR` launch is
   given and the table they live in. The DEVICE half is gone (sixth batch): `api_basis` carries it,
   `pass::device_proc` resolves the entry point, `pass::shader_group_handles` reads the group handles, and the
   file names no `VkDevice`/`VK_SUCCESS`/`vkGetDeviceProcAddr` at all. Putting the REGIONS behind the contract
   is a VOCABULARY step (`command_buffer::trace_rays(regions)`) rather than a migration, with its own design
   questions; until it is taken deliberately, this file's remaining natives are the escape bucket, exactly
   like `pass::native_commands` / `pass::native_device` / `pass::device_basis`, and they should NOT be listed
   as "left to clean up".
3. **The include sweep, continued - and it is a SECOND MIGRATION, not a delete pass.** Of the 40 engine
   files (the census's scope) that still include a Vulkan header, the blockers are:

   | blocking token(s) | files | what it would take |
   |---|---|---|
   | `VkFormat` only (the pass files that survived the fourth batch: character_forward, fxaa, post, upscale and the scene-ish set) | ~6 | an `rhi::image_format` lane for the pass-level format facts (the job passes and the inheritance already have one) |
   | `VkImage` / `VkImageView` / `VK_NULL_HANDLE` in the pass `.cpp` files (deferred, fxaa, geometry_buffer_debug, goo_rim, post, toon_screen_rim, upscale) | ~7 | the raw lanes of `resolved_binding` retired: those files already ask `image_handle`/`view_handle` first and keep the raw test as a refusal |
   | `VkDeviceAddress` / `VkPipeline` in the JOB passes (mask_bake, compute_skin) | 4 | the job passes' own AS/SBT lane (the same vocabulary step item 2 names), or their own escape use |
   | `VkCommandBuffer` / `VkBuffer` / `VkStridedDeviceAddressRegionKHR` / `VK_SUCCESS` (ray_traced_shadow.cpp, the framework's `pass.cppm`, `native_commands`) | ~5 | item 2: PERMANENT by decision (`pass::native_commands` / `native_device` describe it, so `pass.cppm` keeps them) |
   | the backend-facing modules (`render_layout`, `bindings`, `init_utils`, `render_resource/shared`, the scene/transparent inheritance) | ~12 | they DESCRIBE the device layout; several are legitimately in the escape bucket |

   The honest order is: item 1 (the frame sweep) -> the pass files' raw-lane retirement -> then delete
   includes file by file, `runtime.frames.cppm` last, re-running section 6's gates after each batch. The
   fourth and fifth batches' method is the one to copy: run a code-only token scan (comments stripped)
   over the candidate, and delete the include only when the file names NO Vulkan type, NO `VK_` macro and
   NO `vk*` entry point.
4. **One VUID-free smoke run per ungated path, every time.** The ray-traced path is not in the fourteen
   scenarios, and the third batch's real defects were found by a hand smoke run with
   `[render] rt_shadows = true`. Any path the capture gate cannot reach (ray tracing, bloom, the
   megalights demo) deserves the same 40-frame run with validation on before a batch is called green -
   the fifth batch's probe VUIDs showed up in BOTH instruments at once (0/14 on the gate, 66 VUIDs on the
   smoke run), which is the reassuring case: the instruments agree.


