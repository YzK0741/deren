# CPU render graph core

Import `deren.engine.render_graph` and link `render_graph_core`. This independent
CPU library builds dependency and optional abstract synchronization plans. It creates no GPU resources, records no
commands, and emits no RHI barriers. Existing production pass chains remain active.

Resources are created outside the graph, for example through resource clusters.
The core accepts nonzero opaque `resource_identity { device, object }` values. These
values identify actual objects within a device domain; names do not identify
resources. The core does not own or dereference those objects. A later execution binding
layer must retain their leases and validate device compatibility.

## Build a plan

```cpp
import deren.engine.render_graph;
namespace rg = deren::engine::render_graph;

rg::builder graph;
auto image = graph.import_texture({1, 42}, rg::initial_contents::undefined, "HDR");
if (!image) return image.error();

rg::resource_access uses[] = {{*image, rg::content_access::discard_write}};
auto producer = graph.add_pass("Lighting", uses);
if (!producer) return producer.error();
auto output = graph.export_resource(*image);
if (!output) return output.error();
auto plan = graph.compile();
if (!plan) return plan.error();
// plan->execution_order contains indices into plan->passes.
```

Texture, buffer and pass refs are distinct types with graph incarnation checks.
Default or foreign refs are rejected. Reimporting the same device/object/kind
returns the same ref; inconsistent initial content, shape or resource kind is rejected.
Names and normalized accesses are copied into the builder. Rejected registration
does not append a partial resource, pass, output or order request.

The builder is noncopyable/nonmovable. Its first compile freezes it, including
when compilation fails; construct another builder to revise the graph. Owning
compiled_graph and graph_error values remain readable after the builder and
input strings are destroyed. Ordinary invalid inputs return expected errors;
allocation failure follows utility's existing fast_panic policy.

## Content and roots

Accesses are read, discard_write, preserve_write, read_write and invalidate.
A discard must actually overwrite the entire declared range. preserve/read_write
consume the previous contents of that range. invalidate leaves its range undefined.
An active read must have defined contents throughout its declared range.

## Typed pass parameters

The typed overload `add_pass(name, parameters, options)` owns a const copy (or moved
value) and derives its access list from that value. No parallel access array is
needed. Define `void visit_resources(parameter_visitor&) const noexcept` and enumerate
each resource field explicitly; ordinary scalar fields are not passed to the visitor.

```cpp
struct downsample_parameters {
    rg::texture_read source;
    rg::texture_discard_write destination;
    float exposure = 1.0f;
    void visit_resources(rg::parameter_visitor& visit) const noexcept {
        visit(source, destination);
    }
};
// After importing image with at least two mips and defining mip0:
auto pass = graph.add_pass("Downsample", downsample_parameters{
    {*image, {0, 1}}, {*image, {1, 1}}, 1.0f});
```

`texture_parameter<content_access>` and `buffer_parameter<content_access>` contain
`resource` and `range`. Aliases are available for all five operations:
`texture_read`, `texture_discard_write`, `texture_preserve_write`,
`texture_read_write`, `texture_invalidate`, and the corresponding `buffer_` names.
These express content operations. Each field also accepts a third `access_scope`
member containing usage and pipeline stages, as described below.

The visitor supports nested parameter structs, std::array, std::optional, and spans
constructed over owned fields. An absent optional declares no access. It visits
fields in enumeration order; the existing registration code validates and normalizes
their ranges and rejects conflicting operations or foreign refs transactionally.
Enumeration must be deterministic and free of side effects. Every actual resource
access must be listed, including indirect/bindless targets. This explicit protocol
cannot detect an omitted field and provides no automatic C++ reflection.

The builder first moves the value into its final const allocation, then visits it
once; move-only parameters work. `plan.passes[i].parameters.get<MyParameters>()`
returns a const pointer, or nullptr for a different type or a legacy untyped pass.
Parameter values survive the builder and original input. Copying a plan shares
the same immutable snapshot, including move-only values; destruction releases it
after its last owner. Failed registration releases its candidate snapshot.
Allocation uses utility's mimalloc allocator and existing fast_panic policy.

Constness is shallow: use owning value fields for variable data. Raw pointers,
string_view, span, reference wrappers and shared mutable objects do not become
deep snapshots. Their backing data must stay alive and immutable for the whole
plan lifetime. Prefer constructing spans inside visit_resources over the snapshot's
own arrays; do not store a span into the original object's storage. GPU leases and
descriptor retention remain the future execution binding's responsibility.

The span-of-resource_access overload remains available for CPU compiler tests and
low-level declarations. This increment stores parameters and compiles accesses;
it does not yet invoke workloads or record callbacks.

## Abstract synchronization (single queue)

`compile()` retains the dependency-only mode. `compile({.synchronization = true})`
additionally requires explicit scopes on every live access and an initial state on
every resource used by live passes or exported as an output. Missing declarations
return expected errors; no partial synchronization plan is returned. A successful
result sets synchronization_planned. This flag is false for dependency-only plans,
even when they happen to have no commands or contain scoped declarations.

```cpp
auto image = graph.import_texture({1, 42}, rg::initial_contents::undefined,
    "HDR", {4, 1}, rg::resource_state{}); // Explicit undefined initial state.
// After checking image:
downsample_parameters args{
    {*image, {0, 1}, {rg::resource_usage::sampled, rg::pipeline_stage::compute_shader}},
    {*image, {1, 1}, {rg::resource_usage::storage, rg::pipeline_stage::compute_shader}}};
// Define mip0 in an earlier pass, register args and export the required range.
auto plan = graph.compile({.synchronization = true});
```

The low-level resource_access constructor accepts scope as its fourth argument.
The initial-state argument follows shape on import_texture/import_buffer.
Repeated canonical imports must agree on initial state, including whether it is
specified. Supplied invalid scopes/states reject during registration even on dead
passes. Unspecified scopes remain legal declarations for dependency-only compilation.

| Usage | Resource and content operations | Pipeline stages |
| --- | --- | --- |
| sampled | Texture read | vertex_shader, fragment_shader, compute_shader, or their union |
| storage | Buffer or color texture read/write/read-write/preserve | Same shader-stage choices |
| uniform_buffer | Buffer read | Same shader-stage choices |
| color_attachment | Color texture discard/preserve/read-write | color_output |
| depth_stencil_attachment | Depth/stencil discard/preserve/read-write | depth_stencil |
| depth_stencil_read | Depth/stencil read | depth_stencil |
| transfer_source | Texture/buffer read | transfer |
| transfer_destination | Texture/buffer discard_write | transfer |
| vertex_buffer, index_buffer | Buffer read | vertex_input |
| indirect_buffer | Buffer read | draw_indirect |

Stage bits combine with `operator|`. Same-pass overlapping declarations must have
identical content operation and scope; combine shader-stage bits in one declaration
when the same range is read by several shader stages in that pass. Different usages
or stage sets on overlapping declarations reject instead of silently dropping one.
Live invalidate declarations return unsupported_access in synchronization mode;
content-only invalidation still works in dependency mode.

resource_state contains the current abstract resource_layout, pipeline_stage masks
and generic memory_access read/write masks. Backend native layouts may differ:
Vulkan maps ordinary abstract layouts to GENERAL without dropping dependencies.
Initial and final graph states remain frontend semantic handoff records. Initial contents and synchronization state are
independent: undefined contents do not erase prior accesses. An explicit undefined
layout with no accesses is only valid for undefined initial contents. Buffers use
general as their state layout (undefined is allowed on first import). Initial
texture state may also be present; WSI acquire/readiness remains external.

The initial-state parameter currently applies uniformly to the complete imported
object. Different incoming abstract subresource frontiers require a valid conservative
whole-object summary or a future range-state import API; do not pick one fragment's
state and lose the other fragments' dependencies. This does not require matching
the frontend names to native layouts.
State masks are a conservative outstanding-access frontier, not a description of
one last instruction. A read-only current layout can retain earlier write masks
and stages so a subsequent graph does not lose visibility requirements.

After culling and scheduling, barriers are generated in actual execution order.
Each barrier specifies resource, exact range, before_pass, before/after state and
layout_transition/memory_dependency flags. Record it before that pass. Read-read
image layout changes add state_transition dependencies in registration order;
incompatible reverse explicit ordering therefore forms a cycle. Compatible reads
merge stage scopes. Buffer role changes do not invent image layout transitions.
Writes wait for accumulated readers and previous writers; same-layout RAW/WAW
still produce memory barriers. A later reader in a new stage receives write
visibility even if an earlier reader already used the same layout.

A frontend read-only transition may become a physical transition on a backend
that uses distinct layouts; that physical transition can write image memory. The
CPU compiler therefore conservatively models it as a dependency. When there is no
logical writer to track, its destination scope conservatively includes every
currently supported image-access stage; the final frontier retains that widened
scope. This covers later readers in another stage as well as a following graph.
All layout transitions are marked as memory dependencies. This policy follows the
[Vulkan layout-transition requirements](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-image-layout-transitions)
and favors a simple correct baseline over minimizing destination masks.

final_states covers the complete shape of each required resource, preserving
untouched imported regions and applying only live executed accesses. These differ
from final_ranges, which describes all declared logical content, including dead
declarations. Final abstract states conservatively retain the last writer scope as well as
outstanding reader scopes. Sparse record order is not spatial sorting. These are
planned states, not evidence of submission/completion; publish them only through
the future successful-submission protocol. No final requested layout/epilogue is
emitted yet, including no automatic transition to present.

This model contains no Vulkan masks, RHI handles or queue ownership transfers.
Generic read/write masks permit a conservative backend mapping; an execution
adapter must map the complete stage sets, layouts and aspect ranges, or reject an
unsupported combination. Existing RHI recipes do not yet establish that mapping.
Host flush/invalidate, upload/WSI readiness, AS/ray/mesh stages, cross-queue work,
in-pass barriers/feedback, rendering-scope rules and GPU feature validation remain
outside this baseline. Abstract CPU tests do not validate GPU synchronization.

## Subresource ranges (first A2 increment)

Imports accept an optional fourth shape argument: texture_shape specifies mip_levels,
array_layers and the color/depth/stencil aspect mask; buffer_shape specifies byte_size.
Repeated imports must agree on the complete parent object's shape. Views refer to
ranges of that same canonical parent, rather than independent resource identities.

For compatibility, omitted shapes describe one mip, one layer, color, or one buffer
byte. These are symbolic defaults, not inferred GPU metadata. Supply the actual
shape when expressing ranges; the future execution binding must verify it.

```cpp
auto image = graph.import_texture({1, 42}, rg::initial_contents::undefined,
                                  "Pyramid", {4, 2, rg::texture_aspect::color});
// After checking image:
rg::texture_range level0{0, 1, 0, 1, rg::texture_aspect::color};
rg::texture_range level1{1, 1, 0, 1, rg::texture_aspect::color};
rg::resource_access downsample[] = {
    {*image, rg::content_access::read, level0},
    {*image, rg::content_access::discard_write, level1}
};
// level0 must have a producer before this pass becomes active.
```

Texture range fields are base_mip, mip_count, base_layer, layer_count and aspects.
Buffer range fields are offset and size. An omitted range selects the whole object;
remaining_subresources, remaining_bytes and texture_aspect::all resolve against
the imported shape. Normalized records contain concrete nonempty counts/masks.
Empty, out-of-bounds or overflowing ranges are rejected even on otherwise dead
passes. Validation uses subtraction before addition, including uint64 byte limits.

Disjoint ranges in one pass may use different operations, such as reading mip0 and
writing mip1. Overlapping ranges with different operations are rejected; express a
read-modify-write explicitly. Overlapping accesses with the same operation are
normalized into a disjoint union so overlapping read_write does not read its own
new version. Pass registration stays transactional on any rejection.

Content versions, initialization, value dependencies and hazards are range-aware.
Exporting a range retains only its current producers. Dependencies and content
reads report their exact overlap; explicit order edges have no resource range.
Errors for undefined contents identify the missing range. Clearing mip0 does not
define mip1, and depth writes do not define stencil. No pixel-level coverage is
tracked: a partial shader overwrite inside one mip still requires clear/preserve.

The compiler splits sparse rectangles/intervals only at actual access boundaries;
it does not allocate a state per byte, mip or layer. Fragment count still grows
with access complexity; this is a correctness baseline, not a resource pool or a
claim of constant memory for arbitrarily fragmented workloads.

Registration order determines the content version each read sees. Compilation
first traces value dependencies backwards from exported final contents and
passes marked `pass_options{.side_effect = true}`. Imported ownership or an
ordinary write is not itself a root. Unused passes, including overwritten dead
writes, are culled. Undefined reads in dead passes are ignored; invalid refs are
always rejected during registration.

Accesses through repeated imports use the same canonical range and conflict rules.

## Dependencies and order

After culling, compilation constructs RAW, WAR and WAW dependencies for live
accesses. Read-only sharing adds no hazard. Explicit `order_after(pass, predecessor)`
constrains order but does not keep an otherwise dead predecessor alive. An
explicit-order cycle is rejected even if its nodes have no output roots. Cycles
formed by live content/hazard dependencies and explicit order are also rejected.

Among ready passes, execution always selects the smallest registration index.
The result keeps dependency reasons (value, RAW, WAR, WAW, explicit order), live
flags and content reads. Multiple reasons may connect the same endpoints. Cycle
diagnostics identify a blocked pass; they do not promise a complete cycle path.

Each resource's final_ranges owns a disjoint cover of the full imported shape,
with producer, version and defined state per fragment. defined is true only if
all fragments are defined. last_producer is invalid_index when final fragments
have different producers (including imported contents). last_version counts
writing passes for the resource; multiple disjoint writes in one pass share a
version. Per-fragment producer/version is the authoritative content record.

These describe final declared logical contents, including culled declarations.
Reads include dead declarations for inspection. Dependencies and execution_order
contain only live execution. outputs records the requested normalized export
ranges. None of these records is submitted GPU state or completion.

## Implementation and limits

The private Boost.Graph backend supplies bidirectional adjacency storage,
reverse/filtered views and explicit-workspace BFS. Vessel selectors and graph
properties use utility allocation. Project Kahn scheduling reports errors through
expected rather than calling the throwing BGL topological_sort. Public interfaces
contain no Boost, RHI or Vulkan types.

Tests cover a 20,000-pass chain, version culling, read-modify-write, aliases,
hazards, stable order, cycles, owned diagnostics and OOM. The Boost allocation
probe observes the actual compiled graph module as well as its shared adapter.

Subresource ranges, typed parameter visitors, the baseline abstract barrier
plan and diagnostic dumps are implemented, and so are execution preparation, binding, lease
retention, GPU recording/submission and final-state publication: the converters and the optional
scoped recording capability live in the backend (see [the RHI mapping audit](render_graph_rhi_mapping.md)),
and the Bloom, post-chain and main-graph paths record real frames when they are opted in (see
"Composing an executable graph" and "First main-chain boundary" below). What the CPU core itself
still does not do is validate GPU synchronization on its own, and the graph does not replace
production rendering by default.

## Diagnostic dumps

Call dump_text(plan) or dump_json(plan) explicitly; both return an owned vessel::string.
The same functions accept graph_error after a rejected registration or compilation.
They perform no file writes, logging, recording or submission. They are normal
allocating diagnostic helpers, not part of fast_panic or the OOM path.

The text form has a short header and execution order, followed by one labeled
resource/pass/dependency/read/output/barrier/final_state record per line. Record
bodies use the same escaped field representation as JSON. JSON schema_version is
1 with kind compiled_graph or graph_error. Output is deterministic for the same
record ordering; records are not spatially sorted or canonicalized across unrelated
graph constructions.

The graph dump includes resource identity/shape, initial state, declared logical
contents, extracted parameter accesses, side-effect/live flags, cull reason,
dependencies, reads including dead declarations, outputs, execution order,
abstract barriers and final abstract state frontiers. Opaque user parameter values
are not serialized; parameters_present reports only whether a snapshot exists.
Dead passes use the coarse reason not_reachable_from_outputs_or_side_effects;
the dump does not invent a more detailed optimizer trace.

Each resource's use_interval is its inclusive first/last position in execution_order,
not its pass registration indices. An unused or export-only resource has null interval.
It represents scheduled live accesses, not GPU completion or allocation lifetime.
declared_final and final_states remain distinct logical and planned-state records.

All uint64 identities, versions, buffer sizes and byte offsets are decimal strings
in JSON, preserving precision in JavaScript tooling. Indices are numbers, with
invalid_index represented by null. Stage/access fields include symbolic arrays and
numeric masks. Valid UTF-8 names are preserved; invalid input bytes become U+FFFD,
one replacement per invalid byte. Quotes, backslashes, NUL and control characters
are escaped. The dump is diagnostic output, not a binary round-trip serializer.
It does not validate manually modified compiled records, although it bounds-checks
indices before deriving usage intervals.

The [RHI mapping and conversion documentation](render_graph_rhi_mapping.md)
describes the separate graph-to-RHI scope adapter and Vulkan native converter.
Different frontend/native layout names are allowed. The new converters preserve
independent endpoint scopes and explicit ranges; the old role-pair recording face
does not consume them. The separate execute module binds leases and records through
scoped_synchronization. No executable bindings or GPU recording are implied
by a dump or an unbound barrier template.


## Resource binding and recording

`deren.engine.render_graph.execute` adds `execution::prepared_graph`. Import
resources with `execution::identify(lease)`, compile synchronization, then pass
image/buffer leases to `prepared_graph::prepare(core, plan, bindings)`. Identity
comes from the backend allocation domain, so wrappers sharing a deduplicated
image import as one graph resource. Keep an original lease alive continuously from
import through prepare; numeric identities alone do not retain allocations or
guard against allocator handle reuse after destruction. Preparation
owns a snapshot and checks device identity, exact resource shape, live access
usage/stages and every bound barrier through the optional
`rhi::scoped_synchronization` capability. Resources used only by culled passes
need no binding; exports still need one. Duplicate or unrelated bindings fail.
A backend without the capability returns an error before any recording.

`record(commands, callback)` emits a batch before each live pass, then invokes a
const noexcept callback returning `rhi::error`. `record_context` exposes the pass
record and only the images/buffers declared by that pass. This context is borrowed
only during the callback; copying it does not retain resources. Empty batches still
check the command buffer. A recording or callback error stops execution; the
caller must discard that partially recorded command buffer.

Keep `prepared.retention()` until GPU completion. It pins the core, graph and
bound leases; recording does not submit, wait, publish final state or determine
GPU lifetime. Pipelines, views, descriptors and uploads used by callbacks need
their own retention. Parameter snapshots retain their existing shallow ownership
rules. Initial states remain the caller's handoff declaration, and exports do not
request epilogue transitions. The API consumes compiler-produced graphs; editing
compiled records does not recompile or repair their synchronization.

This is an opt-in entry point. The post-processing chain has a graph path;
scene rendering still uses the established path.



## Opt-in Bloom boundary

Set `[render] bloom_render_graph = true` before startup, or call
`runtime::set_bloom_render_graph(true)` before the first pace attempt. The default
remains the existing path; changing the graph selection during a running session
returns not_ready. The ordinary Bloom intensity/debug-view gates still apply.

`deren.engine.render_graph.bloom` compiles four graphics passes: HDR prefilter,
then three downsamples. Each reads the previous image in the fragment shader and
CLEARs its own color attachment. A fifth boundary pass samples all four outputs;
it records synchronization only and establishes the composite's incoming state.
The HDR transition is still owned by the preceding legacy boundary. The graph
callback uses `post_bloom_pass::record_content`, which omits all legacy barriers.
Pipeline binding, viewport, heap slots, shader constants and fullscreen draws are
shared with the established implementation.

The runtime pins existing output generations through aliasing shared image leases;
`resource_lease::from_shared` retains the supplied owner without taking a second
raw release reference. Generation retention also keeps the existing image views
alive. The prepared graph/off-path leases remain in the paced frame slot until
that slot completes. Callback-only pipelines and descriptor storage still follow
the runtime's existing ownership/wait protocols.

Only successful submission publishes Bloom initialization. State is keyed by
resource-generation ownership identity and image instance, so an unrelated output
rebuild does not discard another image's frontier, and returning to a still-live
generation preserves its state. Expired generation entries are reclaimed. Reused
images retain fragment-read/color-write scopes even though their texels are
cleared. A graph failure aborts end_recording and blocks submission; it never
falls back to legacy commands after partial graph recording. Present failure does
not undo an already successful queue submission.

When Bloom is disabled, a scoped boundary establishes sampled layouts for the
statically used descriptors. It declares no graph read of undefined texels.
Real-device captures and synchronization validation are required before changing
the default or removing the legacy path.

## Opt-in post-processing chain

Set `[render] post_render_graph = true`, or call
`runtime::set_post_render_graph(true)` before the first pace attempt. This selects
the complete post chain instead of the narrower Bloom-only option. Both default
to false. The graph contains four Bloom passes, composite and the selected FXAA
or upscale resolve. Composite declares HDR, all four Bloom levels and the optional
color LUT. Disabled Bloom clears its four attachments: the current shader samples
them even at zero intensity, so their contents must be defined.

`deren.engine.render_graph.post` builds the chain from imported identities and
states. Resource creation remains outside the graph. Existing generation owners
pin HDR/Bloom/LDR images and views; frame-slot retention keeps graph bindings and
the output view alive until GPU completion. Bloom/LDR initialization is published
only after successful submission. The scene-to-HDR boundary remains external.

The optional `rhi::frame_resources` capability returns a shared snapshot of the
currently acquired output. Its identity is stable within that acquire, distinct
from the mutable legacy `frame_image()` facade. It pins the core, but does not
extend the native swapchain generation's validity through resize. Submission,
frame advance and reacquisition invalidate old bindings; prepare and record check
their identity, including passes without barriers. Snapshot images cannot be used
with an independently owned command buffer. Existing acquire/submit/resize rules
remain mandatory.

For a windowed snapshot's initial UNDEFINED transition, the Vulkan recorder adds
a source execution scope covering its acquire-semaphore wait. Discarding contents
does not remove WSI ordering. This augmentation belongs to the acquired-image
protocol; ordinary owned images retain the generic UNDEFINED mapping.

Composite/resolve callbacks use checked `record_content` entry points without
legacy barriers. UI stays inside the final rendering instance. Screenshot/readback
still runs through its existing protocol after drawing. A separate graph handoff
then establishes PRESENT (windowed) or sampled (offscreen), with PRESENT carrying
no destination stage/access. Recording failure blocks submission instead of
falling back after partially recording a graph.

Canonical owned image/buffer identities use stable backend allocation-record
addresses, shared by deduplicated wrappers; frame snapshots use their own object
addresses in the same core domain. Keep leases alive from import to preparation.
Identity values are process-local and do not own resources.

## Composing an executable graph

`deren.engine.render_graph.compose` adds `execution::builder`. Import image/buffer
leases with explicit initial contents/state; actual resource shape comes from the
backend description. Consistent canonical aliases return the same ref; conflicting
imports fail. GPU resources are still created outside the graph.

Use `frame.append(fn)` to synchronously run a pure CPU construction function against
the shared description builder. The callable must be noexcept and return an owned
`expected<T, graph_error>` value, including void. An append error is latched: a
partially constructed graph cannot subsequently prepare. No builder reference may
escape the invocation. `post::append(graph, inputs, options)` demonstrates module
composition: it returns named pass refs and a color ref without importing,
exporting or compiling. The standalone `post::compile` wraps that same function.

Bind returned pass refs with `bind_pass<Parameters>(ref, recorder)`. `ownable`,
`graph_appender` and `pass_recorder` constrain stored values and exact noexcept
signatures. Move-only recorders are supported; null function pointers are rejected.
The recorder receives the original typed parameter snapshot, commands and borrowed
record_context. Const invocation still permits GPU side effects. Refs retain their
builder incarnation; graph indices or pass names cannot substitute for a pass ref.

The top level exports its actual outputs, then calls `prepare()`. Every live pass
must have a correctly typed recorder or explicit `bind_barrier_only`, checked before
any commands are recorded. Dead passes need no action. The prepared graph's
`record(commands)` uses the same synchronization loop as the legacy callback API;
the two action modes cannot be mixed. Any failure stops recording and the caller
must refuse submission.

Recorders and explicit `retain(shared_owner)` objects join the existing GPU
retention token. Store frame data at a stable address before resolving self-pointing
spans; copying a span or capturing a raw pointer does not retain its owner. The
runtime post adapter now uses this mechanism instead of global pass-index dispatch.
The graph still does not submit or publish cross-frame state.

See the [composition design](superpowers/specs/2026-10-16-render-graph-composition-design.md)
and [implementation acceptance](reviews/2026-10-16-render-graph-composition.md).

## First main-chain boundary

`[render] main_render_graph` (or `set_main_render_graph(...)` before the first pace) enables scene
geometry, GBuffer handoff, deferred lighting, the whole scene tail (shadow composition, cluster sort, the
ray-traced shadow chain, TAA resolve + history copy, GBuffer debug view, gaussian splats, transparency, character
forward and the two screen rims) and post in one graph, and also enables the post graph. **It now DEFAULTS TO
TRUE**: every gate scenario records through the graph and is byte-identical to the legacy path, and a ray-traced
frame has been A/B-verified byte-identical between the two paths (see the gate review's stages 2F-2O and the RT
slices). The ONLY feature that still selects the legacy scene tail before graph recording, per frame and with the
reason logged, is **MegaLights**, kept as an EXPERIMENTAL feature by the owner's decision: its trace is a compute
ray query whose sampled-lights result is not capture-reproducible, so it has no byte-identical acceptance
instrument. Startup selection is locked like the existing graph options. Disabling post while main is selected is
an invalid argument; disable main first if changing startup configuration.

`deren.engine.render_graph.lighting::append` accepts the three surface refs, depth
and existing scene-color ref. It declares fragment sampling and a preserve-write
color attachment, so the LOAD keeps the emissive produced by geometry. The returned
HDR ref feeds post::append directly, eliminating the manual HDR sampling handoff.
Depth sampling uses sampled/fragment scope; depth_stencil_read describes attachment
depth-test access and is not a substitute for shader sampling.

`deren.engine.render_graph.geometry::append` declares discard writes for the three
surface targets, velocity, HDR and depth. These imports start with undefined
contents, and the geometry callback clears them even when the scene has no visible
leaves. The existing scene pass still owns secondary recording and scheduling;
its checked content entry point propagates begin/end/execute failures after closing
the rendering scope. Graph eligibility is frozen before the legacy scene producer
would record, so supported frames have no duplicate geometry or attachment barriers.

Mesh/material/bindless inputs, deformation, shadow and cluster producers, host
publication and inactive descriptor-layout protocols remain outside this graph's
coverage. This slice does not claim full-frame resource tracking. Existing
generation owners pin all six attachments through aliasing leases, and the
composed payload has stable storage.

### Shared read-only inputs

Geometry also accepts an optional `geometry::shared_inputs` bundle: camera,
materials, instances, previous transforms, current/previous skin matrices and morph
data. Typed parameters declare camera uniform reads in mesh+fragment, materials
storage reads in fragment, and the remaining storage reads in mesh. Lighting's
optional `lighting::shared_inputs` bundle declares camera/light uniform fragment
reads. Attachment-only CPU clients may omit these bundles; the runtime main path
requires all eight buffer imports and reuses one camera ref for both consumers.

Per-slot buffers use the frame slot, not the swapchain image index. Every import
checks its device/generation and expected borrowed handle, then retains an aliasing
lease. Shared tables conservatively declare their full buffer range. Data comes
from the existing CPU publication protocol: slot completion, writes, required flush
and submission. Importing defined read state does not itself synchronize a host
write. No fake GPU upload pass or host-write barrier is introduced.
Camera/light imports must support uniform-buffer use, matching the shader's
ConstantBuffer declarations. A custom storage-only constant generation is refused
by graph preparation even if the legacy provider validation accepted it.

Task and mesh shader stages are available throughout the CPU graph, RHI scope
bridge and diagnostics. Vulkan maps them to their EXT stage bits and advertises
them only when the respective device feature was enabled on the graphics queue.
Unsupported consumers fail during preparation. Ordinary post graphs do not acquire
task/mesh masks; read-only image transitions include those stages when that image
has corresponding live consumers in the graph. Cross-graph handoffs must describe
their required scopes explicitly.

Material textures/IBL, per-mesh addresses, meshlet tables/atomic statistics, indirect
commands and cluster producers still need separate declarations. The shared bundle
does not claim to enumerate every resource the pass can access.
See the [shared-input acceptance record](reviews/2026-10-16-render-graph-shared-inputs.md).

During graph-owned deferred preparation, the existing surface/depth ensure hooks
are suppressed; CPU frame preparation still runs. The graph emits the handoff, then
checked deferred record_content records only LOAD/draw commands. Recording or
preparation failure blocks submission; no legacy tail is replayed after partial
graph recording. See the [lighting acceptance record](reviews/2026-10-16-render-graph-lighting.md)
and [geometry acceptance record](reviews/2026-10-16-render-graph-geometry.md).
