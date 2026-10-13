# Unified resource access: detailed proposed design

Status: written proposal for review, not implemented. Refines the previously discussed approach and
uses the current [pass inventory](../../render_resource_inventory.md). Scope is the engine resource
layer and its adapters; the initial migration preserves the existing RHI ABI and shader heap layout.

## 1. Objective and non-goals

Unify resource identity, instance selection, declared GPU use and completion-dependent ownership.
Keep descriptor heaps, buffer device addresses, attachment views and acceleration-structure mapping
as supported access mechanisms. A pass describes its need; the engine resolves the right instance,
retains its allocation and produces the existing GPU-facing representation.

The first migration must preserve pixels, the two frame slots, optional async presentation, windowed
resize and windowless captures. It is not a complete render graph implementation. Memory aliasing,
multiple queues, automatic asynchronous uploads, shader slot redesign and temporal-history policy
changes are outside the initial work. Shadow map size remains startup-only initially.

## 2. Four distinct identities

| Identity | Meaning | When it changes |
| --- | --- | --- |
| Resource family | Existing semantic id, e.g. shadow_map, HDR, camera_ubo | When a new semantic family is introduced |
| Logical handle | Registry index plus incarnation; typed image/buffer/AS handle | When a logical entry is deleted/reused; NOT ordinary reallocation |
| Allocation generation | Immutable allocation description and owned instances, with monotonic generation id | When resolution, format, capacity or owned objects change |
| Concrete use | Allocation instance plus layer/mip or byte range and access role | Each scheduled use in a particular frame/pass |

A stale logical handle is rejected through incarnation validation. The registry keeps the active
allocation generation; frames retain whichever generation they resolved. Content revision is separate:
new camera/skin/caster data invalidates a shadow cache without reallocating its image.

Aliases such as scene_color must resolve to the canonical underlying allocation instance before
state/hazard tracking. Two ids, two views or sampled/storage descriptors naming the same image do not
create two independent resource states. No transient memory aliasing is introduced.

Illustrative engine types (not new exported RHI interfaces):

```cpp
struct image_handle { uint32_t index, incarnation; };
struct buffer_handle { uint32_t index, incarnation; };
struct as_handle { uint32_t index, incarnation; };
struct frame_identity { frame_slot slot; output_image_index image; uint64_t output_generation; };
struct image_range { aspect_mask aspects; uint32_t first_mip, mip_count, first_layer, layer_count; };
struct buffer_range { uint64_t offset, size; };
```

Use strong types for frame slot and output-image index at this layer. Conversion to shader uint lanes
happens at the binding adapter. The existing RHI image range currently has no aspect field; preserve
its existing semantics initially and reject combinations requiring an unsupported precise barrier,
rather than silently claiming the current contract can express them.

## 3. Ownership and instance domains

The registry owns generation records. Each owned generation owns its RHI images/buffers/AS and their
cached views using the existing object_manager ownership contract. Frames retain generation records,
not bare native objects. The backend core must outlive the registry and all submitted frame leases.
Views are released before parent images; AS dependencies and descriptor/view dependencies must also
remain retained. Shared references are allocated once per generation and deduplicated per frame,
not allocated per draw or per texture sample.

An allocation description has one explicit indexing domain:

- per_frame_slot: shadow maps, camera/light/skin/morph/previous-transform buffers, cluster lists,
  per-slot TLAS and RT visibility;
- per_output_image: G-buffer/HDR/LDR/bloom and current temporal image storage;
- single: scene textures, single material/instance descriptors, IBL and shared lookups.

A single resource can still be mutable. Publication and content-update rules are separate from scope.
The current schema labels material/instance tables and TLAS differently from their actual bindings;
reconcile those entries with host creation and shader indexing before they drive instance selection.
Do not reinterpret temporal history as previous chronological frame merely from its storage domain.

Imported output images have a different policy: the backend owns image destruction. The registry
records borrowed images and owns only any engine-created views. It pins a valid output generation and
cannot promise to extend an imported image's life with a shared pointer alone. Existing swapchain
rebuild wait/drain behavior remains until an explicit backend import-lifetime protocol exists.
Windowless output is still selected by output-image identity; equal slot/image counts are incidental.

## 4. Access declarations: role and representation

Each concrete use records resource/range, pipeline stage class, access role and requested representation.
Access roles include sampled read, storage read/write/read-write, uniform/structured read, color/depth
attachment read/write, transfer source/destination, indirect-argument read, AS build read/write and AS
traversal read. Device address is a representation, not an access role: a device-addressed buffer can
be read by a mesh shader, written by compute, or read by an AS build.

Initial normalization uses existing rhi::image_use and rhi::buffer_use roles and their supported backend
translations. Stage metadata improves engine-side diagnosis; it does not pretend the current role-only
RHI barriers accept arbitrary stage/access masks. Missing role pairs retain an explicit manual adapter
or fail migration validation until the contract is deliberately extended.

Attachment declaration includes load/store behavior, blending and depth-write state. Loading/blending
color requires reading existing attachment data as well as writing it. Depth test with writes disabled
is a depth read. Discarding old contents permits an undefined old layout, but does NOT remove ordering
against earlier outstanding readers/writers. Store-discard marks subsequent contents invalid.

Declare copies and indirect fetches, even when shader source does not mention those operands. TAA and
MegaLights history updates are actual transfer tasks. Declare BLAS/TLAS build inputs independently of
ray traversal descriptors. Conditional passes contribute uses only when they will actually execute;
registered shader bindings are potential uses, not proof of a write.

## 5. Two-phase pass API

Separate planning from recording. During planning a pass requests resources and declares all uses;
resolution/validation constructs a frozen pass view. Recording uses that view without changing the
registry, heap slots, resource generation or state ledger.

The following is illustrative, not an implemented header:

```cpp
// Declaration / plan phase: ranges are required or normalized to explicit whole-resource ranges.
auto depth = pass.depth_target(shadows, cascade_layer, load_op::clear, store_op::store);
auto geometry = pass.read(vertices, whole_buffer, mesh_shader, device_address);
auto shade_shadow = lighting.sampled_depth(shadows, active_layers, fragment_shader);

// After frame resolution, during recording:
auto vertex_address = resolved.buffer(geometry).device_address();
auto depth_view = resolved.image(depth).attachment_view();
auto shadow_slot = resolved.image(shade_shadow).sampled_slot();
```

The view token encodes the declared representation and cannot be resolved as an unrelated one. Asking
for a storage image from a sampled-only declaration, or an address from a buffer without device-address
support, returns an explicit error. Typed representations still contain small GPU values for push data;
resource retention comes from the resolved frame, never from those integers.

Compatibility adapters produce the existing resolved_io/resource_table pointers, fixed heap indices,
address halves, rendering attachment views and RT mapping data. Preserve shader push layouts and fixed
slot constants during the first migration. Converted passes cannot obtain an undeclared raw resource
through this adapter. Unconverted passes stay in an explicit legacy path with current synchronization,
instead of being declared fully tracked because they happen to share a table.

## 6. Frame protocol and GPU completion

For the first shadow migration, the frame lease is engine-owned and bounded by existing slot waits:

1. Apply desired configuration and create a complete prospective generation without publishing it.
2. Open the frame with frame_walker::wait_and_acquire(). Only a successful open supplies a usable
   slot/image identity and proves the slot wait succeeded.
3. Collect that slot's previous submitted lease, adopt a pending replacement for this idle slot, and
   publish only descriptor entries it is safe to update. Freeze matching camera/light/shadow metadata.
4. Plan active uses, resolve views and prepare/validate transitions centrally.
5. Record primary/secondary commands; worker tasks receive immutable resolved views. They do not
   concurrently mutate ownership, descriptors, state or cache-validity records.
6. Submit. On success, commit the scheduled state/content changes and transfer retained generations
   into this slot's submitted lease. On refusal, abort the tentative state changes and keep ownership
   until command reset/discard and any actual GPU submission status are known.
7. Present through the existing mode. Reclaim this lease only after a later successful GPU slot wait
   or an explicit successful full drain, not when a presentation future resolves.

A failed/out-of-date open has no valid frame identity in the current API, even if its wait half happened
to finish. Conservatively keep leases until a later successful open/drain; do not infer completion from
a zero frame struct. This initial policy avoids changing the by-value frame_open_info ABI. If acquire
fails indefinitely, the existing recreate/shutdown drain remains the cleanup path.

One lease per slot is sufficient only while that slot has at most one outstanding frame submission and
no additional independent GPU work uses its resources. Upload/one-shot/multi-queue families are not
migrated to this rule until completion tokens account for their actual submissions. Backend
pending_submission retention of commands/texture groups remains; do not weaken it.

Graphics-queue state planning follows accepted submission order, not only last-completed state.
Track committed scheduled state and tentative state for the current recording separately. Do not
publish a final state or mark shadow contents rendered merely because commands were recorded. Device
loss invalidates the session ledger and ends normal scheduling; arbitrary cleanup is not inferred from
an unsuccessful GPU wait.

## 7. Descriptor publication and replacement

Owned generation replacement is transactional: create all required images/views, validate them, then
make the generation available. Allocation failure leaves the old generation selected. Enqueue only the
latest desired description to avoid building a chain of obsolete resize/cascade requests. Destroy an
unsubmitted failed/pending generation normally when no CPU recording references it.

For shadow's fixed per-slot heap entries, slot 0 can adopt generation B after its GPU wait while slot 1
still reads generation A. Its light UBO, layer count, cache version and shader-visible shadow descriptor
must all select the same generation. A global desired cascade count is not enough to set the UBO for
an old slot that has fewer allocated layers. Keep each frame's selected configuration explicit.

Fixed single-binding entries, such as current material/IBL slots, can be read by several in-flight
frames. Retaining old allocations does not make overwriting their descriptor safe. Keep their existing
global waits, or later allocate immutable generation-specific descriptors and pass the selected slot
explicitly. This later shader/binding change is not part of the first shadow migration.

Caches and recorded command buffers cannot silently retain borrowed pointers to freed generations.
Reset/re-record their dependent state before reuse, or retain their generation explicitly. Resource
retirement accounts for CPU recordings, submitted leases, persistent bindings and registry ownership,
not just a frame counter. A generation becomes releasable when all relevant owners have dropped it.

## 8. State tracking and barriers

Key the ledger by canonical allocation generation + concrete instance + normalized subresource/range.
Image layers/mips/aspects and buffer byte intervals must be distinguished. Disjoint uses need not
conflict. Overlapping RAW, WAR and WAW uses require ordering even when the nominal image layout stays
GENERAL. A repeated shader-write role is not evidence that no barrier is needed.

First add validation beside the existing manual barriers: each declared access should match the
recorded transition and legal content state. Report resource name/generation/slot/range and the two
passes for conflicts. Then enable generated transitions only for migrated families and supported role
pairs. Shadow's write -> sampled-read conversion is the initial automatic case.

Preserve shadow reuse: cached valid contents can already be in sampled state; an omitted shadow pass
contributes no new write. Spare array layers included by an exposed view need legal initialization/layout
and must be tracked, not assumed to have been rendered. Submission success commits content revision
and geometry signature; aborted frames do not mark the cache valid.

Descriptor aliases, sampled/storage views and physical-address access all converge on the same ledger.
This does not require rewriting every shader access; host-side declared actual uses are the authoritative
record. Debug validation can reject undeclared adapter access; it cannot magically discover arbitrary
shader pointer accesses absent from declarations.

## 9. Location in the existing code

- Keep render_resource.cppm as the pure description vocabulary; correct scope facts and add missing
  families/access descriptions incrementally.
- Add a nested engine module, e.g. deren.engine.render_resource.instances, for typed handles,
  generation ownership, registry and frozen resolved views. It may import the RHI, never Vulkan.
- Keep scheduling, frame leases and configuration adoption in runtime or a runtime resource partition.
- Use existing pass::resource_table/resolved_io as compatibility adapters instead of building a second
  unrelated resolver. New passes consume declared typed views; legacy passes stay clearly identified.
- Backend mapping, Vulkan allocation/barrier translation, descriptor heap publication and device-address
  retrieval keep their current backend/extension boundaries. The first migration adds no RHI vtable slots.

## 10. Migration and acceptance gates

This is the design sequence; a separate implementation plan should specify concrete tasks after review.

A. Shadow bundle: group image/view/capacity/cache state, make cleanup order explicit, retain existing
waits and manual barriers. Freeze the current shadow-fixed runtime beforehand and require matching
static/orbiting/transparent captures. This isolates ownership refactoring from image-quality changes.

B. Typed resolution: introduce handles, canonical generations and frozen frame views for shadow;
correct scope declarations. Verify distinct slot/image indices (2 slots versus 3 output images), null
window mode, missing resources and stale handles. Keep legacy shader slots.

C. Shadow slot adoption: retain submitted leases, adopt replacements only after successful slot opens,
and remove only the validated shadow replacement global wait. Test two slots in flight, rapid cascade
changes, disabled shadow reuse, creation failure, abandoned recording, skipped acquire and shutdown.
Assert no early destruction or retained-generation growth after slots complete.

D. State validation then generated shadow barriers: test layered writes, sampled array reads, cache
reuse, spare layers, aliasing ids and same-layout write hazards. Keep unsupported cases explicit.

E. Migrate post-processing, including TAA/MegaLights transfer-history uses; then Gaussian address buffers,
indirect args and the VRDX binding-state boundary; then AS build/jobs and single-binding scene assets
with suitable completion/publication rules. No family is considered migrated until all access channels
and outstanding submissions are represented.

CPU probes should verify state/ownership/adoption with deliberately unfinished submissions. Native GPU
checks cover synchronization validation, fixed and orbiting PBR, transparent materials, window restore,
windowless capture and async-present enabled/disabled. Memory accounting must distinguish active,
pending and submitted-retained generations. Steady-state resolution should use fixed-size/scratch
storage and per-frame deduplication, with no new allocation per draw. Performance measurements must
separate normal frames from rebuild events and compare identical visual settings.

## 11. Review checklist

- Logical incarnation, allocation generation and content revision have separate purposes.
- F/I/G domains follow actual host/shader bindings; temporal semantics are not changed implicitly.
- Descriptor replacement is safe independently of allocation retention.
- Direct addresses, attachments, indirect commands and copies are in the access model.
- Tentative recording changes become committed state only after successful submission.
- Imported outputs and independent GPU submissions retain conservative lifetime paths.
- Shadow cached contents and exposed spare layers remain valid.
- Initial work changes neither RHI ABI nor shader heap/push layouts.
