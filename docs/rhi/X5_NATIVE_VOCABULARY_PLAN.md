# X5: engine resource vocabulary migration

Implements the existing `NATIVE_HANDLE_EXIT_PLAN.md` X5 design. The engine must
use RHI objects and semantic descriptors, preserve ownership and rendering, and
keep P-Nm/P-Import at zero. P-Census counts engine files, not plugins; do not
reduce it through new exclusions or opaque replacements of API types.

## Approach and order

1. **Shared samplers and obsolete mapping (this batch).** Publish borrowed
   `rhi::sampler*` from the runtime's existing owners. Delete the unused
   declaration-to-Vulkan mapping module: its only consumers are obsolete tests.
   Remove unused Vulkan includes from already migrated pass jobs.
2. **Resource publication.** Change `resource_handles`, `resolved_binding` and
   family publication together. Use the existing RHI image/view/buffer channels;
   remove the parallel raw fields and all consumers, including null checks.
   Preserve per-image/per-slot indexing and resource lifetime across recreation.
3. **Semantic values.** Use RHI image formats, extents, index types, cull modes,
   heap descriptors and view descriptions from producers through consumers.
   Add contract concepts only where real consumers need a missing capability;
   translate API values exclusively in the backend.
4. **Remaining escape paths.** Move image descriptor writes and RT shader-group
   operations behind their contract objects. Change GUI creation to a contract
   device plus window; the API-specific GUI plugin obtains backend details.
5. **Portability proof.** Complete the no-Vulkan engine compilation and injected
   non-Vulkan runtime checks, then make native vocabulary a zero-required gate.

Do not replace types with integers or `void*` merely to pass the census. Moving
all consumers at once is riskier than vertical batches; retaining aliases would
leave the dependency intact. Existing RHI concepts are the preferred route.

## This batch: exact changes and checks

- [x] In `tests/test_pass.cpp`, exercise sampler hints with real fake RHI sampler
  objects, pointer identity, `none`, an unpopulated hint and an unknown hint.
  Assert `sampler_set::of()` returns `rhi::sampler*`; observe a compile failure
  against the old sampler table before changing production code.
- [x] In `vulkan/render_resource/shared.cppm`, replace all six sampler fields and
  `of()` with borrowed `rhi::sampler*`, initialized to null. Import RHI rather
  than a Vulkan header. The runtime remains the owner.
- [x] In `runtime/runtime.frames.cppm`, return pointers to the existing six
  sampler objects directly; do not query `vulkan_escape` in `shared_samplers()`.
- [x] Delete `vulkan/bindings/bindings.cppm`, its CMake entry, unused imports in
  runtime files, and enum mapping tests. Repository search must show no live
  module/function reference. Descriptor heap creation is unchanged.
- [x] Remove unused Vulkan headers from `compute_skin.cppm`, `mask_bake.cppm`,
  `mask_bake.cpp` and `ray_traced_shadow.cpp`. Build proves no transitive reliance.
- [x] Build the engine and pass test with the dynamic backend, run CTest,
  formatting, boundary gates and frozen rendering. Expect P-Census 31 -> 25,
  P-Nm/P-Import zero, frozen rendering 14/14. Record actual results in progress.

## Review focus

- Borrowed sampler pointers must never release resources or outlive the runtime.
- Empty or unknown hints return null; populated hints preserve object identity.
- Deleted mappings have no production consumer or documentation-test requirement.
- Header removal must compile all affected jobs, including RT-related files.
- RT `device_lost` remains an independent unresolved acceptance failure; a green
  raster gate must not be presented as RT acceptance.

RHI ABI remains 27: no exported RHI virtual or descriptor changes in this batch.
The first batch is committed locally as `7d053bb` at the user's request; further X5 batches remain open.

Verified: full dynamic build, CTest 19/19, test_pass 169/0, frozen render 14/14,
formatting and dynamic boundary gate. Native census 25; P-Nm/P-Import zero.
Independent read-only review found no actionable defect in this batch.

---

## Remaining batches, measured (2026-10-08, after the first batch)

The census is **25 engine files, 25 distinct `Vk*` types and 40 distinct `VK_*` macros**. Measured
distribution (`python scripts/check_native_boundary.py --verbose`), by number of files each name appears in:

| type | files | macro | files |
|---|---|---|---|
| `VkFormat` | 9 | `VK_NULL_HANDLE` | 15 |
| `VkImageView` | 9 | `VK_FORMAT_UNDEFINED` | 5 |
| `VkBuffer` | 8 | `VK_INDEX_TYPE_UINT16/_UINT32` | 4 / 3 |
| `VkDeviceAddress` | 8 | `VK_SAMPLE_COUNT_1_BIT` | 4 |
| `VkExtent2D` | 8 | `VK_FORMAT_R8G8B8A8_UNORM/_SRGB` | 4 / 2 |
| `VkImage` | 7 | `VK_DESCRIPTOR_TYPE_*` | 3+2+2 |
| `VkDeviceSize` | 6 | `VK_IMAGE_ASPECT_COLOR_BIT`, `VK_IMAGE_LAYOUT_GENERAL` | 2 / 2 |
| `VkCommandBuffer` | 6 | | |
| `VkBindHeapInfoEXT` | 5 | | |
| `VkDevice` / `VkPhysicalDevice` | 4 / 2 | | |

### B2 - resource publication (the double lane is the thing to remove)

`pass::resolved_binding` carries TWO lanes today: the raw `VkImageView view` / `VkBuffer buffer` /
`VkImage image`, and the contract `rhi::image_view* view_handle` / `rhi::buffer* buffer_handle` /
`rhi::image* image_handle`. Its own comment says why: "the raw lane stays for the passes that have not
migrated and for the third-party recording that never will". THE BATCH IS: find which passes still READ the raw
lane, migrate each to the contract lane plus the recording face's verbs (`barrier`, `begin_rendering`,
`bind_*`), stop publishing the raw fields in `runtime.frames.cppm`, then DELETE the three raw fields.

56 references across 6 files: `runtime/runtime.frames.cppm` 24, `vulkan/pass/pass.cppm` 22,
`vulkan/core/filter/filters.cppm` 5 + `.cpp` 2, `vulkan/pass/fxaa.cpp` 1, `vulkan/pass/post.cpp` 1,
`vulkan/render_start_demo/render_start_demo.cpp` 1. `VkCommandBuffer` (6 files) belongs with this batch: a
pass's recording signature should take `rhi::command_buffer&`, which is what the runner already holds.

Instrument: frozen render 14/14 (every pass is on that path) plus `test_pass` - and each raw-lane reader has
to be FOUND, not inferred: grep for the field names, migrate, and let the compiler prove the lane is empty
before deleting it (an unused lane is invisible; a reader the compiler can still see is not).

### B2 - DONE (2026-10-08)

The double lane is GONE. `resolved_binding` carries only the contract handles; `resource_table::family_entry` lost
its `views`/`images` spans; `publish_family(id, element, image_handles, view_handles)` no longer takes the native
pair at all; `resolved_io::own_per_image` is a run of `rhi::image_view*`; and `filters`'s `resource_handles` is
the contract trio.

Readers migrated: the four resolve validators in `pass.cppm` (`view/buffer/image == VK_NULL_HANDLE` -> the
handles being null), the family fallback in `find()`, `views_of`/`instances_of`, the resource-table consistency
check in `runtime.frames.cppm`, and five passes that guarded on a raw view (post, deferred, goo_rim,
toon_screen_rim, geometry_buffer_debug). The publisher in `runtime.frames.cppm` fills one lane now: the `single`,
`buffer`, `image` and `owned_family` helpers, the render-target chain, the RT visibility image, the shadow
cascades, the IBL triple and the furnace cube.

`tests/test_pass.cpp` was THE LANE'S UNIT TEST (it published and asserted the native spelling), which is why the
first attempt at this batch - migrating only the readers - made it SEGFAULT: the pass refused to record and the
test dereferenced what it expected. It is migrated too: its fake handles are `rhi::image_view*`/`rhi::image*`/
`rhi::buffer*` sentinels, and `publish_family`'s two spans are passed in the new order.

Verified: full build, CTest 19/19, `test_pass` 169/0, frozen rendering 14/14, clang-format-check clean,
P-Nm/P-Import 0. RHI ABI stays 27 (no exported contract change - this batch only removed engine-side fields).

Measured note: P-Census stays at 25 FILES and its token count went 205 -> 209, because the census counts source
TEXT and the new comments NAME the removed types to explain what left. The dependency is what changed: the raw
lane no longer exists in code. The raw vocabulary that remains is the escape helpers (`buffer_of`,
`scene_target_image`, `scene_target_view`, `write_sampled_target`, `write_storage_target`, `native_target`) and
the families of step B3 - B4 and B3 respectively.
### B3.1 - DONE (2026-10-08): VkDeviceAddress leaves the engine

51 sites in 11 engine files stopped naming `VkDeviceAddress`; the spelling is `std::uintptr_t` (the user's
ruling). The API's own type is `std::uint64_t` and the CONTRACT still returns `std::uint64_t`
(`buffer::device_address()`), so on every target this project builds for the two are the same width and the
change is a spelling change - recorded here because a 32-bit host would make them differ. RHI abi stays 27.
Measured: P-Census tokens 209 -> 201.

Verified: build, CTest 19/19 (parallel AND serial, see below), frozen rendering 14/14, RT acceptance
(`check_rt.ps1`: GPU traversal verified, 40 frames, clean validation, screenshot).

### B3.2 - DONE (2026-10-08): the engine's size vocabulary is rhi::image_extent

31 sites in 12 engine files stopped naming `VkExtent2D`: runtime.constructor/declarations/frames/cpp,
init_utils, core.cpp/core.api_core/core.constructor/core.declarations, and the four pass files
(scene, transparent, character_forward, shadow). `rhi::image_extent` is `{width, height, depth = 1}`, so a 2D
size is the same aggregate with the contract's own depth default.

TWO CONSEQUENCES worth recording, because both are the shape this migration keeps taking:

- **The identity helper died.** `contract_image_extent(VkExtent2D)` existed only because the two halves spelled
  a size differently; with one spelling it was a conversion from a type to itself, so it is gone and its two
  call sites use `render_extent()` directly.
- **One API-facing conversion took its place.** `core::render_extent_2d()` is the single place that builds a
  `VkExtent2D` out of the contract value, because `VkRect2D`'s extent (the scissors in `core.cpp`) and the
  swapchain's `create_info.imageExtent` are Vulkan structs that a Vulkan call needs. A Vulkan struct is built
  where Vulkan is spoken - not stored in the engine's own declarations.
- A structured binding of a 2D extent (`auto const& [width, height] = render_extent()`) broke: the contract
  type has three members. It now binds the extent once and reads its two fields.

Measured: P-Census tokens 201 -> 193. RHI abi stays 27.
Verified: build, CTest 19/19 (parallel), frozen rendering 14/14, clang-format-check.
### Housekeeping found by running the suite in parallel: ctest was NOT parallel-safe

`ctest -j 8` reported 18/19 while the same tree passed 19/19 serially: every test shared ONE working
directory (`build/test-run`), and `test_runtime_injection` reads, from a file, the panic text a CHILD process
left behind - so a test running beside it overwrote that file and the parent read the wrong panic (2 failed
checks about the abi refusal's text). Each test now has its own `test-run/<target>` directory. Verified
19/19 both ways. This is a test-harness bug, not an engine one, and it is why every earlier "19/19" in this
project's history was really "19/19 when run serially".
### B3 - semantic values, one family per commit

Ordered by blast radius, smallest first so a mistake stays local:

1. `VkDeviceSize` / `VkDeviceAddress` -> `uint64_t` (mechanical; 6 and 8 files). No behaviour.
2. `VkExtent2D` -> `rhi::image_extent` (8 files: `pass/scene.cppm`, `pass/transparent.cppm`,
   `pass/shadow.cpp`, `pass/character_forward.cppm`, `runtime/runtime.declarations.cppm`, ...).
3. `VkFormat` + `VK_FORMAT_*` -> `rhi::image_format` (9 files; the contract already has `bytes_per_pixel` and
   the format vocabulary `image::get_content` uses). `VK_FORMAT_UNDEFINED` becomes the contract's
   "no format" value, not a macro.
4. `VkSampleCountFlagBits` / `VK_SAMPLE_COUNT_1_BIT` -> the contract's sample count (2 files).
5. `VkCullModeFlags` / `VK_CULL_MODE_*` -> `rhi::cull_mode` (2 files), `VkBool32` -> `bool` (2 files).
6. `VkBindHeapInfoEXT` -> the contract's heap bind info (5 files; the heap requests are already contract PODs
   with a tagged chain, so this is a spelling change at the producers).
7. `VK_NULL_HANDLE` (15 files) disappears with the families above - it is the raw lane's null, so it must not
   outlive them; where a contract handle is meant, `nullptr`; where a count or an enum is meant, its own zero.

Instrument per family: frozen render 14/14 and `ctest`; a family that touches a pass's DECLARATION also needs
`test_pass` (169 checks today), and one that touches the frame loop needs the probe read-back lines.

### B4 - the last escape paths

Image descriptor writes and RT shader-group operations move behind their contract objects (the
`acceleration_structure_heap_binding` this session added is the shape: the pipeline description carries the
binding and the backend maps it). GUI creation changes to "contract device + window": `gui_create_info` stops
carrying `void*` handles and carries the `rhi::api_core` face instead, and the plugin asks the escape for what
it needs - which is what finally removes `VkInstance`/`VkDevice`/`VkQueue`/`VkFormat` from
`runtime/runtime.cpp` (today the heaviest remaining file after the constructor).

Instruments: the GUI gate (`scripts/windows/check_gui.ps1`) and the RT acceptance
(`scripts/windows/check_rt.ps1`) - a green raster gate is NOT RT acceptance.

### B5 - the portability proof, and then the gate closes

1. A second CMake configuration that compiles the engine with NO Vulkan include path and NO Vulkan library
   (the P-Gate of the exit plan). It is the strongest statement available and it is cheap once B2-B4 land.
2. An injected non-Vulkan `api_core` that the engine runs against (`tests/probe_backend` already is one: it
   announces no `vulkan_escape`), extended to drive a frame instead of only the handshake.
3. `scripts/check_native_boundary.py --require-zero` becomes the standing gate: P-Import 0, P-Nm 0,
   P-Census 0 - and the census stops being "report-only".
