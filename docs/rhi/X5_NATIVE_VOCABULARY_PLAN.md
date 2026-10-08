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
