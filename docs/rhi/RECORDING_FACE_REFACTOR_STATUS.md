# Recording-face refactor: goals, progress, blocking state, and next steps

> Working note (2026-10-07). **STATUS: the phase-A batch has LANDED AND IS GREEN.** The build compiles,
> and every gate in section 6 passes on this machine: build 0, `ctest` 19/19, `clang-format-check` 0,
> the boundary gate 0 symbols, the spike 95 checks / 0 failed, `test_runtime_dyn` 10 / 0, and the render
> gate **matched 14 / mismatched 0 (exit 0)**. The census moved from 116 to **60** engine-side `vkCmd*`
> call sites; the 60 that remain are enumerated in section 8, not hiding in the pass layer.
>
> THIS NOTE IS THE COMMIT-MESSAGE-LENGTH VERSION of that batch: section 2 is what changed, section 5 is
> the failure class it closed (including four bugs the compiler could not have found), section 8 is what
> is deliberately left. Sections 1, 3, 4 and 7 are the parts that stay true for the next batch.
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
| `mask_bake.cppm/.cpp`, `compute_skin.cppm/.cpp` | their record entries take `rhi::command_buffer&` (their push callbacks too), dispatch through the contract and keep two raw steps through `pass::native_commands` (the compute bind and, for the skin job, a global memory barrier). The runtime's structure hooks reach the frame's contract buffer by IDENTITY (`frame_command_buffer()` checked against the native they were handed) instead of widening the ray-tracing module. |
| `runtime/*` | `frame_command_buffer()` (non-owning shared_ptr over the core's borrowed view) + `native_frame_commands()`; `frame_services::cmd`, the three `ensure_*_sampled`, `make_*_environment`, the push/mesh endpoints and `record_shadow_cascade` all speak the contract; the secondaries (`secondary_command_buffers`, `main_segments`, `shadow_recording`) are `std::shared_ptr` from `make_command_buffer()`; `record_shadow_content` takes the session's shared_ptr. |
| `render_environment.cppm`, `primitive.cpp`, `readback.cpp` | the four mesh/push endpoints take `rhi::command_buffer&` (the runtime converts once, inside); the primitives pass `*env.command_buffer`; the read-back's one-shot buffer IS the recorder. |
| `vulkan/core/*` | `frame_commands` lost its `final` (owned buffers derive from it); `frame_commands::execute` RECORDS (`vkCmdExecuteCommands` on the frame's own buffer, provenance-checked) instead of refusing; `begin_rendering`'s attachment array is 8, not 4 (the scene carries five colours + depth); `owned_command_buffer::begin_recording` derives the descriptor-heap inheritance from the core's own heap state. |
| `tests/*` | the merged `foreign_command_buffer` / `probe_command_buffer` (lifecycle + series, `make_command_buffer` with a no-op deleter), `test_pass`'s contract stand-in for `cmd`, and the identity/provenance assertions the deleted borrowed view used to carry. |

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
9. **A gate that counts "record() was called" is not a gate that the frame was recorded.** The
   renderer's `run_report` said `recorded = 1` while the scene's scope was refused whole (bug 3). The
   only instrument that caught it was the 14-hash render gate; when a frame comes back UNIFORM with
   zero VUIDs, suspect an early `return` on a refused contract call, not a shading bug.
10. **Every new publication must fill BOTH lane sets.** `resolved_binding` carries the raw handles and
    the contract ones, and a publisher that fills only the raw set compiles and then refuses the scope
    that needed the other (bug 3). Audit with a sweep over `pass::resolved_binding{` / `single(`, not by
    reading the one site you happen to be editing.

## 8. What is left (ranked, with the measurements each step needs)

The pass layer is done: **60 of the original 116 `vkCmd*` sites remain, and none of them is a pass
recording its own frame content.** They are, by file: `runtime/runtime.frames.cppm` (34 - the frame
loop's own open/close, its barriers, the furnace clear, the pass-runner viewport/scissor/bind, and the
shadow hand-back), `runtime/runtime.probes.cppm` (8 - the probe's own path), `ray_tracing.cpp` (4),
`runtime/runtime.cpp` (3), `pipelines.cppm` (3 - `begin_pipeline`), `compute_skin.cpp` (2 - pipeline bind
+ global memory barrier), `ray_traced_shadow.cpp` (2 - the RAY-TRACING stage override), `readback.cpp`
(2), `cluster.cpp` (1) and `mask_bake.cpp` (1) - the last two are the compute PIPELINE BINDs.

1. **The pipeline migration is the gate on the rest** (it is what the remaining `vkCmdBindPipeline`
   sites wait for). `api_core::create_pipeline` builds GRAPHICS pipelines only: `pipeline_desc` has no
   compute spelling and `owned_pipeline::bind_point` is therefore always GRAPHICS. Add the compute form
   to the descriptor and the branch to `create_pipeline`, turn the `vkCreateComputePipelines` builders
   in `vulkan/pipelines/pipelines.cppm` into contract pipelines (the graphics ones already are), and
   carry `rhi::pipeline*` beside `VkPipeline` through `pass::owned_pipeline` / `resolved_io::pipelines`.
   That deletes `pass::native_commands()` (section 2's helper) and its call sites in `cluster.cpp`,
   `mask_bake.cpp`, `compute_skin.cpp` and the pass runner.
2. **Two pieces of VOCABULARY are owed, each with its measurement on the record** (section 5): a stage
   override on `rhi::image_barrier` (the ray-traced shadow's barriers replace the recipe's writing /
   reading stage with the RAY-TRACING one) and a global memory barrier (`compute_skin`'s
   build-ordering barrier has no operand, and `barrier_group` carries images and buffers only). Both are
   raw TODAY through the escape, on purpose. `image_barrier` is a by-value POD with no `struct_size`
   guard, so a field cannot be appended without the abi-visible layout change that guard exists to
   avoid - the design step is a role/stage vocabulary, not a mask parameter.
3. **Then `pass_context::device` and the escape's other uses** (`native_device_of(...)` in
   `runtime.frames.cppm` / `runtime.cpp`, the `built_against` markers): the same step drops the
   `VkDevice` parameter from the pipeline builders.
4. **abi 21**: `make_command_buffer()` is already the way the engine creates its secondaries
   (`runtime.constructor.cppm`), and the borrowed frame buffer is handed out as a non-owning
   `std::shared_ptr` (`runtime::frame_command_buffer()`). What is left is the pin:
   `promise/rhi/rhi.contract.cppm`'s `abi_version` is still 20, and `tests/test_dynamic_link.cpp` +
   `tests/test_runtime_dyn.cpp` both spell 20u.
5. **The include sweep.** 62 engine files still `#include <vulkan/...>`; the target is the escape
   bucket (<= 7). Delete each file's include as it stops naming a native type, `runtime.frames.cppm`
   LAST - and re-run section 6's gates after each batch, because the render gate is the only thing that
   catches what a lost contract lane does to a frame (bug 3).

