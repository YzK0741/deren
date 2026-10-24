# Deferred render operations

Date: 2026-10-24
Status: implemented in the 2026-10-24 checkpoint; accepted with follow-ups (see
`docs/reviews/2026-10-24-render-operations.md` and `...-review.md`).
Independent builder check: no important unanswered planning questions reported.

## Intent and decisions

Record RHI operations in command-owned storage, simplify redundant state, validate
the resulting stream, and lower it to native commands in order. Keep resource
groups and unified push as the public binding model. Backend descriptor containers
stay private.

The user selected a polymorphic `render_operation` base with concrete operations,
value equality, and `is_unique()`. Both duplicate state elimination and removal of
state overwritten before use are required. The proposed application mechanism is
explicit `apply_operations()`, with automatic final application in
`end_recording()`. These calls report errors; existing void draw methods initially
keep their signatures.

The detailed boundaries and conservative rules below are engineering decisions
derived from the current implementation, rather than additional user requests.

## Goals and limits

- Reduce redundant native state recording without changing rendering results or
  the order and count of observable GPU work.
- Validate at enqueue and application, with ordinary RHI errors for caller errors.
- Own operation payloads and preserve the existing GPU resource lifetime contract.
- Support primary and mergeable command buffers through their common recorder.
- Preserve the explicit descriptor heap bind used by the Gaussian GUI workaround.

This first version does not add parallel command recording, command sorting,
automatic synchronization, render graph dependency inference, draw fusion, or a
new resource ownership system. It does not remove draws, dispatches, copies,
barriers, or timestamps on speculative grounds. Performance must be measured;
fewer native setters alone do not establish a frame-time improvement.

## Approaches

1. Immediate recording with a backend state cache is small, but cannot remove a
   distinct setter overwritten before use without buffering it.
2. A deferred polymorphic operation stream supports the user's chosen equality
   and uniqueness mechanism, owned payloads, and both simplification passes.
3. A tagged value stream can perform the same analysis with different storage
   costs, but does not follow the requested base/derived organization.

Use approach 2. Avoid one heap allocation per operation by placing derived objects
in command-owned arena or pooled storage. Arena sizing and concrete container
choices are left to the implementer and must follow the project's vessel and
no-exception allocation conventions.

## Operation contract

The conceptual base interface contains a virtual destructor, `kind()`,
`is_unique()`, and same-kind equality. A nonvirtual `operator==` checks kind before
delegating to equality. No RTTI is required. Actual names may follow nearby RHI
conventions; member names have no prefixes or suffixes.

Equality means equal operation values, including resource identity, offsets,
ranges, and copied payload bytes. It is not permission to remove an operation.
`is_unique() == true` means that equality never justifies removing that occurrence.
It does not mean that its arguments are different from all other operations.

| Family | Examples | Unique | Initial treatment |
| --- | --- | --- | --- |
| State | pipeline, vertex/index binding, resource group, viewport/scissor, push | false | Simplify using state dependencies |
| Execution | draw, indexed/indirect draw, dispatch, mesh tasks, trace rays | true | Retain every occurrence in order |
| Effect/boundary | copy, barrier, clear, build, execute, rendering scopes, timestamps | true | Initially flush and emit immediately |

Start by buffering state and execution families. APIs outside that initial set
are ordered application boundaries, so the implementation can grow incrementally
without silently changing their ordering. Each secondary has its own stream.

Effect information is explicit, using a rule table keyed by operation kind or an
equivalent internal representation. It describes state reads, writes, and opaque
clobbers. Equality and uniqueness are only the coarse filter, never substitutes
for this information.

State keys distinguish pipeline/bind point, vertex slots, index binding, group
binding, dynamic state, and push byte intervals. Draw consumes its applicable
pipeline/group/push/dynamic/vertex state; indexed draw also consumes index state.
Dispatch does not consume vertex or index state. The implementer must define the
read/write table for every initially supported operation; unknown operations are
conservative boundaries.

## Two simplification passes

First remove adjacent equal, non-unique state setters when no intervening effect
or clobber changes their meaning. Never use a global set of equal operations.

Then remove a state setter if its complete written state is overwritten before
any consumer or boundary observes it. Backward liveness or equivalent last-writer
tracking is acceptable. Keep the relative order of retained operations.

Examples:

```text
bind_vertex(A), bind_vertex(A), draw
  -> bind_vertex(A), draw

bind_vertex(A), bind_vertex(B), draw
  -> bind_vertex(B), draw

bind_vertex(A), draw, bind_vertex(B), draw
  -> unchanged

draw(X), draw(X)
  -> unchanged
```

An earlier push may be removed when later writes completely cover its interval
before a consumer. Preserve partially overlapping earlier pushes in the first
version; interval splitting is optional future work. Copied bytes participate in
equality. Do not infer that shader inputs are unused merely from absent reflection
data.

The requested fine filtering of "unique but unused" does not grant blanket
permission to remove protected effectful operations. Initially it removes
distinct state writes proven unused. No zero-count draw/dispatch/copy elimination
is included; statistics, queries, and side effects require separate proof.

## Canonical state and pipeline behavior

Keep desired logical state separate from the native applied-state cache.
Optimization operates on logical state; lowering establishes the required native
state for each consumer. The native cache is invalidated by opaque boundaries.

Current Vulkan pipeline binding publishes zero/default resource-group token
fields. Model those writes in lowering, not as hidden logical side effects of
every pipeline setter. Rebinding the same pipeline must not erase a loaded
compatible group. Across a pipeline switch, retain the logical group but validate
it against the new pipeline at the next consumer, and republish its fields as
needed. An incompatible group is an error unless replaced before consumption.

Preserve the existing push contract: pushes may precede pipeline binding, but
cannot overwrite reserved group token fields when the relevant pipeline is known.
Enqueue validates against the currently desired pipeline when available. Before a
consumer, validate the live user push ranges against that consumer's reserved
fields as well. If a later pipeline introduces a conflict, report an error; do
not silently reinterpret user bytes as group tokens. Compatible pipeline changes
preserve user push state. Native default publication must not erase valid user
push bytes.

Do not add blanket shader-parameter-initialization checks or change existing
defaults as part of this work.

## Application and recording boundaries

`apply_operations() -> error` applies pending operations while recording remains
open. Empty application succeeds. Outside recording it reports the existing
invalid-state category. Native recording begins through existing begin methods;
this is CPU deferral within the established command-buffer lifecycle.

An intermediate application is an observability boundary: retain and emit final
state setters even without a draw in that batch. They can be observed by later
recording or native interop. Carry the resulting logical state forward. At final
`end_recording()`, trailing pure state with no future consumer can be discarded.
Final application precedes native command-buffer end.

Before an immediate operation, apply the preceding batch and then emit that
operation at exactly its original position. Rendering scope changes and timing
operations follow this rule. Scope state is checked at enqueue so a queued draw
outside an appropriate rendering scope is not accidentally accepted.

Executing secondaries applies preceding primary operations, emits execution,
retains secondaries through submission, and conservatively invalidates the
primary's applied-state cache. Keep the logical state and re-establish it before
subsequent consumers where required by the native API. Never merge streams across
different command buffers.

Native access needs an explicit interop protocol: apply pending work before raw
recording, and invalidate applied state after raw recording. The current native
`get()` accessor alone cannot report an application error, so it is not an
implicit flush mechanism. Add an error-returning preparation/finish bridge and
migrate raw recording callers before enabling deferral on those routes. Raw
recording may clobber any native state; subsequent RHI consumers restore logical
state. Interop must remain inside the valid recording/scope lifecycle.

The GUI's secondary uses `inherit_descriptor_heaps = false` and an explicit engine
heap bind before ImGui recording. Keep that bind and its position. It is a native
interop requirement, outside ordinary redundant RHI state elimination.

## Validation and errors

Enqueue checks inexpensive facts: recording state, argument ranges/alignment,
required non-null objects, ownership/provenance, and structural scope legality.
Rejected arguments are not appended. Optimization must not hide these errors.
Retain errors already detected synchronously by error-returning APIs where their
validity is independent of a later consumer. Temporary state incompatibility
introduced by later pipeline changes is validated when consumed.

Validate the complete retained batch before emitting any native commands from
that batch. This is not rollback of native commands emitted by previous batches.
Deferred consumer errors surface through `apply_operations()` or
`end_recording()`; document this timing change. Void APIs can latch the first
recording error. An implicit application failure at a void boundary also latches
the error and emits no further ordinary commands.

A latched/deferred application failure makes the recording non-executable.
`end_recording()` returns that error, performs only native scope/recording cleanup
needed for a safe reset, and never publishes the buffer for submission. Recovery
uses the existing legal reset/begin lifecycle. Reset clears operations, caches,
retained recording references, scope state, and the first error. A rejected
error-returning enqueue does not poison the stream unless it reflects a prior
latched failure. Do not panic for recoverable caller mistakes.

## Payload and resource lifetime

Copy push bytes and other caller-owned arrays when enqueued; spans cannot outlive
the call by assumption. Retain resource-group shared references while pending.
After emission, transfer or retain native-use references in the existing
recorded/submitted lifetime structures until GPU completion. Destroying an
operation after emission must not release a resource still used by the GPU.

Reference-taking pipeline/buffer APIs keep their existing caller obligation:
their owners remain alive through GPU use. Deferral does not turn a borrowed
reference into shared ownership. This obligation now explicitly includes pending
CPU recording. Document it in the RHI contract. Resource-group owned members keep
their stronger existing retention guarantee.

Use registered object identities (including device and lifetime generation) or
equivalent non-reused identities for equality/cache entries. Do not rely solely
on raw addresses that can be recycled. Validate liveness through the backend
registry before dereferencing cached borrowed objects at application. A stale
identity fails recording; it must never refer to a replacement object at the
same address. Reset releases all pending storage and references according to the
existing in-flight reset restrictions. There is no new concurrent destruction or
recording guarantee.

## Integration and verification

Keep analysis rules shared where possible, with Vulkan native emission private.
The CPU probe backend needs equivalent recording/application semantics so tests
exercise the contract rather than a different immediate interface. Public virtual
interface changes require the next ABI revision during implementation; this
design-only change does not bump the current ABI 52.

Verify with a deterministic reference interpreter and optimized/unoptimized
operation streams. Cover repeated draws, A/B/A state, consumers between setters,
different state slots, push copying/full and partial overlap, pipeline/group token
interactions, intermediate and final application, scope/timestamp/execute
boundaries, native interop invalidation, stale identities, failure/reset, and
group retention through completion. Check emitted sequences as well as resulting
logical state; call counts alone are insufficient.

Run existing CPU/contract tests, resource-group and secondary GPU probes, then PBR
and static-camera Gaussian GUI smoke checks with validation enabled. Measure CPU
recording cost, allocation count, and native setter count with optimization on
and off on the same workload. Rendering output must agree. Keep an internal
unsimplified replay path for verification; a public toggle is not required.

The implementer chooses arena chunk sizes, rule-table representation, internal
class/file split, and benchmark fixtures. These do not change the contracts above.
Implementation planning and code changes are a subsequent step.
