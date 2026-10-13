\page managed_resources Passchain-managed resources

Passes can describe dynamic image and buffer requirements. The passchain collects all descriptions,
merges compatible producers, creates complete resource generations, and distributes a shared snapshot.
Production migrations now cover raster shadow images, clustered-light count/index buffers and light/head
constant buffers, G-buffer attachments, HDR/scene-input, TAA history, LDR, Bloom, MegaLights and RT visibility images;
existing GPU bindings and barriers remain explicit.

## Ownership and lifetime

The registry owns `resource_id -> shared_ptr<const generation>` and groups ids in
`map<lifetime, vector<resource_id>>`. A generation owns its device root, allocations and image views;
views are destroyed before their images, and the device is released last. A pass holds a snapshot and
uses its objects. The runtime retains submitted snapshots until the corresponding frame-slot wait
succeeds. Destroying a chain drains its device before releasing its owned passes and resources.

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
shadow, cluster and/or light/head resources. The application chain remains independently collected and initialized.
Successful submissions retain both application and compatibility snapshots. Chain handover selects the
producers anew; cached compatibility pass objects are owned by the runtime's compatibility chain.

Native tests with 32 lights exposed two older bugs now fixed: the cluster dispatch omitted its heap
frame/image push indices, and shared shading read the host's floating light-count lane as uint bits.
The dispatch now pushes those indices first, and shading reads the numeric float with a bounded count.
Neither fix changes the host UBO layout or the RHI ABI.

## Scene constant migration

`scene_constants_pass` describes the light block (8608 bytes) and head block (64 bytes), each with
frame-slot scope and lifetime. They use coherent uniform memory with storage and device-address flags;
the existing shader layouts and heap slots remain unchanged. Both initialize once and never request
`zero_on_frame`. Fresh light blocks receive `light_ubo{}`; fresh head blocks receive `head_ubo{}`,
including its nonzero default axes. Reused generations retain their contents.

CPU parameter calculation stays in runtime. After a successful slot wait, runtime calls
`pass_chain::write_frame_buffer(completed_slot, id, bytes)` to upload that slot's current values.
The registry checks resource identity, frame scope, slot bounds, byte capacity and coherent mapping.
The caller supplies GPU completion; the upload method does not wait. Initial priming of new generations
occurs before their first use, or after the existing idle wait during replacement.

The application and compatibility chain can supply light/head independently. Admission validates all
selected descriptions before changing borrowed handles or heap bindings and rejects per-frame clearing
for these constants. Replacing providers preserves submitted snapshots until their slot completes.
Arbitrary-time setters modify CPU state only; they cannot overwrite a constant block read by the GPU.

## Output target migration

`output_targets_pass` declares the base output image families at `resource_context::render_extent`: HDR,
G-buffer albedo/normal/material, velocity, physical scene input for TAA, and depth. These requests have
`output_size` lifetime and `per_swapchain_image` scope, with one instance per host-supplied output count.
The existing runtime still supplies the RHI output-image bound. Formats and attachment/sampled/copy
usage match the former allocation path; HDR retains transfer-source usage for history and readback.

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

## MegaLights and ray-traced visibility

The three ray-related passes declare their own output images, using the same requests as the shared
producer. MegaLights trace/resolve/history use output-image scope and half resolution with a 1x1 floor.
Trace and resolve require sampled/storage usage; resolve also needs transfer-source usage for its copy.
History uses `history` lifetime and sampled/transfer-destination usage. RT visibility is full-resolution
R16F, has output-size lifetime and **frame-slot** scope, with sampled/storage usage.

Runtime validates the expected scope and instance count per family, then adopts each generation using
its actual object count. Storage images receive both sampled and storage heap descriptions at the existing
slots. A sampled image view is retained for attachments/resource-table routing; storage heap publication
uses the storage role. The former runtime allocation loops are removed; only the furnace cube remains in
that legacy render-target rebuild function.

Each concrete pass holds its bound snapshot. MegaLights trace restarts its sequence when its trace
generation changes; temporal resets its resolved/history state on resolve/history replacement. Runtime
invalidates the MegaLights history-valid array when any of its three generations changes. Ordinary
reconciliation reuses generations and preserves state. Existing dispatches, copies and barriers remain
unchanged, and submitted snapshots retain retired images until the relevant slot completes.

MegaLights history becomes valid only when the temporal pass reports that it copied a new history for
the current output image. The shader guards history sampling with the validity bit in `extents.w`;
the push layout and ABI are unchanged. Toggle changes and generation replacement invalidate history.
Frames that skip MegaLights (including G-buffer debug and unlit frames), failed recording, and failed
submission invalidate the current image. A presentation failure after successful submission does not
discard the GPU work. See the [temporal correctness review](reviews/2026-10-14-megalights-history-fix.md).

## Remaining migrations

Other pass allocations, acceleration structures, imported presentation images, sampler ownership,
automatic descriptor publication, and automatic barriers retain their existing paths. This stage establishes
ownership and completion retention; it does not turn the lifetime label into a render graph or remove waits.
See [the pass resource inventory](render_resource_inventory.md) for the remaining access channels.

## Validation

Portable regressions cover shared producer/consumer snapshots, growth/reuse, description conflicts,
partial creation failure, history preservation, frame/output instance counts, device-root retention,
frame-slot retirement, failure before acquisition, and replacement of an application chain. A real shadow
pass also exercises cascade growth/shrink and verifies that sampled heap identities follow the new images.
Shadow evidence is recorded in `docs/reviews/2026-10-13-managed-resources.md`; clustered-light
results are recorded in `docs/reviews/2026-10-13-cluster-managed-resources.md`.

Scene-constant upload, slot-isolation and chain-handover evidence is recorded in
`docs/reviews/2026-10-13-scene-constants.md`.

Output-image migration evidence is recorded in `docs/reviews/2026-10-13-output-resources.md`.

TAA/LDR/Bloom migration evidence is recorded in `docs/reviews/2026-10-13-post-resources.md`.

MegaLights/RT image migration evidence is recorded in `docs/reviews/2026-10-13-ray-resources.md`.
