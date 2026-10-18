# Render graph to RHI mapping audit

The converters and optional scoped recording capability are implemented. The graph is the only recording path
now: since the 2026-10-17 cleanup every frame a build renders is graph-recorded, and the startup switches that
used to select it (and the legacy chains they fell back to) are gone. See the
[post-chain acceptance record](reviews/2026-10-16-render-graph-post.md) for current GPU evidence. This audit does
not imply that every subsystem's resources are declared to the graph.

## Frontend semantics and native layout are independent

The Vulkan backend uses GENERAL for ordinary image operations, attachments and
descriptors. UNDEFINED initialization and PRESENT are exceptions. This is the
existing [unified image layouts policy](unified_image_layouts.md), visible in
core.api_core.cpp's barrier_for, begin_rendering and copy functions, as well as the
constant_init barrier recipes. The role name shader_read does not mean that the
backend changes the image to SHADER_READ_ONLY_OPTIMAL.

The graph retains distinct abstract resource_layout values for sampled, attachment
and transfer semantics. They need not equal the native layouts. The earlier audit
recommendation to make the frontend adopt GENERAL is withdrawn following the user
design clarification. No frontend layout policy switch is needed.

Incoming and final graph states remain frontend handoff states. The Vulkan layer
maps ordinary layout names to GENERAL while keeping initialization UNDEFINED and
presentation PRESENT as exceptions. It preserves ordering, availability/visibility,
content preservation/discard and subresource ranges. Collapsing two abstract states
to one native layout must not erase a synchronization requirement. Native object
state and submission bookkeeping belong to the backend/execution coordinator;
the frontend state must not be mistaken for a literal VkImageLayout value.

## Implemented conversion chain

1. `deren.engine.render_graph.rhi` / render_graph_rhi converts a synchronization-
   compiled graph through rhi_bridge::lower_scopes into indexed scope packets. It
   preserves barrier order and resource/pass association, rejecting invalid ranges,
   indices, ordering and unknown flags. The core graph target still imports no RHI.
2. `deren.promise.rhi.synchronization` / rhi_sync_contract defines handle-free
   image/buffer transitions, independent endpoint stage/access scopes and explicit
   aspect/mip/layer or byte ranges. This is a separate data module, not an addition
   to the existing virtual recording face.
3. `deren.vulkan.synchronization` / vulkan_synchronization maps those descriptors to
   unbound VkImageMemoryBarrier2/VkBufferMemoryBarrier2 templates. It maps ordinary
   layouts to GENERAL and generic reads/writes to MEMORY_READ/MEMORY_WRITE masks,
   independently preserving source and destination stage unions. It emits a
   template even when oldLayout equals newLayout; no dependency is elided by layout
   equality. map_layout also exposes the backend's native layout spelling.

```cpp
import deren.engine.render_graph.rhi;
auto scopes = deren::engine::render_graph::rhi_bridge::lower_scopes(plan);
// Check scopes before using its barrier packets. No objects are bound by this call.
```

For example, frontend storage-write -> sampled-read can remain GENERAL ->
shader_read_only in the graph and GENERAL -> GENERAL in Vulkan, retaining compute
source write and fragment destination read masks. Dumps and final graph states
keep the frontend description; lowering never rewrites the input plan.

The returned native handles are null and must be bound before recording. These
functions perform no native calls or allocation on the Vulkan conversion side.
Queue capabilities, image format/usage/features, actual object bounds, native
state provenance and lease retention still need validation during real binding.
Depth/stencil subsets are retained, not silently widened; actual device support is
not inferred from a successful conversion. Native remaining-size/count sentinels
are refused as literal normalized counts rather than changing their meaning.

Tests link the independent converters. The backend DLL also implements the new optional scoped_synchronization capability. The legacy role-pair interface stays unchanged. Preparation negotiates the capability before binding and recording.

## Contract and implementation evidence

| Source | Relevant contract or behavior |
| --- | --- |
| [rhi.api_core.cppm](../source/promise/rhi/rhi.api_core.cppm), image_barrier/subresource_range | Image role pair and mip/layer range; no explicit layout, access mask or aspect mask |
| Same file, barrier_group/stage_hint | One optional shader-stage hint for the image batch; not independent source/destination stage sets |
| Same file, buffer_barrier/memory_barrier | Buffer role pair; offsets and sizes for buffer barriers; no explicit endpoint masks |
| [core.api_core.cpp](../source/backends/vulkan/core/core.api_core.cpp), image_use_pairs/barrier_for | Finite table of 22 image pairs; unspecified pairs are refused rather than inferred |
| Same file, with_stage_hint | Replaces shader bits on both image endpoints with the same hinted stage; does not combine arbitrary stage unions |
| Same file, buffer_masks_for | Fixed masks per buffer role; hint is not applied to buffers or the global memory barrier |
| Same file, subresource_of/aspect_for | Counts of zero become remaining mip/layer sentinels; aspect comes from format, with depth-stencil formats selecting depth, not caller-selected stencil |
| Same file, frame_commands::barrier | Up to eight image and eight buffer barriers per group; validates/builds the group before its pipeline-barrier call |
| [constant_init.cppm](../source/backends/vulkan/constant_init/constant_init.cppm) | Named role recipes retain stage/access dependencies while ordinary layouts are GENERAL |

These are properties read from the source. Existing recipe tests establish those
recipes' behavior; they do not certify coverage of every possible graph frontier.

## Usage and range audit

The following table concerns the legacy recording roles. The new scoped converter
and bound recorder retain this richer metadata. Bloom integration uses that path.

| Graph declaration | Closest existing RHI role | Remaining condition before graph execution |
| --- | --- | --- |
| sampled texture read | image_use::shader_read; depth_read is sampled depth in the old vocabulary | New scoped converter preserves endpoint masks and maps native layout to GENERAL; binding/recording still needed; sampled depth differs from read-only depth attachment |
| storage texture write/read-write | image_use::shader_write/shader_read_write | Table has selected compute paths, not every GENERAL read/write pair; no dedicated general-layout storage-read role or arbitrary mixed frontier |
| color attachment write/preserve | image_use::color_attachment | New converter maps native layout to GENERAL while retaining scopes; old recipes alone do not cover every accumulated frontier |
| depth/stencil attachment or read-only depth attachment | depth_attachment; depth_read names sampling | Explicit aspect and read-only attachment semantics need coverage; stencil-only requests cannot be translated by the current aspect inference |
| image transfer source/destination | image_use::transfer_source/transfer_destination | Only listed role pairs exist; preserve actual GENERAL state, ranges and all endpoint masks |
| shader buffer read/write/read-write | buffer_use::shader_read/shader_write/shader_read_write | Fixed role masks cannot represent arbitrary graph frontier unions; shader writes/read-write currently select compute |
| uniform buffer read | shader_read is the nearest broad shader role | No dedicated uniform role; any reuse needs explicit access-mask and stage coverage proof, not enum-name equivalence |
| vertex/index buffer read | No corresponding legacy buffer role | The scoped converter carries vertex-input stage/access coverage; the old recording entry cannot consume it |
| indirect buffer read | buffer_use::indirect_read | This reader role exists, but its preceding frontier may still require richer source scopes |
| buffer byte interval | buffer_barrier offset/size | Carry normalized nonzero size exactly and validate against the bound real object; RHI size zero means remaining bytes |
| texture mip/layer interval | subresource_range | Carry concrete normalized counts; verify real parent shape and view range during binding |
| texture aspect subset | No explicit aspect in legacy subresource_range | New scoped data and native converter carry explicit subsets; real binding/recording must validate format/features instead of using the legacy inference |

For example, setting stage_hint::fragment on a compute-to-fragment recipe also
replaces its compute shader source stage. It is not a way to independently set the
consumer stage. A single hint likewise cannot represent the graph's conservative
multi-stage read-only layout-transition destination or mixed reader/writer frontier.

Splitting one frontier into several guessed role pairs is not a certified fallback:
it can lose scope coverage or repeat a layout transition using an incorrect old
layout. The executor must reject an unrepresentable barrier during preparation.

## Scoped recording contract

Keep existing role-pair APIs for migrated legacy callers. The new scoped-data
module already carries backend-neutral, independent source/destination scopes,
abstract layout, aspect/mip/layer and byte ranges. The next recording entry must
bind this information without reducing it to one legacy role pair or stage hint.
Generic read/write masks are mapped conservatively by the new Vulkan converter.
No Vulkan masks belong in engine or graph interfaces.

The bound recorder validates all requested stages against the actual queue,
aspects/layouts against image features, and every object against its device domain.
Depth/stencil independent ranges need the appropriate backend capability or a
clear unsupported result. Do not widen an aspect to include an unrelated plane
while pretending the compiler modeled it. Unknown combinations fail preparation.

The bound recording API is a new optional `rhi::scoped_synchronization` capability,
using explicit image/buffer spans and complete scopes. Existing role-pair PODs and
virtual interfaces retain their layout. The engine execute module prepares every
batch before recording and retains resource leases. Production Bloom/post paths
use this API and publish initialization after submission; the generic recorder
does not publish outgoing state. See [binding and recording](render_graph.md).

## Bloom default-path acceptance gates

**This list is the gate the graph path had to pass BEFORE it became the default; it has since been
enabled, so read the numbered items as the record of what was checked rather than as pending work.**

The existing [post_bloom_pass::record](../source/engine/pass/post.cpp) is a graphics
fullscreen draw chain, not a compute dispatch chain. It currently owns its input
and output role barriers and its begin/end rendering scope. HDR input transition
and composite consumption have additional owners outside this function.

Before enabling a graph execution path:

1. Validate semantic-to-native lowering against actual backend state and the
   attachment/descriptor GENERAL expectations. Preserve the distinct frontend
   initial/final descriptions; do not reinterpret them as native layout values.
2. Retain canonical parent image/view leases and validate their device, shape,
   format, usage and range. Collect pipeline/descriptor/upload retention too.
3. Audit the real Bloom color-output and fragment-read scopes, including initial
   HDR handoff and all four exported levels. A discard/CLEAR content declaration
   must not erase outstanding accesses from an earlier frame using that object.
4. Specify outgoing sampled-state requirements or retain one explicit manual
   handoff owner. export_resource currently roots contents; it emits no epilogue
   transition. In particular, the deepest Bloom level still needs its handoff.
5. Prepare and validate the whole lowered plan before recording. Replace only
   the barriers owned by that migrated boundary; do not execute both barrier paths.
6. Publish final state after successful submission and retain all objects through
   completion. Preserve old state on failed prepare/record/submit. Slot readiness,
   uploads, WSI and cross-submission order remain external protocols.
7. Run image comparisons and synchronization validation for Bloom on/off, resize,
   multiple frame slots, failure paths and the manual composite boundary before
   making the new path default or removing its old branch.

The current CPU tests and dumps make these declarations reviewable; they do not
replace these execution and GPU acceptance gates.

