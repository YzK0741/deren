# Render resource management: proposed direction

Status: proposed design for review; implementation has not started.

## Objective and constraints

Following the shadow fixes, make render resource ownership, descriptor publication, slot reuse and
retirement explicit. Preserve the current image, frame depth, optional async presentation and
windowless mode. Begin with the raster shadow family. Other render targets, scene assets, automatic
barriers and aliasing are later migrations, not first-phase requirements.

The user's aim is to continue resource-management work after the shadow audit/fixes. The assumed
priority is correctness and easier maintenance, followed by avoiding unnecessary device-wide waits.
No additional buffering or async compute is part of this proposal.

## Existing foundation

- `deren.engine.render_resource` already describes resource identity, scope, lifetime and binding
  read/write access. Extend this vocabulary when needed instead of introducing a competing schema.
- Shadow images are per frame slot, selected in shaders by `heap_frame_slot`. HDR/G-buffer/history
  families are generally per output image. These two indexing domains must stay separate, including
  when offscreen mode happens to have two output images matching the two slots.
- Runtime owns shadow images, array/layer views, allocated layer count, content version and per-slot
  rendered version/geometry hash in separate fields.
- `frame_walker::wait_and_acquire()` waits the current slot's timeline before returning its frame
  identity. This supplies a retirement boundary for resources used only by these frame submissions.
- Vulkan `pending_submission` already retains command buffers and texture groups until fence/timeline
  completion. It is not a general owner for every render attachment and heap descriptor target.
- Swapchain and cascade rebuilds use global waits. Scene-resource preparation currently runs before
  slot wait/acquire, so replacing resources there cannot automatically be treated as a safe rebind.

## Alternatives

1. Group the existing fields only. Lowest risk and useful cleanup, but leaves replacement policy and
   lifetime implicit. This is a useful first migration step.
2. Add resource generations and per-slot adoption/retention to the existing engine descriptions.
   Recommended direction: group first, then make replacement safe and local to the slot. It can use
   the current frame completion boundary for the shadow family without changing the RHI ABI.
3. Introduce a complete render graph now, including dependency compilation, transient aliasing and
   automatic barriers. Broad reach, but too much change to validate at once; defer until ownership
   and resource-state facts have a reliable home.

## Shadow resource generation

A generation owns an immutable description (generation id, map size, allocated layers, image format)
and the complete per-slot image/view bundles. Views and images are replaced as one unit; views are
released before their parent image. Creation succeeds before a replacement becomes visible.

Content validity is separate from allocation identity: changing the sun/camera or caster geometry
invalidates cached contents without creating a new image generation. Per-slot state records the
last successful generation/content revision/geometry signature and image usage. A skipped shadow pass
retains its valid sampling state; it does not invent an attachment write or force an undefined-layout
transition merely because the frame number changed.

Each prepared frame captures the generation and the matching light parameters. Shadow recording,
heap publication and PBR lighting consume that frame's selection. They must not query mutable global
resource settings halfway through recording. CPU references survive until every submission that uses
that generation has completed.

## Safe adoption and retirement

For the shadow family, allocation can happen before the slot wait; descriptor mutation cannot.
After the walker has actually waited the slot, and before recording the next frame:

1. Collect the previous submission's retained generation for that slot.
2. Adopt a fully created replacement if that slot needs one.
3. Publish only the idle slot's heap entry and prepare matching light-buffer parameters.
4. Reset generation-dependent content/command state and record using the captured selection.
5. Retain that selection for the submitted frame. Other in-flight slots retain their old selections.

The old generation is released after the final slot/reference stops using it, not after an arbitrary
number of CPU frames and not when a presentation future resolves. Host presentation completion is
not GPU rendering completion. A skipped/deferred acquire must not accidentally publish new descriptors;
slot-completion accounting must explicitly follow the walker's actual wait semantics.

Keep existing global waits as the initial fallback during field grouping. Remove only the shadow
replacement wait after per-slot retention and heap publication have tests. Swapchain/borrowed output
images keep their current rebuild synchronization. A resource used by standalone uploads or other
queues cannot use this frame-only retirement rule; those families need their own completion accounting
before migration. Keep shadow-map size startup-only in the first step; do not quietly change that API.

## Later state and barrier work

After ownership migration, add instance/subresource usage state alongside the existing descriptions.
Start by validating manually recorded transitions against that state. Then derive the shadow depth
write -> PBR sampled read transition from scheduled uses, preserving cached-pass skips and active/spare
layers. Descriptor binding declarations alone do not prove a pass actually ran or wrote a resource;
resolved schedule uses must contain that fact. Do not enable aliasing or queue scheduling in this step.

## Acceptance and migration boundary

The first implementation step groups the shadow image/view/capacity/cache state and routes existing
creation, binding, reuse and teardown through it, while retaining current waits and barriers. It should
produce byte-identical captures against a newly frozen current shadow-fixed runtime. Old pre-shadow-fix
references are not the reference for this refactor.

The next step tests replacement with both slots in flight: the idle slot changes generation while the
other keeps its old image and descriptor; old views/images survive until GPU completion; failed creation
leaves the prior generation usable; repeated cascade changes do not leak retired generations; disabling
shadows preserves legal descriptor and cached-content states. Validate destruction order with a CPU probe
and real GPU synchronization validation. Include fixed/orbiting PBR, transparent geometry, window
restore, windowless capture and async-presentation modes. No image-quality change is intended.

Review this design before producing the separate implementation plan. No product code is changed by
this document.
