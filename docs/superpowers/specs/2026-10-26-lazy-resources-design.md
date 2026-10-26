# Lazy buffer and image resources

## User-selected behavior

Add public RHI types `lazy_buffer` and `lazy_image`. Accept only shared-owned source bytes, retain
the resource description and source owner, and allocate/upload at first real native-command use.
This includes a surviving binding lowered to native commands, or an explicit resource barrier that
needs a native handle. Creation and logical bind recording do not allocate a GPU resource or upload.
No scene streaming scheduler, CPU BVH, visibility feedback, automatic eviction or production uploader
policy change is part of this work. First use is synchronous; asynchronous preparation is future work.

## Public shape

The following are proposed implementation choices, not additional user requirements:

```cpp
using lazy_data = std::shared_ptr<utility::vessel::vector<std::uint8_t> const>;

struct lazy_buffer_desc {
    std::uint32_t struct_size = sizeof(lazy_buffer_desc);
    buffer_desc resource;
    lazy_data data;
};
struct lazy_image_desc {
    std::uint32_t struct_size = sizeof(lazy_image_desc);
    image_desc resource;
    lazy_data data;
};
// New public interfaces; inherited ordinary resource methods remain available.
struct lazy_buffer : buffer { /* state query */ };
struct lazy_image : image { /* state query */ };
```

Expose error-returning factories on `resource_factory`, returning
`expected<unique_object<lazy_buffer/lazy_image>, error>`; an unimplemented backend returns unsupported.
Do not add lazy-specific bind overloads. Derived objects can use supported existing resource bindings.
Advance the current ABI by one when implementation changes the factory vtable; audit loader fixtures,
CPU test backends and RHI standard with the implementation rather than assuming the current number.

`data` is mandatory and nonempty. `resource.initial_bytes` must be empty: there is exactly one source.
Validate byte count at creation: exactly buffer size, or the packed image extent/mip/layer payload.
Copy metadata and debug-name text, retain the shared owner without copying its payload. Normalize
image mip count before validating. The source is immutable from creation until the last lazy object
reference is released, including through other aliases; shared_ptr<const T> cannot enforce this alone.
No callbacks, file I/O, decoder, raw-pointer source, span-only source or mutable source is introduced.
Callers can construct shared-owned vessel bytes once and share them across resource descriptions.

This concrete container is an initial narrow source contract. No generic shared_ptr<void> plus an
unrelated span is accepted, because that would fail to express which bytes the owner actually owns.

## First supported uses

- Device-only buffer usages already supported by normal creation, consumed through vertex/index
  bindings or uniform/storage buffer members of resource_group. Reject directly mapped usages at
  lazy creation with unsupported; `mapped()` returns empty without loading. Device-address allocation
  flags are allowed and required where existing descriptor bindings need them. Public raw-address
  extraction for a lazy object is unsupported and does not load; backend internal address resolution
  happens after materialization. Addressable storage does not expose an early raw address.
- Sampled color images, including existing supported mip/layer/cube descriptions, consumed through
  texture_group, resource_group or logical descriptor_space bindings. Reject storage/attachment and
  host readback roles for lazy images in this first increment. Normal resources keep their behavior.
- Lazy image views are logical objects retaining the image and validated view description. Creating
  or sharing a view must not materialize it. Incompatible view roles are refused before allocation.
- Direct copy/clear/indirect-argument/build/native-address uses of lazy objects are unsupported in this
  increment and must fail explicitly before reading a nonexistent native handle. An explicit resource
  barrier, including scoped synchronization, constitutes demand when recorded natively: validate the
  request, materialize its resource, then resolve the native handle and record the barrier. A barrier-only
  recording can therefore load resources. No silently dropped transition or fake handle is allowed.
  Upload establishes an internal initial access/layout; preserve public barrier semantics when
  transitioning from it. An explicit undefined/discard request must not be misrepresented as preserving
  initial content. Audit imported initialized-image states in graph probes accordingly.

These limits keep first-use loading explicit and testable; accepting a lazy factory call is not a
promise that every ordinary resource capability is supported by a lazy resource.

## Binding and native lowering

The Vulkan backend already records operations and simplifies them in `apply_operations()`. Extend
that path: simplify, collect the required surviving resource bindings, retain the lazy owners,
materialize missing resources, resolve native bindings/descriptors, then emit native commands.
Do not materialize in logical bind setters or inspect shader execution to guess demand. Binding A then
B with no consumer of A must not upload A when simplification removes it. Existing operation statistics
remain about operations; add focused lazy-resource counters/probes rather than changing their meaning.

Existing barriers and rendering-scope boundaries flush operations and resolve native resources before
later bind calls. Do not add whole-command-stream lookahead merely to enforce bind-only loading;
explicit native resource barriers are first-use demand as above. All related scoped-sync paths must
use the same resolver. Avoid accidental eager loads during registry lookup.

Validate metadata/provenance/layout compatibility before loading so an invalid bind does not allocate
resources. Deduplicate the demand set. Resource identity and operation equality use the stable logical
object identity, never a changing native handle. Materialization occurs outside resource-registry locks.

Resource-group creation currently resolves physical resources and writes descriptors. For groups
containing lazy members, retain logical member descriptions and resolve physical views/descriptors at
first actual use. Ordinary-only groups keep the current eager path where practical. Texture groups
and logical fixed-slot descriptor writes need equivalent logical retention; merely writing a slot must
not materialize its resource. Bind resolves the lazy members of the bound container conservatively.
All members of a bound group/fixed-slot profile count as demanded, even if shaders dynamically sample
only a subset. This feature alone cannot stream an all-scene bindless texture table selectively.

Any descriptor storage newly made lazy must have a stable lifetime and immutable contents while used
by a recording or pending submission. Do not patch descriptors still visible to GPU readers. No
automatic per-frame versioning or relaxation of existing descriptor-update synchronization is added.
Prevalidate a fixed-slot update before replacing its retained logical reference; invalid updates leave
the previous binding unchanged. Serialize descriptor updates according to the existing contract.

## Materialization and errors

States are unloaded, loading, ready and failed, observable without triggering loading. One thread
performs materialization; concurrent first users wait for that operation and observe the same result.
Publish ready only after physical creation and initial upload visibility are complete. Never publish
partially built resources. Record the failure error and keep failed terminal for this increment; no
implicit retries or retry API. Independent successfully materialized resources may remain ready after
another member fails: no all-or-nothing allocation transaction is promised.

Reuse existing validated backend allocation/upload helpers. A buffer staging upload may submit a
separate upload command and wait for its fence before consumer submission. It must not emit copy
commands inside an active rendering scope, reenter the current recorder, wait on its own unsubmitted
commands, or call whole-device wait_idle. This initial synchronous path can wait behind previous work
on the same queue; it does not promise low-latency streaming. Normal image upload policy remains as-is.

`apply_operations()` and `end_recording()` report the concrete materialization error through the
existing latched recording-error channel and mark the recording unexecutable. Previously emitted native
commands in a failed recording cannot be submitted. No fallback resource or hidden content substitution.

Implementation detail: existing physical allocation/upload helpers collapse refusal into
operation_failed. Checked begin/end/submit/wait/reset sequencing prevents ready publication after
failure and waiting after failed submit. If a successful submit is followed by an unsuccessful wait
other than device loss, completion remains unknown; use existing fast_panic fatal handling to avoid
freeing resources that might still be pending. No per-resource quarantine/recovery is implemented.

Recorded commands retain resolved lazy resources through the recording and all pending submissions;
reusable executable recordings retain them until reset/destruction. Cover frame, independent and
mergeable command paths. GPU completion releases submission references, not references held by a
still-executable reusable command. Deferred views/groups retain their dependencies transitively.

## Lifetime and memory scope

Keep source bytes shared-owned for the lazy object's whole lifetime, including after upload. This
costs CPU memory intentionally and permits later unload/reload work without changing source ownership.
Physical allocation stays until the object's last reference is released; no automatic unload or budget
policy. All objects continue to require their originating core/backend to outlive them. CPU-memory
retention, GPU-resource retention and backend-module lifetime must follow existing ownership rules.

Creating 100 lazy resources and binding 10 can allocate just those 10; eventually binding all 100 can
make all 100 resident. This is deferred creation, not a complete memory-residency system.

## Verification and acceptance

- CPU tests: exact byte-length validation, mandatory owner, conflicting initial_bytes, unsupported
  usages, copied metadata/name lifetime, source-owner lifetime, no eager loading through queries/views.
- Recording tests: no-use and superseded-bind resources remain unloaded; consumed binds load once;
  shared resource/group references and concurrent first use do not duplicate uploads; failures latch
  and prevent submission. Logical descriptor writes alone do not load; binding demands members.
  Explicit native resource barriers do load; a superseded bind only stays unloaded if no other
  native command (including an earlier barrier) demanded that resource.
- Device probes: deterministic buffer/image contents through supported uses, lazy groups/views,
  barriers and descriptor synchronization with Vulkan validation; frame/independent/mergeable paths;
  source-owner release by caller before lowering; command reset and pending-completion lifetime.
- Resource-allocation counters establish zero physical allocation before demand, then one physical
  materialization per lazy object. Do not infer this from process VRAM alone (allocator blocks persist).
- Run affected CPU suites, RHI contracts/documentation gates and focused device probes with build -j14.
  No automatic runtime migration, large-scene benchmark, commit or push is included without further scope.

## Independent builder check

Review identified two implementation-critical details, now resolved above: internal buffer device
addresses must remain available for descriptor encoding, and native barriers are valid first-use
loading triggers. With those decisions recorded, the reviewer reported no further important open
questions. Both are proposed engineering choices for the user's design review.
