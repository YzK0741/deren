# Recording face plan: `command_buffer`'s record series, the `barrier` and `rendering_info` descriptors, and the `pipeline` extraction

> Audience: the agent that implements this. Everything needed is in this file; the measured
> numbers come from `runtime/**` and `vulkan/**` (the engine's own sources) at
> `master` = `5f3aa99`, abi 19, boundary gate 0 symbols / import meter 0, backend a DLL loaded
> by name. Read `DYNAMIC_LINK_PROGRESS.md` first for the surrounding state.

## 0. Why this exists

The engine is contract-only **at the boundary** (no backend symbol crosses, no engine source
imports a `deren_vulkan` module) but **not Vulkan-free in its sources**: 62 of its 80 files
`#include <vulkan/...>`, and the passes record raw Vulkan themselves. Measured census:

```
MEASURED CALL SITES (the sweep's convention, stated so the number is comparable - it is call
sites, NOT tokens: a line counts when it is not a comment and contains kCmd<Name>():

  18 vkCmdBeginRendering
  17 vkCmdEndRendering
  11 vkCmdSetCullMode
  10 vkCmdDraw
   8 vkCmdBindPipeline
   6 vkCmdDispatch
   4 vkCmdExecuteCommands
   3 vkCmdSetViewport
   3 vkCmdSetScissor
   2 vkCmdCopyImage
   2 vkCmdSetDepthWriteEnable
   1 vkCmdSetDepthBias
   1 vkCmdCopyBuffer
   1 vkCmdClearColorImage
   1 vkCmdDrawMeshTasksIndirectEXT

and the pipeline side: 12 vkCreateComputePipelines, 2 vkCreateGraphicsPipelines,
4 vkCreateShaderModule, 10 vkCmdBindPipeline. Engine files here means runtime/** plus vulkan/**
excluding vulkan/core/** (the backend), extensions .cppm/.cpp/.hpp: 80 files, of which 62 include a
Vulkan header.pipeline side: 12 vkCreateComputePipelines, 2 vkCreateGraphicsPipelines, 4 vkCreateShaderModule,
               10 vkCmdBindPipeline; vulkan/pipelines/pipelines.cppm is 76 KB with vk=294.

escape use is concentrated: only 7 engine files use native_* at all
(22 native_image_view, 8 native_command_buffer, 2 native_command_buffer_of, 1 native_pipeline).
```

**Do not copy the Vulkan API.** The list below is what this renderer actually calls; anything it
does not call is not part of the face. Two verbs are deliberately NOT generalised, and that is the
measurement's verdict, not a shortcut:

- **`vkCmdPushConstants` is never called** - the 5 tokens are comments explaining why. Every
  heap-native pipeline is created with `layout = VK_NULL_HANDLE` (validation requires it), so a
  stage block travels as DATA through `vkCmdPushDataEXT`. There is no push-constant path to model.
- **The 2 host-visible barrier sites stay in the escape bucket** (`runtime.probes.cppm:271`,
  `ray_tracing.cpp:631`): they are a probe's own copy and an AS build input, i.e. exactly the
  "native fact" the escape exists for. The contract therefore needs no host-access vocabulary.

## 1. Scope of the first batches

Order matters: `barrier` and `rendering_info` are the two descriptors most likely to leak Vulkan
vocabulary into the contract, so they are specified here field by field and implemented first,
with one pass migrated as a pilot before the rest.

1. contract: record series + `barrier` + `rendering_info` (+ the small state enums) - **abi 19 -> 20**
2. backend: implement the translation (the only place that reads Vulkan structures) + table tests
3. pilot: migrate ONE pass (`post` is a good pilot: 4 barriers, 1 begin/end) - 14 hashes + VUID 0
4. the rest, one pass per commit, `runtime.frames.cppm` (18 barriers) last
5. `pipeline`: `pipelines.cppm` stops naming `VkPipeline`/`Vk*`; the engine's pipeline builders take
   contract descriptors
6. sweep: delete each file's `#include <vulkan/...>` as it stops needing one

## 2. The record series (verbs; parameter types are contract PODs or contract handles)

| group | verbs | replaces | count |
|---|---|---|---|
| render scope | `begin_rendering(rendering_info const&)`, `end_rendering()` | `vkCmdBeginRendering`/`EndRendering` | 29 / 22 |
| binding | `bind_pipeline(pipeline const&)`, `bind_vertex_buffer(buffer const&, uint64 offset)`, `bind_index_buffer(buffer const&, uint64 offset, index_type)` | `vkCmdBindPipeline` etc. | 10 / 1 / 0 |
| draw | `draw(vertex_count, instance_count, first_vertex, first_instance)`, `draw_indexed(index_count, instance_count, first_index, vertex_offset, first_instance)` | `vkCmdDraw`/`DrawIndexed` | 10 / 5 |
| compute + geometry | `dispatch(x, y, z)`, `draw_mesh_tasks(x, y, z)`, `draw_mesh_tasks_indirect(buffer const&, offset, count, stride)` | `vkCmdDispatch`, `vkCmdDrawMeshTasks*` | 6 / 16 / 3 |
| dynamic state | `set_viewport(viewport const&)`, `set_scissor(rect const&)`, `set_cull_mode(cull_mode)`, `set_depth_write(bool)`, `set_depth_bias(bias, slope, clamp)` | `vkCmdSet*` | 3 / 3 / 11 / 4 / 2 |
| data | `push_data(heap_address, std::span<std::byte const>)` | `vkCmdPushDataEXT` | 13 |
| synchronisation | `barrier(barrier_group const&)`, `barrier(image_barrier const&)` | `vkCmdPipelineBarrier2` | 53 |
| copy + clear | `copy_image(image_copy const&)`, `copy_image_to_buffer(...)` (EXISTS), `copy_buffer(...)`, `clear_color_image(...)` | `vkCmdCopy*`, `vkCmdClearColorImage` | 4 / 2 / 1 / 1 |
| secondary | `execute(command_buffer&)` (EXISTS, abi 16) | `vkCmdExecuteCommands` | 4 |
| query | `write_timestamp(query const&, stage)` (`create_query` EXISTS) | `vkCmdWriteTimestamp` | 1 |

**Not in this series** (they belong to faces that already exist):

- `vkCmdBuildAccelerationStructuresKHR` (4), `vkCmdTraceRaysKHR` (3), `vkCmdBuildMicromapsEXT` (3)
  -> the `ray_tracing` face (abi 5+), which takes the recorder as its target.
- the descriptor-heap description/properties -> the `descriptor_heap` face (abi 12). `push_data` is
  the one heap verb that belongs on the recorder, because it is a command-buffer operation.

**`push_data` shape.** The engine computes an absolute heap slot today
(`heap_slot_offset(heap_slots::x)`), and the framework already holds an owner+callback rather than
the heap itself (`pass::resolved_io::push_endpoint`, `pass_host::push_block =
{.owner = this, .push = &runtime::push_stage_block}`, `runtime.frames.cppm:2761`). Keep that
ownership relation: the pass talks to the HOST, the framework's endpoint implementation calls the
contract verb, and the address it passes must come from the contract's heap face - the engine must
not fabricate device addresses. The backend wraps it into `VkPushDataInfoEXT` + `vkCmdPushDataEXT`.

**Interface placement.** Define the series ONCE on the recording interface. `command_buffer` (the
owning handle, abi 16: `begin_recording`/`end_recording`/`recording`/`execute`/`release`) and
`command_list` (the borrowed frame view the passes hold) both expose it; do not declare the verbs
twice. `command_buffer::recording()` keeps returning the borrowed view.

## 3. `barrier` (53 sites; replace `vkCmdPipelineBarrier2`)

Only "use -> use" is exposed. The backend derives stage/access masks; that is sound because the
engine's only two explicit host-visible masks are the two escape-bucket sites above.

```cpp
struct subresource_range {                 // all-zero means "the whole image"
    std::uint32_t base_mip = 0, mip_count = 0;
    std::uint32_t base_layer = 0, layer_count = 0;
};

struct image_barrier {
    rhi::image*       resource = nullptr;  // a contract handle
    image_use         from     = image_use::undefined;   // EXISTS in the contract
    image_use         to       = image_use::undefined;
    subresource_range range    = {};
};

enum class buffer_use : std::uint32_t {    // APPENDED; small on purpose (4 sites)
    undefined = 0, shader_read, shader_write, transfer_source, transfer_destination
};

struct buffer_barrier {
    rhi::buffer*  resource = nullptr;
    buffer_use    from     = buffer_use::undefined;
    buffer_use    to       = buffer_use::undefined;
    std::uint64_t offset   = 0, size = 0;  // size == 0 means "the whole buffer"
};

struct barrier_group {
    std::uint32_t                   struct_size = sizeof(barrier_group);  // see section 6
    std::span<image_barrier const>  images  = {};
    std::span<buffer_barrier const> buffers = {};
};

// on the recorder:
void barrier(barrier_group const& group) noexcept;
void barrier(image_barrier const& one)   noexcept;
```

Measured facts behind the omissions: there is **no cross-queue submission**, and the 46
`VK_QUEUE_FAMILY_*` tokens are `IGNORED` initialisations, so **no ownership-transfer field**. An
optional `stage_hint` is deliberately NOT pre-added: add it only if a migrated site demonstrably
needs a wider mask, under the `struct_size` rule below.

## 4. `rendering_info` (29 begin / 22 end; replace `vkCmdBeginRendering`)

```cpp
enum class load_op  : std::uint32_t { load = 0, clear, dont_care };   // APPENDED
enum class store_op : std::uint32_t { store = 0, dont_care };         // APPENDED

struct color_attachment {
    rhi::image_view* view   = nullptr;      // the engine's attachments are views
    load_op          load   = load_op::load;
    store_op         store  = store_op::store;
    float            clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // read only when load == clear
};

struct depth_attachment {
    rhi::image_view* view          = nullptr;
    load_op          load          = load_op::load;
    store_op         store         = store_op::store;
    bool             read_only     = false;    // depth read vs depth+stencil attachment
    bool             has_stencil   = false;
    float            clear_depth   = 1.0f;
    std::uint32_t    clear_stencil = 0;
};

struct rendering_info {
    std::uint32_t                     struct_size = sizeof(rendering_info);  // see section 6
    rect                              area        = {};    // CREATED here: rect did NOT exist in the contract before this batch
    std::uint32_t                     layer_count = 1;     // 6 sites use a non-1 value
    std::span<color_attachment const> colors      = {};    // 1-2 entries per call
    depth_attachment                  depth       = {};
    bool                              has_depth   = false;
    bool                              secondary_contents = false;  // 6 sites USE secondary buffers
};

void begin_rendering(rendering_info const& info) noexcept;
void end_rendering() noexcept;
```

Deliberately **absent**: `pResolveAttachments`/`resolveMode` (no evidence any pass resolves MSAA
in a rendering scope) and `viewMask` (no multi-view use found). Both can be appended later because
`rendering_info` carries `struct_size`. Measured support for what stayed: 3 `LOAD_OP_CLEAR` sites,
6 non-default loads, 8 store mentions, 6 non-1 `layerCount`, 6 `CONTENTS_SECONDARY` sites.

## 5. `pipeline`

The contract already has `pipeline` (abi 8); `vulkan/pipelines/pipelines.cppm` (76 KB, vk=294) is a
wrapper on top of it that is the largest remaining Vulkan contact in the engine.

1. `pipeline_handle` (line ~120) holds BOTH `rhi::pipeline* owned` and a raw `VkPipeline`
   (`:124`, `:136`, `:137`, `:143`): drop the `VkPipeline` member and reach the native one through
   `escape().native_pipeline()` - measured: **1 call site**.
2. `vkCreateComputePipelines` (12) + `vkCreateGraphicsPipelines` (2) -> `api_core::create_pipeline`
   (EXISTS) with the full state descriptor. The builders already take contract vocabulary
   (`rhi::blend_mode`, contract formats), so this is completing the descriptor (sample counts,
   depth/stencil, dynamic state, push-constant ranges are NOT needed - see section 0).
3. `shader_module_handle` (`:57`) -> the contract's `create_shader` (EXISTS).
4. `ckCreatePipelineLayout` is NOT needed: no pipeline in this renderer has a layout.

## 6. Structures: which are frozen and which grow

- `rendering_info` and `barrier_group` carry `struct_size` as their FIRST member. The backend reads
  each field only if the caller's declared size covers it (the repo already does this field by
  field in `sanitize_sampler_desc` via a `covered_by` helper). These two may therefore GROW.
- `rect`, `viewport`, `subresource_range`, `image_barrier`, `buffer_barrier` are FROZEN: adding a
  field to a by-value/`const&` contract POD is an abi change (a by-value-returned POD is frozen by
  the rule already recorded in `DYNAMIC_LINK_BOUNDARY_GOALS.md`).
- New enum VALUES are appended and never reused (the rule at `rhi.contract.cppm`'s `abi_version`).
- Adding the virtuals and the descriptors -> **abi 19 -> 20**, with the reason written at
  `abi_version`, and probe + `tests/spike_backend_boundary.cpp` + `tests/test_dynamic_link.cpp` +
  the scaffold pinned in the same batch.

## 7. Acceptance criteria (unchanged from every batch before this one)

Per commit: build exit 0; `ctest` (currently 19/19); `clang-format-check` 0;
`python scripts/check_backend_boundary.py --config dynamic --require-zero` -> **exit 0** (boundary
0 symbols, import meter 0 - the extraction adds no boundary symbol because virtual calls do not);
spike `--with-device` -> 0 failed and self-exiting; scaffold `test_runtime_dyn.exe --with-device` ->
0 failed; and the render gate
`pwsh -File scripts/windows/check_render.ps1 -Full -BuildDir build-release-dyn-clang64 -Compare frozen`
-> **exit 0, matched 14 / mismatched 0** with zero VUIDs.

The sweep is measured, not asserted: **engine-side `vkCmd*` call count 166 -> 0** and **engine files
that include a Vulkan header 62 -> at most 7** (the escape bucket: 22 `native_image_view`,
8 `native_command_buffer`, 2 `native_command_buffer_of`, 1 `native_pipeline`). Report both numbers
every batch.

## 8. Working method and known traps

- Work on a WIP branch (`git switch -c wip/recording-face`); commit freely there; keep `master`
  green and untouched; land only when the whole gate passes. **Never push** - the Lead verifies and
  pushes.
- One pass per commit once the pilot lands. `runtime.frames.cppm` (18 barriers) is the largest
  single chunk and should go last.
- **Two documented, retryable infrastructure flakes** (recorded in
  `DYNAMIC_LINK_PROGRESS.md` section 18.7 and section 19): a ninja PCM write failure
  (`unable to open output file '...pcm'`, `ERROR_USER_MAPPED_FILE`) and the docs build's
  `epstopdf`/`xpdf: reading PDF image failed`. Retry ONLY those two signatures; any other error
  stops immediately.
- Do not leave stray `*.log` files in the repo root (they make the tree dirty). Put build logs in
  `%TEMP%`.
- One builder at a time. `git add` explicit paths.
- Report per batch: the six readings, the two sweep numbers (section 7), and anything not verified.
