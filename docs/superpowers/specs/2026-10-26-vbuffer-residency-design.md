# Same-frame visibility-driven texture residency

Status: approved and implemented (see execution ledger and acceptance review). The user selected same-frame exact loading over delayed feedback.
This document's checkpoint API and initial eligibility restrictions are implementation proposals.

## Goal and scope

Keep scene geometry resident. Use the current frame's completed visibility buffer to discover which
opaque materials need shading, upload only their complete texture images, then resolve that same frame.
Save device memory while accepting CPU/GPU synchronization and upload stalls. Preserve the existing
renderer when the feature is disabled. No virtual textures, mip/page streaming, geometry streaming,
CPU ray probing, asynchronous decode, upload-policy change or frame-time improvement claim.

Existing code: runtime.constructor.cppm::register_material uploads scene images eagerly and writes
global fixed slots. core.lazy_resources.cpp::prepare_lazy_descriptors demands every registered lazy
fixed binding, so registering all lazy images would defeat selectivity. Logical views/fixed slots and
recorded commands also prevent unload. Frame submission currently completes/presents a whole frame;
independent command_submission explicitly excludes an acquired frame's borrowed recorder.

## Approaches

1. Selected: same-frame visibility feedback, host wait, exact whole-image demand, then shading. No
   missing-texture frame for newly revealed objects; incurs a synchronization bubble each frame.
2. Deferred feedback: previous frames guide uploads with placeholders for misses. Better overlap,
   but the user chose current-frame correctness over this tradeoff.

For the selected route, split recording at an explicit frame checkpoint. A duplicate standalone probe
would avoid frame API work but repeat visibility raster and risk camera/HZB divergence; do not use it.
Do not submit a frame-owned recorder through the independent submission extension.

## First supported profile

An opt-in renderer setting (proposed name: visibility_texture_streaming, default false) requires the
existing eligible static visibility-buffer deferred path and a frame-checkpoint capability. Restrict
the initial scene profile to ordinary PBR materials with supported sampled color textures. Unsupported
toon/custom, transparent, deforming, instanced or ray-tracing texture consumers select the eager path
at scene registration, with an explicit reason. The builder must enumerate actual consumer paths to
enforce this predicate; never silently stream a texture with an untracked consumer.

Pin the alpha-source images of MASK materials: raster and shadow alpha tests need them before feedback.
Pin renderer defaults/environment/LUTs and any other textures needed by admitted auxiliary passes.
An image shared by streamed and pinned materials is pinned. If a later configuration would add an
unsupported consumer, finish existing work and make every scene image resident before enabling it.
If a frame falls back to the ordinary G-buffer path, make all its needed material images resident before
recording it; reverting to full scene residency is an acceptable conservative first fallback.

HZB and DAG retain their existing correctness/recovery policy. Feedback runs after hardware/software
visibility and recovery, before resolve. Currently the hybrid winner is chosen inside resolve, not
stored in one merged ID image: feedback must read hardware ID/depth plus software ID/depth and use
the same choose_visibility_candidate helper and frozen hybrid flag as resolve. Do not move the later
depth merge ahead of resolve. It must not use intermediate HZB candidates or the hardware-only prefix.
Invalid/unresolved records yield a reported failure or a
conservative full-resident fallback, never a silently incomplete demand set.

## Data and ownership

Add an engine-level scene texture residency owner, separate from RHI. Stable texture IDs and material
indices retain their existing shader meaning. Each deduplicated texture record keeps immutable metadata,
shared const utility::vessel::vector<uint8_t> source, lazy_image, pinned/wanted/published/physically-ready
state, and byte counters. Descriptor removal and successful physical reclamation are distinct events.
The upload bytes must have owning storage after register_material returns; current borrowed tex.data
cannot be captured as a span. Share an existing compatible owner if available; otherwise copy once
at registration. Key deduplication by content plus format/mip/extent semantics, preserving current rules.

Maintain CPU material-to-texture edges as material registration assigns stable IDs. GPU feedback is a
material bitset indexed by the existing material table, not frame-local item IDs carried across frames.
For current capacity16384 it is2048 bytes, plus a small status/header. Empty pixels contribute nothing;
valid IDs decode through this frame's frozen draw_item table to material_index, with bounds checks.
Use a full-resolution compute scan and atomicOr; no downsampling that misses tiny visible surfaces.
Clear bits/status every frame, publish compute writes, copy to coherent readback and publish host reads.

## Frame checkpoint (proposed optional RHI extension, ABI58)

Expose one synchronous operation, provisionally checkpoint_and_wait(), on an optional frame extension.
It accepts only the current active frame, outside rendering/query scopes that cannot cross submissions.
It flushes deferred operations, ends/submits the prefix and waits for that prefix's graphics completion,
then opens a fresh continuation recording for the same acquired frame and stable borrowed recorder face.
It must not reacquire, present, advance the ring, or signal final-frame completion/present semaphores.
After the prefix completes, release/reset its recording leases before attempting residency reclamation.

The backend consumes the acquire semaphore exactly once (in whichever submission first needs it),
and only final submit signals presentation and the frame-slot completion point. Prefix resources stay
alive through completion. Reinitialize recorder binding state; suffix passes explicitly bind pipelines,
descriptor space, push data and groups. Keep timing/query ranges valid or reject unsupported scope
combinations before submission. The normal no-checkpoint frame protocol is unchanged.

Crucially, a successful checkpoint is also a safe descriptor-publication boundary for this mode:
all earlier graphics submissions capable of reading the global scene texture slots are complete.
All consumers run on that graphics queue and CPU recording/publication is serialized. A future
multi-queue consumer requires explicit synchronization and is outside this profile. This guarantee
must be tested, not inferred merely from current-slot acquire. Do not call whole-device wait_idle.

On failure before submission, report the error and do not continue shading. On submit/wait failure,
preserve the existing fatal/device-loss ownership policy; do not reset/free potentially pending work.
Document how an acquired frame is aborted and drained in the backend before claiming this API complete.
The builder chooses native completion storage and exact naming, with explicit contract/device tests.

## Graph and per-frame order

Freeze camera, draw items, material mapping and scene generation for both segments.

1. Record the normal graph prefix: auxiliary stages and all visibility raster/recovery/software work.
   Its descriptors expose pinned images and already resident images; nonresident scene slots contain
   valid ordinary fallback images, never unmaterialized lazy resources.
2. Compute material feedback from the final visibility candidate selection, copy/status publication,
   end prefix at checkpoint.
3. Wait; validate feedback and generation; union visible-material edges with pinned texture IDs.
4. At the safe publication boundary, replace undesired resident slots with ordinary fallback images,
   drop owned lazy views/groups that cache physical handles, and attempt unload. not_ready defers the
   reclaim (for example an old reusable command still retains it); count this separately. Never force it.
   Keep its slot on fallback even while physical reclamation is deferred; do not republish it merely
   because state()==ready. Retry unload at later safe boundaries, after frame-slot recording recycling.
5. For missing demanded textures, publish only their lazy bindings and perform an explicit descriptor
   bind/materialization before suffix use. Upload finishes before shading under existing lazy semantics.
   Restore valid fallback on a refused upload and fail the current frame explicitly; do not silently
   substitute a placeholder for a required visible texture in this exact mode.
6. Record resolve, lighting and post suffix against the prefix's preserved ID/depth outputs; final
   submit/present proceeds once. Checkpoint completion alone never presents a partial frame.

Partition the composed graph into two compiled segments with explicit exported/imported resource
states and retained resource owners. No callback waits in the middle of prepared_graph::record and
no arbitrary execution-order cutoff. Graph barriers must describe feedback and cross-segment handoff.
History advancement/HZB state publication happen once per full frame, not once per segment.

Start with exact current demand and no artificial VRAM budget or LRU. Reclaim may lag due to live
recordings; report actual successful unload separately from desired nonresidency. Success releases
allocation owners, not necessarily VMA blocks. CPU source memory remains resident by design.

## Evidence and acceptance

Unit tests: ID/material bitset decode, invalid bounds/status, dedup and shared-texture pinning, demand
set unions, unload refusal/retry, scene-generation changes and eager fallback eligibility.
GPU tests: two segments preserve ID/depth/content; prefix waits prior readers before slot mutation;
acquire/present happen once; rebind after checkpoint; deterministic masked occlusion and tiny visible
surface feedback; empty frame; newly revealed texture correct in that same output frame.
Capture correctness against eager rendering at fixed camera, plus camera jumps and repeated turnarounds.
Exercise both frame slots, resize and failure paths; validation layers must remain clean.

Measure Bistro fixed near/far views and a camera path, with eager and selective modes in the same build.
Report GPU rendering time, whole-frame CPU wall time, checkpoint wait, upload bytes/time, logical
resident texture bytes/count, pinned bytes, deferred unload count and allocator/device-memory counters
where available. Geometry is unchanged. Do not equate source bytes with device allocation or claim
memory savings solely from a reduced requested set. Mode-off regression and existing tests must pass.

Suggested implementation sequence: checkpoint contract/probe; residency ownership without feedback;
feedback compute/readback; graph segmentation and same-frame integration; unload/reload cycle; captures
and measurements. Every stage should leave the default renderer usable.
