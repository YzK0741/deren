# The dynamic runtime: the design for step 2

**Status: design, not code.** A1 landed the ownership hand-over (the render chain's fourteen targets are created,
held and released by the engine, through the contract); what is left of the engine's dependency on the backend is
the runtime itself. This is the shape of the second runtime that does not name `core` at all, written before any
line of it exists so the coexistence rules are stated rather than discovered.

## 1. Shape: a directory, the SAME module names, and one runtime per build tree

* **`runtime/`** at the repository root, a peer of `vulkan/`, `promise/` and `utility/`. It holds the same five
  module units the renderer has today - `runtime.cppm` (`export module deren.vulkan.runtime;` +
  `export import :declarations;`) and the four partitions `:declarations` / `:constructor` / `:frames` /
  `:probes` / `:readback` - under the SAME module names. That is what keeps `main.cpp`, `chores`, the passes and
  the tests unchanged: they import a name, not a path.
* **One runtime per build tree, chosen at configure time:**
  * `-DVR_RUNTIME=legacy` (default, today): `vulkan/runtime/*` is in `vulkancorekit`.
  * `-DVR_RUNTIME=dynamic`: `runtime/*` is in `vulkancorekit`, and `vulkan/runtime/*` is **not compiled at all**.
  Why exclusive rather than "both in one build": two BMIs of one module name cannot both be visible to one
  target, so a single tree cannot host both runtimes. Two trees over one source tree, with one switch, is the
  arrangement - and it is what makes the acceptance below a comparison: the legacy tree is the CONTROL.
* **What is SHARED and therefore has to stay green in both trees:** the contract (`promise`), `utility` (the
  loader lives in `deren.utility.dynamic_link`), `vulkan_constant_init` + `vulkan/render_layout` (the slot grid,
  the formats, `bloom_level_count` - A1.0/A1.5), every pass, `deren.vulkan.pipelines` (its builders already take
  `rhi::api_core&`), and the app.
* **CI:** a second configure of the same tree, `-DVR_RUNTIME=dynamic`, added to the existing job (or as a sibling
  job once it renders): ctest, the boundary/import meter, the spike, then the render gate. The 14 references are
  the RENDERER's, not the runtime's, so the dynamic tree must reproduce the same fourteen hashes - that is the
  whole acceptance for a runtime swap.

## 2. Who builds/owns what while both exist

* **The backend does not know which runtime is in front of it.** `deren_vulkan` is compiled from the same sources
  in both trees and offers the same contract + the three C entries; it is STATIC in both until the flip (step 4).
  So step 2 is developed with the backend still STATIC, and `deren_make_api_core()` is exercised exactly as
  `test_dynamic_link` and the spike already exercise it today.
* **The dynamic runtime is the only engine-side caller of the C entry**: it holds
  `std::shared_ptr<rhi::api_core>` built around `deren_make_api_core(rhi::abi_version, &options, &error)` with
  `deren_destroy_api_core` as the deleter, takes the face as `*core_owner`, and reaches raw Vulkan through
  `query_extension<rhi::vulkan_escape>()`. The two measured invariants already in the tree apply: **never
  unload** (the loader's `detach()`), and construct only against the contract's `abi_version`.
* **The legacy runtime keeps `std::make_shared<core>(options)`** and is not touched by this step.
* **The one deliberate duplication is the render-chain creation run** (A1's `create_render_chain_targets`, now
  ~250 lines): the dynamic copy re-expresses it against the face instead of naming `core::heap_slots`. The two
  copies are held in step by the 14-hash gate while both trees exist, and the legacy one is deleted in step 3
  (the atomic flip), which is when the duplication ends. Everything the copies must agree on (slot numbers,
  formats, the bloom level count) already lives in `deren.vulkan.render_layout`, so the agreement is a shared
  module rather than a review.

## 3. The eight import points, point by point (measured)

The import meter reads **8 sites / 8 files** today. A grep of the class name `core` in each file separates three
kinds: vestigial (only comments name it), the new runtime's own, and two facade types that really do hold it.

| # | site | what it actually names today | who takes it |
|---|---|---|---|
| 1 | `runtime.declarations.cppm:62` `export import deren.vulkan.core` | the whole backend class: `std::shared_ptr<core> core_owner`, `core& vulkan_core`, and ~28 distinct backend facts (24x `logical_device`, 17x `heap_grid_offset`, 8x `swap_chain_images`, 6x `window`, 6x `depth_attachment_format`, 5x `ray_query_available`, ...) | **the new runtime's own `:declarations`** (this file is the work): `shared_ptr<rhi::api_core>` + `api_core& face` + `escape()`. Mapping of the ~28, by measured count: (a) contract virtuals already on `api_core` - `logical_device` -> `escape().native_device()`, plus `create_buffer`/`create_command_buffer`/`wait_idle`/`walk_frames`/`begin_commands`/`frame_image`/`frame_readback_buffer`/`query_extension`/`copy_image_to_memory`; (b) `escape()` - `heap_grid_offset` -> the heap writes' own answer + `descriptor_heap::ready()`, `swap_chain_images` -> `frame_image()` + `native_image`, `depth_attachment_format` -> `escape().native_image_format(*gbuffer_depth_images[i])`; (c) `abilities()` / existing extension queries - `ray_query_available`, `mesh_*`, `ray_tracing_pipeline_properties`, `descriptor_heap_limits`, `device_properties`; (d) the `create_info` the engine already receives - `window`, `render_scale`; (e) **to be decided at implementation time, measured first**: `swap_chain_image_format`, `graphics_queue_handle` / `graphics_queue_family_index`, `physical_device`, `instance` (~6 sites) - each is either an existing answer or a one-line escape addition, and none of them may drag an abi bump without that being the stated reason. |
| 2 | `runtime.constructor.cppm:44` `core.pipeline` | nothing: the file's comment says "for the post-process pipeline", but `deren::vulkan::make_pipeline` has NO engine caller (its callers are `core.cpp` / `core.api_core.cpp`, both backend) | **delete** - the same vestigial-import class earlier boundary batches removed five of. Proven by building, not by grepping. |
| 3 | `runtime.frames.cppm:38` `core.pipeline` | nothing (same) | **delete** |
| 4 | `runtime.probes.cppm:38` `core.pipeline` | nothing (same); its builders come from `deren.vulkan.pipelines` | **delete** |
| 5 | `pipelines.cppm:39` `deren.vulkan.core` | nothing: `core` appears only in comments, and every builder's first parameter is already `rhi::api_core&` | **delete** |
| 6 | `primitive.cppm:42` `export import deren.vulkan.core` | nothing in code, but it is an **EXPORT** import, so it is part of this module's published surface | **delete after a consumer check** - the compiler and the meter are the proof |
| 7 | `filters.cppm:50` `export import deren.vulkan.core` | REAL: `user_filter` / `pass_filter` hold `std::shared_ptr<core> owner_share` and `core* vk_core` | the facade types take `std::shared_ptr<rhi::api_core>` / `rhi::api_core&` (the work the engine-side owner type has owed since the construction step), in **both** trees - the legacy runtime can hand them the same contract reference, which is what removes the import from the flip's path |
| 8 | `readback.cppm:26` `export import deren.vulkan.core` | REAL: `explicit readback(core& device)` and `core* gpu` | takes `rhi::api_core&` (+ `escape()` where the copy needs a native handle), in both trees |

So the meter's 8 splits as **5 deletable by measurement** (2-6) and **3 that are real work** (1, 7, 8). A prep
commit doing 2-6 first is worth landing on its own: it moves the reading before the dynamic runtime exists, and
it tells the truth about what step 2 actually has to convert.

## 4. Acceptance (what "the dynamic runtime is done" means)

1. `-DVR_RUNTIME=dynamic` configures, builds (exit 0), `ctest` 18/18, clang-format-check 0;
2. the boundary ratchet still reads 2 with no stale/untracked, and the **import reading DROPS** by the prep
   commits plus whatever the new runtime contributes (the target stays 0 for the runtime; the remaining sites
   are whatever 7/8 still owe at that point, and they are written down rather than discovered);
3. spike `--with-device` 96 checks / 0 failed / self-exited;
4. **the 14 scenario hashes are byte-identical in the dynamic tree**, AND the legacy tree still reads 14/14
   (the control), with validation-layer VUID 0 in both;
5. `--require-zero` is red only on the two whitelisted symbols + the imports still outstanding, and the reading
   is recorded in `DYNAMIC_LINK_PROGRESS.md`.

## 5. What is NOT in this step

Touching `vulkan/runtime/` (the legacy copy stays byte-for-byte as it is, so the control is honest); flipping
`deren_vulkan` to SHARED (step 4); renaming the module or moving the legacy files out of `vulkan/` (that is the
step-3 deletion); and converting any pass - the passes are already contract-side.
