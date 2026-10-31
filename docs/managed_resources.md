\page managed_resources Passchain-managed resources

This is shared engine infrastructure used by [subtilis](subtilis.md), [amnis](amnis.md) and other
rendering clients. It manages resource generations and lifetimes; clients retain their own policies
for geometry LOD/page selection and texture mip residency. A resource passing through this layer
does not make its selection policy part of the resource manager.

Passes can describe dynamic image and buffer requirements. The passchain collects all descriptions,
merges compatible producers, creates complete resource generations, and distributes a shared snapshot.
Production migrations now cover raster shadow images, clustered-light count/index buffers and camera/light/head
constant buffers, G-buffer attachments, HDR/scene-input, TAA history, LDR, Bloom and RT visibility images;
existing GPU bindings and barriers remain explicit.

Shared motion-transform, skin/previous-skin and morph buffers are also managed by
`scene_deformation_pass`; their setup and history contents survive provider replacement.

The dated per-step acceptance records that originally documented these migrations were maintainer
working material and are not distributed with this repository; the conclusions are summarized in the
sections below.

## Ownership and lifetime

The registry owns `resource_id -> shared_ptr<const generation>` (an engine object - `std::shared_ptr`
is the engine's container and no longer manages an RHI handle) and groups ids in
`map<lifetime, vector<resource_id>>`. A generation owns its device root as an
`rhi::shared_object<rhi::api_core>` and its RHI objects as `rhi::unique_object<T>`: allocations and
image views, destroyed so that views go before their images and the device root last. A pass holds a
snapshot and uses its objects. The runtime retains submitted snapshots until the corresponding
frame-slot wait succeeds. Destroying a chain drains its device before releasing its owned passes and
resources.

Lifetime labels are `device`, `scene`, `output_size`, `frame_slot`, `frame_scratch` and `history`.
They classify resources for management; the pass description supplies the actual replacement criteria.
Index scope is separate: device-wide means one instance, frame-slot scope uses the frame-slot count,
and output-image scope uses the output-image count supplied by the host. The runtime currently supplies
the RHI output-image bound because the contract does not expose a swapchain image-count accessor.

## Dynamic collection

`frame_pass::describe_resources(context, requests)` appends owned requirements or a dependency with
`create = false`. A dependency names a producer's id; it does not supply additional allocation constraints.
`pass_chain::prepare_resources(device, context)` collects the entire chain before allocation, so producer
order does not affect resolution. Conflicting descriptions, missing producers and invalid requests fail
without changing the active set or rebinding passes. A creation failure also leaves the former set intact.

Matching allocations are reused. Buffer byte capacity and image layer capacity can grow; shrinking
keeps the existing capacity. Extent, format, usage, lifetime, scope or instance-count changes create a
new generation. Snapshots of the former generation stay valid while their holders remain alive.
`bind_resources(snapshot)` publishes CPU views only; GPU descriptor writes must respect their own
completion rules. Image/buffer initialization uploads are deliberately unsupported in this first API;
nonempty `initial_bytes` is rejected rather than retaining a borrowed upload span. Mapped buffers can
request `zero_on_create` to initialize a newly allocated generation once; reused contents are preserved.

## Frame sequence

1. Collect and reconcile dynamic requests before acquiring a presentation image. Allocation failure
   must not leave an acquired image or a signaled binary acquire semaphore behind.
2. Wait for the selected frame slot and acquire successfully.
3. Release that slot's submitted snapshots and call `pass_chain::begin_frame(completed_slot)`.
   Only mapped buffers explicitly requesting `zero_on_frame` are reset, and only in that slot.
   History resources are preserved and cannot request this reset policy. The optional preserved-id list
   skips resources not consumed this frame; runtime applies it after updating its light facts.
4. Record through the distributed view and existing bindings/barriers.
5. After successful GPU submission, retain the snapshots for that slot until its next successful wait.

The caller must provide the completion wait: `begin_frame` is not a GPU synchronization primitive.
Images still use their declared render-pass clears; persistent images are not blindly cleared each frame.

## Raster shadow migration

`shadow_pass` declares layered depth images in frame-slot scope and retains its bound snapshot.
The chain creates the images, array views and per-layer views. The runtime holds borrowed shadow views
for existing shader-heap publication and resource-table resolution. When the generation changes, it
waits for device idle, updates the borrowed bridge and sampled descriptors, and invalidates cached shadow
content. This preserves the existing safe replacement rule. Independently managed light/head buffers
are reused when only shadow layers grow.

## Clustered-light migration

`cluster_pass` declares the per-frame count and index buffers at the existing maximum grid capacity.
Both use coherent storage memory and device addresses, and initialize to zero once. Only counts request
per-frame zeroing. Runtime skips this reset when no clustered dispatch runs; index rows retain their
contents and the dispatch rewrites the rows subsequently consumed by shading. Resizing changes the
active grid, so it does not reallocate these buffers.

Runtime borrows both buffers and retains a shared snapshot. Replacing the pair waits idle before
switching heap addresses. Admission checks require the complete pair, frame-slot scope, coherent storage
usage, valid sizes and device addresses before changing the bridge. Partial or incompatible providers
fail before acquiring a frame. Cluster barriers reference the distributed generation for that slot.

Hosts without migrated producers use a resource-only compatibility chain that declares just the missing
shadow, cluster and/or camera/light/head resources. The application chain remains independently collected and initialized.
Successful submissions retain both application and compatibility snapshots. Chain handover selects the
producers anew; cached compatibility pass objects are owned by the runtime's compatibility chain.

Native tests with 32 lights exposed two older bugs now fixed: the cluster dispatch omitted its heap
frame/image push indices, and shared shading read the host's floating light-count lane as uint bits.
The dispatch now pushes those indices first, and shading reads the numeric float with a bounded count.
Neither fix changes the host UBO layout or the RHI ABI.

## Scene constant migration

`scene_constants_pass` describes the camera block (272 bytes), light block (8608 bytes) and head block (64 bytes), each with
frame-slot scope and lifetime. They use coherent uniform memory with storage and device-address flags;
the existing shader layouts and heap slots remain unchanged. They initialize once and never request
`zero_on_frame`. Fresh camera blocks receive `camera_ubo{}`; fresh light blocks receive `light_ubo{}`; fresh head blocks receive `head_ubo{}`,
including its nonzero default axes. Reused generations retain their contents.

CPU parameter calculation stays in runtime. After a successful slot wait, runtime calls
`pass_chain::write_frame_buffer(completed_slot, id, bytes)` to upload that slot's current values.
The registry checks resource identity, frame scope, slot bounds, byte capacity and coherent mapping.
The caller supplies GPU completion; the upload method does not wait. Initial priming of new generations
occurs before their first use, or after the existing idle wait during replacement.

Camera is prepared with constructor output targets so setup-time heap publication has a usable buffer;
light/head, shadow and cluster resources remain lazy. Runtime holds borrowed camera buffers and a
generation snapshot. Camera projection, TAA jitter and previous-image matrices retain their existing
calculations; only ownership and the checked upload path change.

The application and compatibility chain can supply camera/light/head independently. Admission validates all
selected descriptions before changing borrowed handles or heap bindings and rejects per-frame clearing
for these constants. Replacing providers preserves submitted snapshots until their slot completes.
Arbitrary-time setters modify CPU state only; they cannot overwrite a constant block read by the GPU.

## Output target migration

`output_targets_pass` declares the base output image families at `resource_context::render_extent`: HDR,
G-buffer albedo/normal/material, velocity, physical scene input for TAA, and depth - plus the renderer's
later additions (TAA history, LDR, the four bloom levels, the ray-traced visibility and, from the
visibility-buffer work, the R32_UINT visibility buffer as family 14: sampled + color attachment, one per
swapchain image). These requests have
`output_size` lifetime and `per_swapchain_image` scope, with one instance per host-supplied output count.
The existing runtime still supplies the RHI output-image bound. Formats and attachment/sampled/copy
usage match the former allocation path; HDR retains transfer-source usage for history and readback.

THE VISIBILITY BUFFER'S RESOURCES (v0.4b/v0.5), and why two of them look unused from here:

- The five G-buffer/HDR families (`hdr`, `gbuffer_albedo`, `gbuffer_normal`, `gbuffer_material`, `velocity`,
  `scene_input`) carry STORAGE usage as well as their sampled/attachment usage: the compute resolve writes them
  through the descriptor heap, and a sampled descriptor and a storage descriptor are different heap slots for
  the same image. The storage twins live at heap slots 756/764/772/780/788 and are published every frame by
  `runtime::resolve_pass` (the scene-colour target alternates between the TAA working image and the HDR image,
  so which image that descriptor names is the frame's answer).
- Family 15 (`visibility_depth`, the capture-only step's private depth) is RETIRED FROM THE FRAME in v0.5: the
  raster lane is the opaque producer now, so it writes the shared `gbuffer_depth` instead, and nothing imports
  it any more. The PRODUCER side is gone as of the non-extension cleanup: `output_targets_pass` no longer
  declares it, the runtime no longer adopts/publishes it and the shader slot constant is removed - which saves
  one depth image per swapchain image (8 x render extent, ~33 MB at 1080x960) plus its descriptor writes. The
  `resource_id` entry itself stays (removing an enum value would renumber every id after it); the v1 software
  raster's resource pass can take that renumber together with its own private depth. HEAP SLOTS 727..734
  remain reserved for it.
- The draw-item table (`visibility_items`) is a per-frame-slot buffer the raster lane's recorder fills and the
  compute resolve reads; its host-visible lane is one 112-byte record per visible item.

The registry gives each physical G-buffer attachment its own allocation id because their formats differ.
`gbuffer_targets` remains the three-element family used by existing pass IO. `scene_input` is the physical
TAA input allocation; `scene_color` remains the per-frame alias selecting that image when TAA runs and
HDR otherwise. The new physical ids identify allocation requests and do not replace existing IO routes.

Application producers can provide individual families. The compatibility producer describes only the
missing ones. Runtime validates the complete selected set (type, lifetime, scope, extent, format, layers,
mips, instance count and required usage) before changing any borrowed image/view or heap binding.
Replacement drains in-flight work, adopts the distributed views and republishes existing sampled slots.
Snapshots retain former generations until frame completion. Existing clears and barriers still own
image initialization; `begin_frame` does not blindly clear images.

Frame preparation reports allocation/admission failure before legacy publication and acquisition, including
the first frame; a subsequent frame can retry. Constructor setup prepares output targets only. Configuration-dependent shadow, cluster and constant
resources retain their first-frame lazy preparation. A size change creates new output generations;
a same-size rebuild can reuse matching allocations. Generation changes conservatively reset existing
layout/history/Gaussian frame state after the drain. Other image families still rebuild through the former path, with the existing swapchain recreation ordering.

## TAA history and post targets

The output producer also declares LDR and four physical Bloom levels with `output_size` lifetime.
Bloom level L uses `max(1, render_extent >> (L + 1))` on each dimension. The four allocation ids retain
the existing `bloom` family routes and heap stride. Runtime reads the allocated images' extents through
the same helper for both owned and borrowed images; it does not repeat the downsample formula.

TAA history has `history` lifetime, output-image scope and sampled/transfer-destination usage. It is
never requested for per-frame clearing. `taa_pass` also describes the identical history requirement,
so a concrete TAA pass has an application-chain producer even if other outputs come from compatibility
providers. Compatible duplicate requirements resolve to one generation.

TAA retains its bound snapshot and compares the history generation when rebinding. An unchanged
generation preserves history validity and `wrote_history`; replacement invalidates both. Existing
swapchain recreation and TAA off-to-on resets still apply. The resolve still copies HDR into history,
then returns it to shader-read use; ownership migration adds no image clear or copy.

LDR/history/Bloom replacement follows the same complete admission, idle wait, borrowed bridge and
submitted-snapshot rules as the base targets. Admission validates the expected lifetime and derived
extent of each family. Runtime no longer allocates or destroys these images in the legacy rebuild loop.

## Ray-traced visibility

The ray-traced shadow pass declares its own output image with the same request as the shared producer:
full-resolution R16F, output-size lifetime and **frame-slot** scope, sampled/storage usage. Runtime validates the
expected scope and instance count per family, then adopts the generation using its actual object count, publishes
both the sampled and the storage heap descriptions, and keeps a sampled view for attachment / resource-table
routing. Submitted snapshots retain retired images until the relevant slot completes.

The MegaLights trace/resolve/history families this section also described were removed from the tree on
2026-10-17 (the render graph gate review records it); the inventory rows for them are historical.

## Shared deformation storage

`scene_deformation_pass` describes motion transforms, current and previous skin matrices, and morph
data as four coherent storage buffers with device addresses. All use frame-slot scope/lifetime and
zero-on-create; none uses zero-on-frame. `skin_matrices_previous` is an appended engine resource id,
preserving all earlier ids and the RHI ABI number. The frame resource table now also publishes that buffer.

Constructor preparation provides the buffers before animation setup or instanced motion writes.
The runtime borrows buffers/mappings and retains each family's owning snapshot. Application and
compatibility chains can independently provide missing families. Admission checks capacity, mapping,
scope, lifetime and reset policy before acquisition or changing borrowed handles/heap bindings.

On replacement, the existing idle wait precedes a per-slot copy from the old mapped buffer into the
new generation; old snapshots remain alive throughout the copy. This preserves setup morph deltas,
partial skin uploads, instanced motion transforms and previous-skin bytes. CPU chronological history
is preserved separately. No full-buffer copy or reset occurs on normal reuse; a morph replacement
does copy up to 48 MiB per slot. Raw morph scratch is borrowed for immediate writes and must be
obtained again after reconciliation; the animation controller already obtains it per operation.

Existing setter/recording writes and their completed-slot/setup timing remain unchanged. This stage
changes allocation ownership and publication, not the deformation algorithm or its GPU barriers.
The animated debug capture separately exposed and fixed an existing HDR attachment ordering error.

## Shared scene tables

`scene_tables_pass` declares nine global buffers: materials, instance transforms, toon colour/lanes/rig,
meshlets, mesh indirect commands, statistics and culled meshlets. Material/instance schema scope now
matches the actual global binding. Seven ids are appended without renumbering prior ids or changing the RHI ABI.
The last three buffers retain internal frame regions and their original offsets; indirect commands
retain indirect usage and are not placed on the descriptor heap.

Constructor collection precedes default-material/toon initialization. Runtime borrows handles/mappings;
admission checks each whole allocation's capacity, scope, lifetime, coherent mapping, flags and reset
policy before adoption. Neutral toon values and material0 registration stay in the original setup code.
Replacement drains GPU work then preserves full mapped contents, including imported tables and cumulative
statistics. Normal frames perform no table reset/copy. All nine allocations are published at instance0
in the frame resource table; submitted snapshots retain retired objects.

## Gaussian GPU frame storage

Each completed Gaussian GPU frame slot owns a resource-only subchain describing ten dynamic buffers:
instances, ranges, keys, values, references, sort scratch, status, indirect draw, parameters and projection
cache. All are device-wide allocations within that slot's subchain, grouped under frame_slot lifetime.
The parent Gaussian pass receives the retained frame snapshot; ordinary reuse does not reconcile.

Registry admission runs before candidate publication and pass binding, including cached-generation
reconciliation. Gaussian checks actual capacities/budget, addresses and coherent mappings there;
rejection preserves active resources. Production supplies an owning device lease from first creation.
Legacy reference-only reuse cannot acquire an owning lease later; create a fresh frame for that transition.
The subchain drains before sorter, asset and device leases are released.

CPU raster slots use the same resource-only subchain for their two coherent instance/reference buffers.
CPU results are staged before resource admission and publication; rejected growth preserves the old frame.
Two retained CPU reference/owner workspaces alternate after warmup, preserving capacity reuse. Runtime
supplies its owning device root from initial creation; the legacy lease contract matches GPU frames.

## Initial buffer contents

A buffer request may carry owned immutable initial_data bytes and an independent initialization_key.
The registry exposes those bytes only during synchronous creation, clears the byte owner from generation
metadata, and retains the key/count for compatibility. After successful upload the producer can release
its byte owner while continuing to describe the initialized resource. Same-key reuse needs no source;
recreation without a source returns not_ready. New content uses a new key. Initial contents cannot be
combined with zero-on-create/frame reset policies.

Static Gaussian geometry and SH buffers use scene-lifetime resource subchains with the existing staging
upload intent. Shared packed data aliases are released after upload; production transfers packed vectors
without an additional copy and supplies the owning device root through runtime create_gaussian_asset.
The legacy const-reference convenience overload temporarily copies its validated packed input.

## TLAS and instance tables

Completed TLAS slots declare a top-level AS and coherent hit-shading instance table in a resource-only
subchain. Growth validates and copies the current record prefix before committing the pair, preserving
the old frame on failure. Normal reuse skips reconciliation. Production forwards core_owner; retained
snapshots hold devices, and slot chains drain before releasing allocations. The typed registry AS path
accepts top-level descriptions and bottom-level descriptions with an owned geometry vector.

TLAS publication uses the current frame slot and a typed AS handle. Allocation persists across empty
or failed frames, but traversal readiness is cleared before recording and enabled only by a successful
nonempty build. Early skin/refit failures leave no stale ready handle.

Nonempty BLAS entries have scene-lifetime subchains and borrowed handles. Geometry vectors are retained
in declarations; only synchronous creation sees their temporary RHI pointer. Optional shared dependency
leases stay alive until after the AS releases. Entry movement and ordinary refit add no idle wait;
final destruction drains each entry. Mask/skin output generation leases are connected to BLAS
dependencies.

Mask/skin bake outputs use scene-lifetime GPU-only buffer subchains. Resource preparation completes
before mask recording starts; partial skin/AS recording failures retain resources and keep traversal
disabled. Initial skin failure also updates runtime's bake flag.

Opacity micromaps use typed scene subchains and are retained by BLAS generation dependencies.
Their requests own immutable upload vectors only through synchronous creation, retaining a separate
content key afterward. Raw RHI spans are never stored; recreation without its upload source refuses.

Production scene geometry uploads also declare vertex/index buffer pairs through scene subchains.
Primitive views carry generation leases, and BLAS retains both source-buffer leases. Legacy geometry
assignments adopt one factory reference, the contract's `rhi::unique_object<rhi::buffer>`, into a
shared holder (`geometry_buffer::operator=`); their external device obligation remains.

## Shared images, samplers and ownership coverage

Shared texture/IBL/furnace/LUT images and six samplers use typed resource-only subchains. Runtime
holds aliasing `rhi::shared_object<T>` handles whose control blocks name the subchain's own
`std::shared_ptr`; those aliases own the subchains and their device roots.
Image upload sources use the immutable initial-data/key protocol and are released after creation.
Only whole views are requested for shared sampled images. Shader binding table uploads also use
managed coherent/addressable buffers through the pass upload hook.

IBL stages a complete replacement, waits for prior readers, publishes all heap slots and only then
replaces owners. Write refusal rolls back the completed slots; failed rollback retains candidates and
blocks acquisition until successful replacement. Display/FGD LUTs publish before replacing owners.
Setters must be called at frame boundaries, without concurrent CPU recording.

Production pass and shared-resource ownership migration is complete. Presentation images stay
backend-owned borrows, diagnostic/probe allocations are explicitly separate, and legacy custom imports
retain their external device/scene obligations. Heap publication, address access and manual barriers
remain their existing access channels; lifetime labels do not constitute an automatic render graph.
See [the pass resource inventory](render_resource_inventory.md); the final audit record is not
distributed with the repository.

## Validation

Portable regressions cover shared producer/consumer snapshots, growth/reuse, description conflicts,
partial creation failure, history preservation, frame/output instance counts, device-root retention,
frame-slot retirement, failure before acquisition, and replacement of an application chain. A real shadow
pass also exercises cascade growth/shrink and verifies that sampled heap identities follow the new images.
The shadow, clustered-light, scene-constant, output-image, TAA/LDR/Bloom, MegaLights/RT image, camera and
shared-deformation acceptance records were maintainer working material and are not distributed with this
repository; their conclusions are summarized above.
