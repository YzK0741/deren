# Current pass resource inventory

Source-backed inventory, 2026-10-12; ownership update, 2026-10-14.

> **NOTE 2026-10-17:** the MegaLights trace/temporal rows and the ml_trace/ml_resolve/ml_history images were
> historical - the chain was removed temporarily on 2026-10-17 (the render graph gate review records it).
> The two rows themselves were removed from the pass table on 2026-10-18; the gate review keeps the history.
 This describes production frame passes and their related compute
jobs, not a proposed resource system. Debug validation probes and offline asset/IBL preparation are
outside the pass table. Conditional resources are needed only when the corresponding feature runs.

Raster shadow images, their view bundles, clustered-light count/index buffers, camera/light/head constant buffers, G-buffer attachments, HDR and
physical scene-input, TAA history, LDR, Bloom and RT visibility images, plus shared
motion-transform, skin/previous-skin, morph and nine shared scene-table buffers now belong to
the passchain resource registry. The
runtime borrows their handles and maintains the existing heap publication and hand-back barriers;
other allocations in the table retain the original ownership. See [passchain-managed resources](managed_resources.md).

## Actual GPU access channels

| Channel | GPU spelling | Host responsibility |
| --- | --- | --- |
| H: descriptor heap buffers | `heap_at<ConstantBuffer<T>>`, `StructuredBuffer<T>`, `RWStructuredBuffer<T>` | Allocate buffer/range, publish a correctly typed heap descriptor, retain backing memory |
| H: sampled images | `Texture2D`, `TextureCube`, `Texture2DArray` with separate sampler; `Sample`, `SampleLevel`, `Load` as appropriate | Own image/view/sampler and publish sampled descriptor |
| H: storage images | `RWTexture2D<T>` indexed writes | Publish storage descriptor and order shader writes before later readers |
| D: device addresses | Push address halves/uint64, cast to `uint*`, `float*` or a structured pointer | Own the addressed buffer and preserve its range/stride/address until GPU completion |
| M: mapped AS binding | RT shadow `mapped_tlas[frame_slot]`, set 0/binding 16 mapped by backend to heap slots | Publish per-slot TLAS and keep AS/backing buffers alive; pipeline defines mapping |
| A: attachments | Raster color `SV_Target`, rasterized depth/depth test, blending | Host supplies attachment views, load/store/depth state; these writes are not RWTexture accesses |
| C: copy/indirect commands | GPU transfer copy, indirect draw argument fetch | Declare source/destination/indirect buffer use even when no shader fetch names it |
| P: pushed parameters | `[[vk::push_constant]] ConstantBuffer<...>` | Native heap path uses `vkCmdPushDataEXT`; carry small parameters, indices and addresses |

Most production heap pipelines do not bind a separate conventional descriptor set per pass.
The RT shadow TLAS mapping and the backend VRDX sorter are important special cases; the sorter uses
traditional push descriptors and explicitly restores descriptor-heap bindings afterward. The RHI
texture-group interface also exists, but current PBR material sampling still uses material texture
indices in the shared heap; do not label every production material fetch as a texture-group fetch.

## Index domains

F = frame slot, I = acquired output-image index, G = single scene/device binding. F and I are separate
identities even if their counts happen to match in windowless mode. G does not imply immutable data.

- F: camera/light blocks, current/previous skin matrices, morph data, previous transforms, shadow map,
  cluster lists, TLAS and mask-instance data, ray-traced visibility.
- I: G-buffer attachments/depth/velocity, scene color/HDR/LDR, bloom pyramid, TAA history
  and per-image processing outputs.
- G: material and instance-transform descriptors, scene textures, meshlet table, IBL cubes/LUTs,
  toon rig/material tables and other explicitly shared lookups. Geometry lives with its scene/asset;
  selected per-frame derived geometry and transient buffers have their own slot lifetime.

## Pass table

Resources below include shader inputs and attachments; H/D/M/C/P refer to the access channels above.
Geometry input means vertex/index buffers plus optional meshlet tables. Raster scene geometry uses
mesh shaders fetching vertex/index data by device address, not a traditional vertex input assembler.

| Pass/job | Inputs | Outputs / other GPU use | Access |
| --- | --- | --- | --- |
| Scene / G-buffer / unlit | Geometry D; camera F; material table/textures G; current skin/morph F; current instance transforms G; previous transforms/skin/morph F for motion | G-buffer albedo, normal/roughness, material/AO, velocity, scene-color attachment and depth I; unlit path depends on selected pipeline | H+D+P, A |
| Raster shadow | Geometry D; cascade matrices/light F; instance G; skin/morph F; MASK material/albedo G | Layered depth image F, one attachment view per cascade | H+D+P, A; later `Texture2DArray<float>.SampleCmpLevelZero` |
| Cluster lights | Camera and punctual-light records F | Cluster counts/indices buffers F | H: constant/structured reads, RWStructuredBuffer writes |
| Deferred lighting (SSAO inline) | G-buffer I; camera/light F; shadow F or RT visibility F; cluster lists F; IBL environment/irradiance cubes and BRDF LUT G | Scene-color attachment I; preserves/composes material emissive already in the chain | H+P, A; SSAO is not a separate registered frame pass |
| Transparent PBR | Geometry D; material textures G; camera/light F; shadow/cluster F; IBL G; current deformation F | Alpha blend into scene color I, hardware depth test against depth I | H+D+P, A |
| Character forward | Geometry D; current deformation F; camera/light/head block F; material/toon lane/color/rig tables and textures G; toon lighting inputs incl. shadow/IBL as selected by shared helpers | Scene-color blend I and depth test; includes character-specific material shading | H+D+P, A |
| Toon screen rim | G-buffer albedo/depth/material I; material/toon records G; camera/light F | Scene-color blend I | H+P, A |
| Game toon depth rim | G-buffer albedo/depth/material I; material/toon color records G; camera/projection F (pushed) | Scene-color blend I | H+P, A |
| RT shadow | G-buffer depth/normal I; camera/light F; TLAS F; mask-instance records F; MASK materials/textures G; any-hit vertex/index addresses D | Visibility storage image F; ray generation/miss/any-hit traversal | H+M+D+P; `TraceRay(mapped_tlas[frame_slot], ...)`, RWTexture2D output |
| Gaussian preparation | Point/SH asset data, candidate ranges, camera/preparation parameters; per-frame keys/values/status/projected-cache buffers | Cull/project outputs, visible count, indirect draw args, sorting input buffers | D+P; raw-pointer reads/writes and atomics |
| Gaussian sorting | Key/value and scratch buffers, element count | Sorted key/value buffers | RHI `gpu_sorting`; Vulkan VRDX push-descriptor compute path, followed by heap rebind |
| Gaussian raster | Sorted references, point/SH data or projected cache, pushed camera parameters | Scene-color alpha blend I, hardware depth test I; indirect argument buffer fetch | D+P+C, A; fragment uses interpolated Gaussian color/opacity without material texture sampling |
| TAA | Current scene color, history, velocity, depth I; pushed reprojection/blend parameters | HDR attachment I, then transfer copy to history I | H+P+C, A |
| Bloom prefilter/downsample | HDR or prior bloom level I; pushed threshold/mode | Four bloom-pyramid levels I | H+P, A; source heap slot is explicitly pushed |
| Post composite | Selected HDR/source I; four bloom levels I; display grading LUT G; pushed exposure/encoding parameters | Display LDR intermediate I when AA/upscale needs it, otherwise output image I | H+P, A; host resolve can redirect the declared output |
| FXAA | Display-color image I; pushed texel/edge parameters | Host-selected display/output attachment I | H+P, A |
| Upscale (EASU / linear, optional RCAS) | Display-color image I; pushed input/output sizes and sharpness | Intermediate/output attachment selected by host | H+P, A |
| G-buffer debug | G-buffer albedo/normal/material/depth/velocity I; camera F for depth display | HDR attachment I, then normal post chain | H+P, A |
| Compute skin job | Vertex buffer address D; skin matrices F; pushed source/destination stride and joint base | Deformed position/normal/UV buffer D used by BLAS build/refit | H+D+P; compute write -> AS build dependency |
| Mask bake job | Vertex/index addresses D; material table/albedo texture G; pushed geometry/material metadata | Expanded or masked geometry buffer D used by BLAS construction | H+D+P |

Outline and eye/hair overlay are additional draw stages in the scene/character orchestration rather
than separate `frame_pass` classes. They reuse geometry addresses and material/toon data; outline can
also sample G-buffer normals, and overlay reads mask textures/rig state while multiplying/blending
scene color through attachments. GUI draws are another external overlay integration.

BLAS/TLAS construction/refit, IBL generation and screenshot/readback copies must also enter a future
resource-use model as producer/transfer tasks, although they are not rows in the registered pass list.

## Gaps relevant to resource management

The current declaration schema is a useful foundation, not a complete actual GPU use graph:

1. Material/instance scope now matches their single global buffers. Mesh indirect/statistics/culled
   tables are global allocations with internal frame regions; allocation scope does not erase that
   region index. TLAS schema and typed AS publication now select the frame slot; empty/failed builds
   retain allocation while withholding a ready traversal handle.
2. Scene passes intentionally have no own descriptor binding list; shared heap inputs are still
   reached by shader code. Toon tables and mesh indirect/culling storage now have ids. Ten Gaussian GPU-frame
   address buffers and two CPU raster upload buffers are declared in per-slot resource subchains; geometry/SH
   assets have scene-lifetime subchains. Mask/skin output buffers now have scene subchains and retained
   BLAS dependency leases. Micromaps have typed scene subchains and BLAS leases. Production geometry
   uploads have scene subchains; BLAS retains vertex/index leases. Legacy imports retain their external
   device/CPU-scene obligations. Shared texture/IBL/furnace/LUT images, samplers and pass/SBT uploads
   now have managed subchains as well. Pushed source selection
   and AS-preparation jobs still need complete representation.
3. Sampled and storage descriptors can name the same image; descriptor identity is not allocation
   identity. Likewise, an address is not an owning reference, and an attachment write is not a
   shader storage write.
4. Transfer updates to temporal history and whether a conditional pass actually ran must be recorded
   separately from potential binding access. Per-image history storage does not by itself identify
   which chronological frame contributed the current history.

A future manager must record the actual resource object, subresource/range, index domain, access,
producer/consumer stage, resource generation and completion-dependent retention. Bindings and device
addresses are publication methods for that resource, not independent lifetime owners.

## Production ownership audit

The production pass/shared-resource migration is complete; see
[final audit and validation](reviews/2026-10-14-shared-assets.md). Remaining direct allocation calls
serve diagnostic probes or HDR capture readback; old generic buffer helpers have no production callers.
Presentation images remain borrowed from backend frame ownership, with explicitly owned views.
Backend AS/micromap storage/scratch and descriptor-heap services stay inside their RHI owners. Legacy
custom/static imports still require their documented device and CPU scene lifetimes. The use-graph gaps
above describe future access/ordering work, not outstanding production allocation migration.

## Source anchors

- `source/engine/render_resource/render_resource.cppm`: resource schema, binding access, pass I/O.
- `source/shaders/common/heap_access.slang` and `heap_slot_constants.slang`: typed fetches/index domains.
- `source/shaders/geometry/mesh_geometry.slang`, `compute_skin.slang`, `mask_bake.slang`: device-address paths.
- `source/shaders/lighting/rt_shadow.slang`: the mapped-TLAS traversal binding method (the MegaLights trace that
  was the second one is gone with its chain).
- `source/shaders/gaussian/gaussian_prepare.slang`, `gaussian_raster.slang`: address-based Gaussian buffers.
- `source/backends/vulkan/core/radix_sort.cpp`: VRDX push descriptors and restored heap state.
- `source/engine/pass/taa.cpp`, `post.cpp`: history copies and routed outputs.
