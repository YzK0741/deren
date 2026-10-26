# The deren RHI API standard

Status: **draft v1 for RHI ABI 60 (2026-10-26)**. This document is normative for every backend DLL that
implements `deren::promise::rhi` and for every engine/application consumer of the contract. When this
document and the contract source disagree, the source is authoritative and this document is the defect;
updates to the contract require an update here in the same change.

The standard does not describe the Vulkan implementation. It describes what any backend must do to serve the
contract, and what callers may rely on. "MUST", "MUST NOT", "SHOULD", "SHOULD NOT" and "MAY" are used in the
RFC 2119 sense.

## 1. Scope and source of truth

- The contract is the exported modules under `source/promise/rhi/`: `rhi.contract.cppm` (version, enums, value
  types, object handles), `rhi.api_core.cppm` (interfaces and virtuals), `rhi.extension.cppm` (optional
  extensions), `rhi.core_desc.cppm` (creation parameters).
- Every interface method listed in sections 7-8 is a virtual function that a backend implements. Default
  implementations return `error::unsupported` (or perform the documented trivial operation); a backend MUST NOT
  claim an ability it does not implement.
- The current ABI is `rhi::abi_version == 60`. A backend is loaded by the application as a DLL through
  `deren_make_api_core(abi_version, create_info, error_info)`; the first argument is the caller's ABI and a
  mismatch is refused by name.
- The contract carries no Vulkan/API-specific types except through optional escape extensions. Backend modules
  MUST not be imported by the engine/application: the boundary is a run-time DLL seam.

## 2. Object and ownership model

- Every contract handle derives from `rhi::object` and carries one public `type` tag (a concrete name). A
  caller MUST check/validate the tag before casting; the backend validates it before touching implementation
  state.
- Handles are reference-counted views on backend-owned objects. `release()` drops the caller's ONE reference;
  the object dies with its last reference. A backend MAY keep the underlying object alive for shared/cached
  content (e.g. content-deduplicated images) after the last contract reference dies, but MUST NOT expose a dead
  handle. After `release()` the caller MUST NOT touch the handle. A borrowed view carries no caller-owned
  reference: calling its underlying `release()` is an ownership precondition violation, and a backend MAY
  terminate on it. Callers MUST NOT rely on a log-only or no-op response.
- Factory methods return `unique_object<T>` unless documented otherwise; `unique_object` owns one reference and
  releases it. Copies of an owning `shared_object<T>` share its control block; constructing one by moving a
  `unique_object<T>` transfers that factory reference. `share()` and `get_image()` provide owning references
  for the handle kinds that support them without consuming the source owner. `borrow()` is non-owning and
  MUST NOT be used where a retaining owner is required (the group creation/loading paths reject borrowed
  members by name).
- The backend core and its DLL MUST outlive every handle, group, command buffer, pending submission and
  completion token it produced. A caller that drops the core while a handle/token is alive violates the
  contract; backends need not defend against it beyond the documented `self_owner`/core lifetime rules.
- Ownership of GPU work follows the recording/retention rules on each verb. Directly bound pipelines and
  borrowed buffer/image operands MUST be kept alive by the caller through pending CPU recording and GPU
  completion; naming them in a command does not transfer ownership. Resource/texture group loads retain the
  group and its owned members through recorded and pending GPU use; push bytes are copied at record time;
  submitted command buffers and merged segments are retained through completion. These specific retention
  guarantees MUST NOT be generalized to every resource named by a recording.

## 3. Errors, validation and optional capabilities

- Fallible verbs use `rhi::error`, `std::expected<T, rhi::error>` or their documented empty-handle channel;
  value-only recording verbs latch failures as specified below. Backends MUST NOT throw, allocate a failure
  path that can throw, use RTTI, or abort on recoverable caller validation errors. Ownership
  precondition violations (such as releasing a borrowed frame view) are outside that recoverable-error
  guarantee. A fatal device/backend condition is `device_lost`/`out_of_*_memory` as appropriate and MUST NOT
  be reported as success.
- Validation MUST establish structure coverage before reading omitted fields and establish handle provenance
  before accessing backend-specific object state. No universal error priority is imposed when several
  preconditions fail: unless a verb specifies an order, a backend MAY check recording/frame state before tags
  or ranges, and callers MUST NOT depend on which failing precondition is reported first. For example, a
  binding call outside recording may return `not_ready` even when its handle is foreign. Failed validation
  emits no native work for the rejected operation; commands from an earlier successful application are not
  rolled back. Deferred application follows the whole-batch rule below.
- Error meanings: `invalid_argument` = caller error, no native work; `not_ready` = no frame/recording or the
  handle no longer holds a reference; `unsupported` = a capability/format/mechanism this backend does not
  serve, refused by name rather than dropped; `operation_failed` = backend failure without a more precise
  channel; `device_lost`/`out_of_device_memory`/`out_of_host_memory` are device conditions; `timeout` and
  `out_of_date` are retryable; `initialization_failed` is creation failure.
- A deferred-recording backend MUST validate the complete pending batch before replay; a deferred error latches
  the recording as non-executable and MUST NOT submit a partial recording.
- Abilities and extensions are the only way to ask "can you do X". `api_core::abilities()` reports bits;
  `query_extension<T>(kind)` returns `nullptr` for an unimplemented extension, which means `unsupported` to the
  caller, never a silent downgrade. A backend MUST NOT claim a bit it cannot serve. The optional
  `descriptor_space` extension is answered only by a backend with a descriptor-heap-like model; its absence is
  a named failure for callers that need it.
- Capability numbers are facts reported through `device_capabilities` (limits, formats, queue families, push
  capacity, shader binding table properties). Callers MUST query them; backends MUST NOT write them into the
  contract as a second source of truth.

### 3.1 Recoverable refusal and fatal backend failure

No-exception operation does not promise recovery from every failure. For recoverable caller validation and
unsupported mechanisms, use the verb's existing error channel (or an empty factory handle); do not terminate.
A returned error MUST NOT be mistaken for success, and a failed recording MUST NOT become executable.

| Condition | Required handling / current scope |
| --- | --- |
| Invalid descriptor, unsupported capability, foreign operand, invalid recording state | Refuse through the documented return channel; ownership precondition violations remain separate |
| Device error on a method returning `error` / `expected` | Preserve the named error when the path reports it; no successful result or completion receipt |
| Factory with only an owning-handle return | Empty means refusal; no precise cause can be inferred from emptiness |
| Fatal host allocation failure or unrecoverable backend initialization failure | May terminate through the project's panic policy; no portable recoverable-OOM guarantee is added |

Current Vulkan limitation: native buffer allocation failure returns an empty allocation, but later upload or
mapping failure in VMA may terminate; native command-pool/command-buffer setup also has fatal paths. These
are implementation limits, not additional caller validation rules or evidence that all device errors are
recoverable. A factory's empty-result clause describes a returned refusal, not a guarantee that every fatal
failure returns. Changing these paths to recoverable results requires explicit cleanup/error propagation and
fault-injection tests. This standard does not silently reclassify ordinary validation errors as fatal.

## 4. Recording, frames and synchronization

- A recording object has one owner thread at a time. The contract does not promise concurrent calls on the same
  command buffer/recorder; callers serialize them. Separate command buffers MAY record concurrently when the
  backend documents that. The backend owns the frame slots and synchronization; a caller MUST NOT submit while
  a frame is already in flight unless the backend's frame model allows it (the current engine model uses
  `frames->frame_begin()` -> `frames->begin_commands()` -> record -> `end_recording` -> `frames->submit()`).
- `begin_recording` opens a session; `end_recording` closes it. `apply_operations` applies pending operations
  and preserves logical state for later recording; `end_recording` applies automatically. Immediate/native
  interop boundaries (`prepare_native`/`finish_native`, `execute`, scoped synchronization, radix sorting, frame
  begin/end) flush pending operations before the native work and restore the documented logical state for later
  consumers. A backend that cannot defer returns `unsupported` from `apply_operations` and applies immediately.
- `submit`/`execute` MUST retain command buffers, merged segments and explicitly retained group owners until
  GPU completion, with borrowed resource operands remaining the caller's responsibility. They MUST NOT publish
  a partial batch after a validation/flush failure. Presentation is a frame-context operation; readback/shared ownership
  follows the handle-specific rules. `wait_idle` waits for all submitted work and is the caller's ordering
  tool when no finer synchronization is exposed.
- Barriers/copies/timestamps/builds are effectful operations: they MUST be executed in recorded order and MUST
  NOT be removed, reordered across an effectful boundary, or fused speculatively. Deferred optimization may
  remove only redundant state operations whose values are provably dead before their next consumer, and must
  preserve every effect operation and the final observable state.

## 4.1 Resource creation and mapped updates

`buffer_usage` describes memory/access intent; `buffer_flags` adds operation capabilities. A flag does not
change the host-access promise of the intent: requesting `storage` or `transfer_source` does not turn a
GPU-only allocation into a host-visible one. Conversely, a nonempty mapped span does not authorize an
operation whose required usage/capability was not requested. Backends may provide extra native capabilities,
but portable callers MUST NOT depend on such extras.

| Buffer intent | Host access after successful creation | Intended use |
| --- | --- | --- |
| `vertex`, `index`, `uniform_gpu_only` | No portable mapping guarantee | GPU consumers; initial upload where supported |
| `uniform_coherent` | Direct coherent mapping | CPU-updated shader parameters |
| `storage_coherent` | Direct coherent mapping | CPU-written tables also used as GPU storage |
| `uniform_cached` | Direct coherent mapping; cached memory preferred | Host reads; caching is a placement preference, not a correctness condition |
| `readback_coherent` | Direct coherent mapping | Destination of GPU-to-CPU transfers |
| `staging_source` | Direct coherent mapping | CPU-written source of recorded GPU transfers |
| `storage_gpu_only`, AS storage/scratch | No portable mapping guarantee | GPU-produced data and build storage |

For the host-mapped intents above, successful creation MUST provide storage usable without explicit host
cache flush/invalidate calls. ABI 54 exposes no such calls. A backend unable to supply that property MUST
refuse creation; it MUST NOT hand out a mapping that silently requires unexposed cache maintenance. The
Vulkan backend requires HOST_VISIBLE and HOST_COHERENT; HOST_CACHED is preferred for `uniform_cached`.
Internal upload staging is not a public mapping and may use non-coherent memory with backend-managed flushes.

Creation has the following boundaries:

- The factory consumes initial byte storage before returning; the caller may then reuse that input storage.
  Nonempty buffer initialization must equal `size`; allocation-only contents remain undefined (section 7.3).
- A successful factory call supplies a usable resource handle, not proof that unrelated GPU work has completed.
  Callers still establish the appropriate use/layout and dependencies for their first consumer.
- `unique_object` owns a handle reference; it does not promise exclusive physical memory. The backend may reuse
  content-keyed storage where the resource contract allows it. Allocation-only buffers are not content-keyed.
- A mapped span borrows storage from its buffer; it does not extend ownership. Frame readback spans additionally
  obey the frame-view window. After owner release or expiry of that window, retaining the span is invalid.
- `mapped()` does not wait, flush deferred recording, insert a resource barrier, or claim a range for the CPU.
  An empty answer is not a request to retry until the GPU finishes: it means there is no available mapping.

**CPU writes:** finish writes before submitting GPU readers. Before overwriting bytes already named by pending
work, establish completion of every use of that range; coherence does not prevent a CPU/GPU race. A mapped
upload buffer remains alive until the GPU copy completes, even though the CPU finished filling it earlier.
Different frame slots or separately owned staging ranges can avoid waiting; a frame index alone is not proof
that a range is reusable.

**CPU reads:** publish GPU writes for `host_read` using the appropriate barrier, then establish completion of
the submission that contains those writes (a matching completion token, the frame completion protocol, or
`wait_idle`). Only then read the mapped bytes. A barrier without completion and completion without the required
visibility dependency are different incomplete steps. Caller synchronization also governs CPU threads reading
or modifying the same span.

Example: create a `staging_source`, fill its mapping, record `copy_buffer` into a buffer with transfer-destination
capability, and publish the destination for its next consumer. For readback, copy into `readback_coherent`,
record transfer-destination -> host-read, submit, wait for that submission, then read. The token tracks this
work; it does not own either buffer or publish a resource transition on the caller's behalf.

Image extent/format/subresource and view-role validation follow the image clauses in section 7. A view is a
selector of existing storage, not a separate upload. Host image copy is optional and has its own format/layout
requirements; buffer mapping guarantees MUST NOT be generalized to images or to presentation images.

## 4.2 Submission acceptance, completion and reuse

| Observation | What it establishes | What it does not establish |
| --- | --- | --- |
| `end_recording() == ok` | A finished executable recording | Queued or completed GPU work |
| Submission succeeds | Work accepted with its explicit retention guarantees | GPU completion, display presentation, or automatic resource barriers |
| `poll(token) == not_ready` | That issued submission is pending | Failure or permission to reuse its resources |
| `wait(token, timeout) == timeout` | Completion was not established within this wait | Cancellation or permission to reset/release borrowed operands |
| `poll` / `wait` returns `ok` | Completion of the work covered by that token | Completion of unrelated later work or presentation |
| Async presentation future completes | The host presentation call returned with its reported result | GPU/display idle |
| `wait_idle()` returns normally | All submitted device work has drained | Submission/application of unsubmitted recorder operations |

A `submission_token` is a copyable, non-owning receipt for one successful independent graphics submission.
Its `device_id` and `sequence` form an issued pair; callers MUST NOT fabricate a token from counters or use
one with another core. Zero, foreign-device and never-issued future values are invalid. Tokens are not
persistent across process runs. Keep the originating core/DLL alive while using one; destroying the receipt
neither cancels work nor releases the backend's pending-work retention.

A timed-out wait is retryable with the same token. `timeout_ns == 0` is a nonblocking completion check with
`timeout` for pending work; `poll` instead reports `not_ready`. `UINT64_MAX` requests an indefinite wait.
A wait MUST NOT hold queue/submission locks while blocking, and waiting for an older completed submission
MUST NOT wait for later work. Completed receipts remain queryable after native per-submission storage is
collected, while the originating core remains alive. Device failures remain failures, never completion.

The extension does not submit/present an acquired frame; frame work follows `frame_context`/frame-walker
protocols. Ordinary `command_context::submit` supplies the same work-retention rules without a receipt.
Before reusing or destroying a borrowed operand, the caller must account for ALL recordings/submissions
that use it, not just the first completed token. `simultaneous_use` is an execution permission, not permission
to record/reset an in-flight command buffer or modify its borrowed data concurrently.

## 4.3 Binding, capabilities and synchronization boundaries

- Loading a resource group retains that immutable group; it does not imply visibility of writes to its members.
  Pipeline/group compatibility and reserved push ranges are checked as specified in section 7. Push bytes are
  copied when accepted, so later changes to the source bytes do not change the queued value.
- Rebinding the same pipeline preserves a compatible group. Switching pipelines retains logical group state,
  with compatibility checked before the next consumer. Native interop restores logical state through the
  boundary protocol in section 7.5; raw native state is not an independent portable state model.
- Capability presence means a mechanism exists, not that every descriptor, range or format is supported.
  Query limits and extension properties, then supply valid operands. `unsupported` identifies an unavailable
  mechanism/shape; `invalid_argument` identifies a bad operand where the verb defines that error;
  `not_ready` identifies a missing execution/recording precondition. Section 3 defines validation priority.
- A factory returning only an owning handle reports refusal as empty; callers MUST NOT infer a precise
  `error` code from emptiness. This standard does not add an unknown-enum/unknown-flag validation policy or a
  new out-of-memory result channel to existing factories.
- Submission order, resource visibility, host-cache coherence and object ownership are separate obligations.
  Barrier/execute/batch submission MUST NOT be read as a blanket guarantee for the other three.

## 5. ABI and compatibility

- Changing an existing interface's vtable (including adding, removing or reordering virtual functions), a
  by-value ABI layout or the entry-point signature bumps `abi_version`. Appending an enum value or adding a
  separate interface does not by itself bump it; changing an existing query interface to expose that interface
  would. Existing enum values, including retired values, MUST NOT be renumbered or reused. A backend compiled
  against a different ABI MUST be refused by the loader, not coerced; a bump permits a deliberate vtable
  revision and does not preserve compatibility with earlier slots.
- Enum values, structure `struct_size` and the `pNext` rules are ABI. A caller compiled against an older
  structure may omit trailing fields; the callee checks `struct_size`/field coverage before reading a field.
  Contract defaults are spelled in the header so both sides agree.
- Backend and executable/plugins are rebuilt together for an ABI change. The DLL name and entry point are the
  stable loader contract; the application resolves the backend by name and never imports backend symbols.
- ABI 54 removes the incomplete query/swapchain factory slots and unused mesh extension from ABI 53; all
  components must be rebuilt together. The standard is written for ABI 54. A future ABI change must add a revision note here and update the affected
  virtual clauses; the conformance checklist in section 6 must be rerun.

### 5.1 Supported build compatibility

This is a project-internal C++ module/plugin ABI for components built with a compatible, coordinated toolchain,
not an arbitrary-compiler C SDK. A common `abi_version` is necessary but not sufficient for compatibility.
The executable, backend and GUI/plugins MUST use compatible target architecture/data model, compiler C++ ABI,
standard-library ABI, runtime-library configuration, packing/alignment and relevant ABI-affecting flags.
Changing any of these requires rebuilding the coordinated component set, even if interface source is unchanged.
A Debug/Release label alone neither proves nor disproves compatibility; the actual ABI-affecting configuration
must match. The supported baseline is the project's configured build, not mixing MSVC and MinGW DLLs.

The `extern "C"` entry point fixes its symbol/linkage; its parameters and return still lead into C++ types,
virtual interfaces and shared ownership. It does not erase those compatibility requirements. The version
handshake cannot detect every incompatible toolchain; callers must not load an arbitrary DLL to test this.

When adding extensions, audit exported value-return helpers and span element layouts as well as vtables.
For example, `all_extension_kinds()` returns a fixed-size array and `resource_group_entry` has no per-element
size guard. Existing by-value layouts cannot be enlarged solely because the new feature is optional.

## 6. Conformance and verification

A backend is conformant when:
1. It exports `deren_make_api_core` and refuses an ABI mismatch without touching device state.
2. It implements every mandatory interface below and returns `nullptr` for every extension it does not
   implement; all implemented extension methods satisfy their clauses.
3. It passes the ABI/ownership tests (including `test_rhi_ownership`, `test_dynamic_link`) and the device probes
   relevant to its implemented features.
4. It never leaks a failed recording: a validation/flush failure is returned and no partial submission is
   published.
5. It releases native resources at last-reference destruction under the caller's precondition that the core
   and DLL outlive all handles and retained work.
6. It reports capabilities/extensions truthfully and refuses unsupported operations by name.

The concrete virtual-function clauses follow. The resource/command/core interfaces are in section 7 and the
optional extensions in section 8. The Lead's conformance checklist and the open questions are in section 9.



## 6.1 ABI 54 clarifications (normative)

The contract headers carry the following ABI 54 clarifications; the corresponding virtual clauses in sections 7-8
are read in their light (a clause that says "contract does not specify" is superseded where this list or the
header block specifies it):

- **Removed in ABI 54:** incomplete query and independent swapchain factories are absent; the unused mesh
  extension is retired. The frame swapchain and tier-1 mesh drawing remain available.
- **Release/ownership:** one reference per release; last reference destroys; shared/content-keyed resources may
  outlive the handle internally; backend core outlives handles/groups/pending work.
- **Recording:** a handle-taking verb answers `error`, a value-only verb answers `void` and latches failure;
  `end_recording`/`apply_operations` report it and make the recording non-executable; one recorder is
  caller-serialized.
- **Command data:** `copy_buffer` MUST reject size 0/overruns; `bind_vertex_buffer`/`bind_index_buffer` MUST
  reject an offset past the buffer; `begin_rendering`/`end_rendering` MUST pair and MUST NOT nest; an open
  scope fails the recording; `clear_color_image` is RGBA; `mark_gpu_timing` labels are borrowed process-lifetime
  text.
- **Core/frame:** `wait_idle` waits without an error channel and is legal outside recording; `main_context` may
  be null and is core-owned; frame views are valid only in their acquire/submit windows.
- **Groups/radix:** `assign_texture_group` success yields one owner and `*result==ok`, failure yields empty and a
  named error; radix requirements/record are as stated in the header block (scratch bytes/alignment/flags,
  range/foreign validation, preservation of logical recorder state across internal native binding changes).
- **Extensions:** null extension means `unsupported`; a non-null extension implies its ability bit; ability
  default bodies are not capability claims; descriptor-heap writes require caller retention and stride-relative
  offsets; descriptor_space is heap-gated and sampled-only; host copy is color-only/tightly packed/GENERAL;
  shader-group records use handle_size/handle_alignment; scoped sync depth/stencil-independent transitions are
  `unsupported`; retain_image is one-strong-reference/epoch-bound; async presentation allows one outstandingcall
  (`not_ready` for a second); `compute_indirect` and `buffer_int64_atomics` are separate questions.


## 6.2 Recommended ownership and sharing (non-normative guidance)

Sections 7-8 are normative. This section is the recommended style for callers choosing a handle type; it does
not add requirements beyond those clauses, but following it keeps lifetime reasoning local.

### 6.2.1 Default choice per object

| Object / factory product | Recommended default | Share only when | Notes |
| --- | --- | --- | --- |
| `buffer`, `acceleration_structure`, `micromap` | `unique_object` owned by the pass/primitive that created it | a second long-lived owner genuinely needs it, or it must outlive the creating pass | Prefer one owner and short lifetimes; do not share merely to avoid a copy of the handle. |
| `image` | `unique_object` for pass-owned targets | multiple passes/frames keep it alive, or it is a cache/streaming resource | `image::share()` copies an owning reference without consuming the factory reference; repeated calls use one control block. Store the shared owner, not a raw pointer. |
| `image_view` | `unique_object` | a view is handed to another owner that outlives the creating scope | A view retains its parent image; sharing the view is usually the honest unit when the parent must live on. |
| `sampler`, `shader`, `pipeline` | `unique_object` in the owning registry/cache | a shared renderer state really outlives the registry entry | Pipelines are cheap handles on backend objects but still have a native lifetime; do not copy them as raw values. |
| `texture_group` / `resource_group` | the `*_ref` (`shared_object`) returned by the factory | always, while recorded/pending commands can use it | These are shared-by-design handles. `borrow()` members are rejected where a retaining owner is required; group members are owned by the group. |
| `command_buffer` (from `create_command_buffer`) | `unique_object` for a one-shot recording | it must be shared with another owner before submission | A submitted buffer is retained by the backend until completion; do not keep a second independent owner to extend the GPU window. |
| `command_buffer` (from `make_command_buffer`) / `mergeable_command_buffer` | `shared_object` / `*_ref` | while recorded batches are pending | Frame primaries are borrowed views, not factory products. |
| `swapchain` | borrowed frame view | never own/release/delete | Use `frame_swapchain()` within the core/frame protocol. |
| frame views (`begin_commands()`, `frame_image()`, `frame_readback_buffer()`, `frame_swapchain()`) | **borrow only** | never share or release | Valid only in the frame/acquire window; do not store them past submit/acquire. |
| extensions (`query_extension<T>()`), `main_context()`, walkers/profilers/surface | **borrow only** | never own/release/delete | Borrowed from the core; keep the core alive. Re-query after a core change rather than caching across a backend rebuild. |
| `submission_token` | copyable receipt | copy freely; it owns no resource | Dropping/copying a token does not cancel work; the core must outlive its use. |
| `retain_image()` (frame resources) | `shared_object` | hand to another frame-scoped owner | One strong reference; valid only within the acquire epoch, empty outside a frame. |

### 6.2.2 The three handle kinds

- **`unique_object<T>`**: move-only, owns one reference, releases exactly once at destruction. Use it for
  factory products that have one clear owner. Do not copy it, do not call `release()` on the raw handle as well
  (that would double-drop the reference), and do not construct a second `unique_object` from the same pointer.
- **`shared_object<T>`**: copyable, owns one reference through a shared control block. Create it only through the
  contract (`share()`, `get_image()`, `make_command_buffer()`, `retain_image()`, `*_ref` factories), or by moving
  a `unique_object` into it to transfer an owned reference. Never wrap another owner's `get()` or a borrowed
  pointer in an independent owning control block. Use it when lifetime genuinely crosses the creating
  scope; a `unique_object` is the better default.
- **`borrow()`**: a non-owning view. Use it only when the owner is provably alive for the whole use, and never
  where the contract requires retention (group membership, pending commands, frame resources). Borrowed
  wrapper destruction/reset drops no RHI reference. This does not authorize calling `release()` through the
  underlying pointer; that is an ownership precondition violation and may terminate. A borrowed handle is
  allocation-free: it stores only the pointer (no control block), and copies stay non-owning.

### 6.2.3 Recommended lifetimes

- **Per-frame resources** (pass targets, staging/readback, one-shot command buffers): create as `unique_object`,
  record/use, then let the container drop after GPU completion. Do not extend a pass by sharing its target unless
  a later pass genuinely reads it. Keep the creating owner alive through GPU completion for directly borrowed
  operands; rely on backend retention only where the particular verb explicitly guarantees it.
- **Cross-frame resources** (cached images, materials, persistent scene buffers, pipelines): store a
  `shared_object` (or the owning registry) for the whole cache lifetime. The core/backend must outlive every
  cached handle; release the cache before `api_core::release()`.
- **Groups**: create once, keep the `*_ref` in shared state, load it before the first consuming draw/dispatch,
  and let recorded/pending work retain it. Do not reload the group to work around a pipeline switch in ABI 54;
  the backend retains and republishes compatible logical state.
- **Frame boundaries**: keep the frame's borrowed primary/image/readback inside the frame; submit before
  reacquiring. A `shared_object` obtained from `retain_image()` must be dropped or used before the next acquire.
- **Backends**: internal sharing (content-keyed images, cached pipelines, group tables) is allowed, but it is a
  backend implementation detail. It never changes the reference the caller owns or the lifetime rule above.

### 6.2.4 Anti-patterns

- Wrapping a raw `image*`/`buffer*` in a second `shared_object` (two control blocks, double destruction).
- Calling `release()` on a handle held by `unique_object`/`shared_object`, or `delete` on any contract handle.
- Storing `begin_commands()`/`frame_image()`/`retain_image()` past the frame/epoch window.
- Using `borrow()` for group members or for anything that must survive into pending GPU work.
- Releasing the sole `unique_object` owner of a directly bound pipeline or borrowed resource before GPU
  completion. Keeping that owner alive is sufficient; use shared ownership only when multiple owners need it.
- Releasing the backend core while a token, group, recorded buffer or shared image owner is alive.

## 6.3 Resource and synchronization verification map

This table locates evidence; passing one row does not prove every backend rule. Device-only probes MUST be
run with `--with-device`; their headless skip result is not device conformance evidence.

| Contract area | Headless checks | Device evidence |
| --- | --- | --- |
| Exact buffer initialization / allocation-only | Shared contract checks in `test_runtime_injection` | Same checks in `test_resource_group`, including upload-capable allocation-only paths |
| Mapped memory selection | `test_vulkan_capabilities` compiles actual allocation-policy bodies | `test_submission_completion` mutates mappings after creation and reads back copied bytes after a token wait |
| Engine buffer batches (not a new RHI guarantee) | `test_resource_cluster` ownership/capability refusal | `test_buffer_transfer --with-device` upload/readback, timeout and pending ownership |
| Pending / timeout / reuse | ABI/value and basic state tests only | `test_submission_completion` blocks the queue, tests retryable timeout, pending reuse refusal and completion |
| Token provenance and persistence within a core | Value-type/ABI checks only | Invalid/future/foreign/core-recreation receipts and repeated queries after collection in `test_submission_completion` |
| Retained groups / copied push / native state restore | Ownership and render-operation tests | `test_resource_group` verifies GPU readback after a native push clobber and compatible pipeline switch |
| Frame / presentation consumers | Render-graph/probe state tests | `test_render_graph_frame`, `test_gaussian_gpu_frame` |

Allocation-policy checks prove requested requirements, not the behavior of every driver or memory topology.
The device readback does not emulate a non-coherent device, exhaust memory types, test arbitrary CPU data
races, or prove display completion. CPU probe backends are ABI/state doubles, not full native-state models.

## 7. Core interfaces

Status: draft for review (the core-interface half of the standard).

Scope: every interface a backend must implement to be a conforming RHI core --
resources and handles (section 1), recording (section 2), and the
factories/contexts/root (section 3). Optional tier-2 extensions are out of scope
here.

Source of truth: `source/promise/rhi/rhi.api_core.cppm` (the contract), with
`docs/dynamic_runtime.md`, `docs/resource_groups.md` and
`docs/superpowers/specs/2026-10-24-render-operations-design.md` where they state
contract-level rules. Where a behaviour is stated only by one backend, the
bullet says so and treats it as evidence, never as the rule.

Notation: `MUST` / `MUST NOT` / `SHOULD` are normative. `contract does not
specify` means the contract is silent, so a second backend may choose and a
caller may not depend on either answer. "one reference" and "release()" are the
contract's ownership verb and are defined once, in `buffer::release()`.

Coverage: 21 interfaces, 124 virtuals.

---

### 7.1 Resource and handle interfaces

The interfaces of this section are OWNED handles: a factory (or `frame_image()`,
for the frame's own image) hands out one reference, `release()` drops it inside
the backend, and `rhi::unique_object<T>` / `rhi::shared_object<T>` are the owner
spellings. Release is not necessarily destruction -- a backend MAY serve the
same content to several callers -- so an object dies with its LAST reference and
a caller may not read `release()` as "the memory is free now". Exception, stated
at each site: the frame's image, frame readback buffer and frame command buffer
are BORROWED views that hold no reference at all and must never be managed.

#### micromap

An opacity micromap: the micro-triangle opacity record a bottom-level
acceleration structure's geometry consults during traversal, so that an opaque
micro-triangle is committed without any-hit work, a transparent one is skipped,
and an unknown one invokes the any-hit shader. Tier-1, like the acceleration
structure it attaches to; created only through
`resource_factory::create_micromap` and recorded only through
`command_recorder::build_micromap`. The backend owns the setup memory (the
attributes, the per-triangle records and the index array the build reads) as
well as the structure's storage and its alignment, so a caller holding the
handle cannot get the alignment or the index array wrong. It is never destroyed
directly: `release()` is the only drop verb.

- `virtual ~micromap() noexcept = default;` -- **Requirement:** destruction runs inside the backend, through the backend's own `operator delete`; the contract exposes no caller-side `delete` path for a handle. **Returns/errors:** none (destructor, `noexcept`). **Ownership/lifetime:** reached at the LAST reference only; wrapping a handle in a second owner is forbidden by the ownership note, not by the type. **Threading:** contract does not specify destruction timing relative to recorded or pending work that names the micromap; do not assume any.

- `virtual void release() noexcept = 0;` -- **Requirement:** drop the caller's ONE reference; the backend destroys the structure and gives its memory back. **Returns/errors:** none (`void`, `noexcept`); no error is reported for a structure that is already gone. **Ownership/lifetime:** do not touch the handle afterwards; the micromap may outlive the call only if some other reference exists (shared ownership is permitted). **Threading:** contract does not specify; a release concurrent with a recorded build, or with GPU work that traverses the geometry consulting it, is not covered by any stated guarantee.

#### acceleration_structure

A top-level or bottom-level acceleration structure. Tier-1 rather than an
ability because it is a RESOURCE the caller creates, reads an address from and
destroys, exactly as a buffer is; the capability question ("can this device run
ray queries / a ray-tracing pipeline") is asked through `device_capabilities`,
not here. The backend owns the structure's storage, the scratch a build needs,
its alignment and the per-geometry offsets -- a caller that never sees them
cannot depend on them, which is what lets a backend without explicit
acceleration structures answer the creation and build verbs however it must. One
reference, dropped through `release()`.

- `virtual ~acceleration_structure() noexcept = default;` -- **Requirement:** destruction is the backend's, at the last reference; no caller-side `delete` path exists in the contract. **Returns/errors:** none (`noexcept`). **Ownership/lifetime:** a structure still named by a recorded build or an instance record must not die before that GPU work completes; the contract states the ordering obligation, not a fence. **Threading:** contract does not specify.

- `virtual void release() noexcept = 0;` -- **Requirement:** drop the caller's one reference (see `buffer::release()`); the backend frees the structure and its memory. **Returns/errors:** none. **Ownership/lifetime:** the handle is dead to the caller after the call; a structure shared with another holder survives until the last reference. **Threading:** contract does not specify.

- `[[nodiscard]] virtual std::uint64_t device_address() const noexcept = 0;` -- **Requirement:** answer THE address a shader, or an instance record's `structure_reference`, uses to reach the structure. A backend MUST NOT answer zero for a TOP-LEVEL structure it successfully created, because an address is exactly what an instance stores; zero otherwise means "the backend could not report one". **Returns/errors:** a raw address, no error channel. **Ownership/lifetime:** a plain value; the address is valid while the structure lives and MUST NOT be cached past its release. **Threading:** contract does not specify; the call is `const` but no lock or thread-safety claim is made.

- `[[nodiscard]] virtual std::uint64_t size_bytes() const noexcept = 0;` -- **Requirement:** answer THE bytes the structure occupies, which a caller needs to describe the structure to something carrying address ranges (the descriptor heap's address-range descriptor is the measured case). **Returns/errors:** a raw byte count; zero means the backend did not answer -- the contract does not make zero an error. **Ownership/lifetime:** a plain value tied to the structure's lifetime. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error write_instances(std::span<acceleration_structure_instance const> instances) = 0;` -- **Requirement:** TOP LEVEL ONLY: replace the instance list the NEXT build reads. Host work on memory the BACKEND owns, with contract-layout records, so the caller of a top-level structure never allocates the array the traversal reads. **Returns/errors:** `ok`; `invalid_argument` when the structure is a bottom-level one or the list is longer than the `instance_capacity` it was created with (refused by name, never truncated). **Ownership/lifetime:** the span is borrowed for the call; the stored instances belong to the structure, not the caller. **Threading:** contract does not specify; the call is host work on backend memory and is not stated to be safe against a concurrent recorded build.

#### buffer

A device-local or host-visible byte range. The interface is deliberately thin:
creation carries the memory intent (`buffer_usage`), the extra capabilities
(`buffer_flag`) and the initial bytes; everything else -- memory properties,
tiling, upload strategy -- is the backend's. This interface owns the file's
canonical `release()` note, and every other owned handle refers to it.

- `virtual ~buffer() noexcept = default;` -- **Requirement:** destruction happens inside the backend at the last reference; no caller-side `delete` path exists. **Returns/errors:** none (`noexcept`). **Ownership/lifetime:** a buffer still used by recorded or pending GPU work must outlive that work; the caller's own pacing is the ordering. **Threading:** contract does not specify.

- `virtual void release() noexcept = 0;` -- **Requirement:** drop the caller's one reference, inside the backend, whatever the compiler's `delete` would have done -- and it MAY FREE NOTHING, because the resource can be shared or reused. **Returns/errors:** none for an owned reference; calling `release()` on a borrowed frame readback view violates ownership preconditions and MAY terminate. **Ownership/lifetime:** the object dies only with its LAST reference; the caller MUST NOT touch the handle afterwards, and MUST NOT read the call as "the memory is free now". **Threading:** contract does not specify; a release racing GPU work that reads the buffer is not covered by any stated guarantee.

- `[[nodiscard]] virtual std::uint64_t size() const noexcept = 0;` -- **Requirement:** answer the buffer's size in bytes. **Returns/errors:** a raw byte count, no error channel. **Ownership/lifetime:** a plain value. **Threading:** contract does not specify. NOTE: the contract does not say whether this is the requested size or the backend's allocated size; a caller must not assume the latter.

- `[[nodiscard]] virtual std::span<std::byte> mapped() noexcept = 0;` -- **Requirement:** return direct host-visible bytes, or empty when unmappable. Host-mapped intents provide the cache-maintenance-free guarantee in section 4.1. The call neither waits nor inserts a barrier. **Returns/errors:** a span, no error channel. **Ownership/lifetime:** borrowed buffer storage, additionally restricted by a frame view's lifetime where applicable. **Threading:** callers synchronize CPU access and establish GPU completion before reusing a range or reading GPU writes; see sections 4.1-4.2.

#### image

A texel resource with an extent and a format. Views are separate objects
(`make_view()`), read-back is a content call (`get_content()`), and the image
itself carries no sampler, no layout and no usage accessor. A backend is allowed
to serve identical content from one shared, content-keyed entry, so `release()`
may leave the object alive for somebody else.

- `virtual ~image() noexcept = default;` -- **Requirement:** destruction is the backend's, at the last reference. **Returns/errors:** none (`noexcept`). **Ownership/lifetime:** an image a view retains, or recorded work names, must not die before that use ends. **Threading:** contract does not specify.

- `virtual void release() noexcept = 0;` -- **Requirement:** drop one reference inside the backend; the object MAY SURVIVE because a backend is allowed to serve identical content from one shared, content-keyed entry. **Returns/errors:** none for an owned reference; calling `release()` on a borrowed frame image violates ownership preconditions and MAY terminate. **Ownership/lifetime:** do not touch the handle afterwards; the last reference is what destroys the content. **Threading:** contract does not specify.

- `[[nodiscard]] virtual image_extent extent() const noexcept = 0;` -- **Requirement:** answer the image's texel extent. **Returns/errors:** an `image_extent` by value; no error channel. **Ownership/lifetime:** a plain value. **Threading:** contract does not specify. NOTE: the contract does not state whether this is the DECLARED extent or the backend's RESOLVED one; for a format role the two can differ in the backend.

- `[[nodiscard]] virtual image_format format() const noexcept = 0;` -- **Requirement:** answer the image's format, with `image_format::unknown` meaning the backend cannot describe it. **Returns/errors:** an `image_format`; `unknown` is the honest "cannot describe", not an error. **Ownership/lifetime:** a plain value. **Threading:** contract does not specify.

- `[[nodiscard]] virtual unique_object<image_view> make_view(image_view_desc const& desc) = 0;` -- **Requirement:** the backend makes the native view; the caller only says WHICH RANGE it wants (base/count for layers and mips, plus a `view_role`). The view is a factory product released by its container; no native view crosses the contract. **Returns/errors:** one `image_view` reference, or an empty product on refusal; a descriptor asking for a range the image does not have (a layer past `array_layers`, a mip past `mip_levels`) MUST be refused with `error::invalid_argument` rather than clamped, and a role the image's format cannot serve (a depth role on a color image) is stated to be a refusal -- the contract does not name which code that role mismatch answers. **Ownership/lifetime:** the view retains a parent reference where the backend supports it (see `image_view`); the span-free descriptor is borrowed for the call. **Threading:** contract does not specify.

- `[[nodiscard]] virtual std::expected<image_content, error> get_content(image_copy_region const& region = {}) const = 0;` -- **Requirement:** read the image's CONTENT back into host memory; the mechanism is the backend's (this one copies out with no staging buffer, no copy command and no submission in the caller's path). The contract fixes the LAYOUT of the answer -- row-major, tightly packed, top-left origin, channel order from the image's format, layers/mips in the order the region asked for -- and the INVARIANT `bytes.size() == width * height * depth * bytes_per_pixel`. The default argument means the whole mip 0, every layer. **Returns/errors:** the content, or a named error and never a partial buffer; `unsupported` for an image the backend cannot copy out of (not created for host transfer, a device without the mechanism), for a format the contract cannot describe, and for the FRAME image on a surface whose swapchain usage has no host-transfer bit (a property of the SURFACE, not of the backend). **Ownership/lifetime:** the answer owns its bytes -- nothing in it points into backend memory -- and they stay valid as long as the caller's value does; callers establish completion and the required host-read visibility/layout before copying prior GPU writes; `wait_idle` is the coarse completion option. **Threading:** contract does not specify; `const` makes no thread-safety claim.

- `[[nodiscard]] virtual shared_object<image> share() {` -- **Requirement:** copy an owning reference WITHOUT consuming the caller's factory reference; while shared references exist, repeated calls use the same control block. Use this instead of constructing an independent `shared_object` from a raw image. **Returns/errors:** a `shared_object<image>`; EMPTY for borrowed images and for backends without shared ownership -- the default body returns empty. **Ownership/lifetime:** the api_core and the backend library MUST outlive all strong and weak references; an empty answer means the caller may not treat the image as shared. **Threading:** contract does not specify, beyond "repeated calls use the same control block".

#### image_view

The contract's substitute for a native view: the backend makes it, hands out one
reference, and it is dropped through `release()`. Backends that support shared
parent references retain owned images independently of the factory reference;
views of borrowed frame images retain no parent and must respect the frame's
lifetime. No native view crosses the contract.

- `virtual ~image_view() noexcept = default;` -- **Requirement:** destruction is the backend's, at the last reference. **Returns/errors:** none (`noexcept`). **Ownership/lifetime:** a view must outlive recorded work that names it as an attachment; the parent retention is dropped by `release()`, not by this destructor's caller. **Threading:** contract does not specify.

- `virtual void release() noexcept = 0;` -- **Requirement:** drop the view, INCLUDING its retained parent reference, after native view destruction -- so the parent image may die with this call. **Returns/errors:** none. **Ownership/lifetime:** do not use the view afterwards; the borrowed-view case (a view of a frame image) holds no parent and MUST respect the frame lifetime stated at `frame_image()`. **Threading:** contract does not specify.

- `[[nodiscard]] virtual shared_object<image> get_image() const noexcept {` -- **Requirement:** copy the parent owner; empty for borrowed swapchain images or for backends that do not support shared parent references. **Returns/errors:** a `shared_object<image>`, EMPTY where no parent owner exists -- never a manufactured owner. **Ownership/lifetime:** the copy is a new strong reference; the api_core and backend must outlive it. **Threading:** contract does not specify.

#### sampler

A sampler state object (address mode, LOD clamp, filters, mipmap mode, optional
depth comparison). Created through `resource_factory::create_sampler` from the
append-only `sampler_desc`; the interface itself carries only ownership.

- `virtual ~sampler() noexcept = default;` -- **Requirement:** destruction is the backend's, at the last reference. **Returns/errors:** none. **Ownership/lifetime:** a sampler a resource group names must outlive the group's recorded use. **Threading:** contract does not specify.

- `virtual void release() noexcept = 0;` -- **Requirement:** drop the caller's one reference (see `buffer::release()`). **Returns/errors:** none. **Ownership/lifetime:** the object dies with the last reference and the caller must not touch the handle afterwards; a sampler the backend deduplicates may survive. **Threading:** contract does not specify.

#### shader

A compiled SPIR-V module in a stated stage (including the five ray-tracing
stages). The stage is part of the descriptor, because a ray-tracing group table
can only be built if a caller can say "raygen". Created through
`resource_factory::create_shader`; the interface carries only ownership.

- `virtual ~shader() noexcept = default;` -- **Requirement:** destruction is the backend's, at the last reference. **Returns/errors:** none. **Ownership/lifetime:** the contract does not say whether a shader must outlive the pipelines built from it; a caller must treat the descriptor's code as consumed by creation and keep the shader while any pipeline it fed is alive. **Threading:** contract does not specify.

- `virtual void release() noexcept = 0;` -- **Requirement:** drop the caller's one reference (see `buffer::release()`). **Returns/errors:** none. **Ownership/lifetime:** last reference destroys; do not touch the handle afterwards. **Threading:** contract does not specify.

#### pipeline

A created pipeline (graphics, compute, or ray tracing, selected by the
descriptor's stage spelling). The interface carries only ownership: binding is
`command_recorder::bind_pipeline`, and the recording verbs live on the recorder,
not here.

- `virtual ~pipeline() noexcept = default;` -- **Requirement:** destruction is the backend's, at the last reference. **Returns/errors:** none. **Ownership/lifetime:** a pipeline bound into a recording is a BORROWED owner: the caller MUST keep it alive through pending recording AND GPU completion -- deferral does not turn a borrowed reference into shared ownership. **Threading:** contract does not specify.

- `virtual void release() noexcept = 0;` -- **Requirement:** drop the caller's one reference (see `buffer::release()`). **Returns/errors:** none. **Ownership/lifetime:** last reference destroys; releasing a pipeline still named by recorded or pending work violates the lifetime rule above. **Threading:** contract does not specify.

#### swapchain

The presentation surface's image chain. It is created once at startup and is
rebuilt IN PLACE -- never replaced -- so the presentation verbs are appended here
rather than to `api_core`: a rebuild is the swapchain's own behaviour, and the
caller supplies nothing, because the backend re-derives the extent from the
window it owns (the one place the current size lives). A windowless core still
supplies `extent()` and `format()` through `frame_context::frame_swapchain()`,
while `recreate()` and `present()` answer `unsupported`.

- `virtual ~swapchain() noexcept = default;` -- **Requirement:** destruction is the backend's, at the last reference; a swapchain handed out by `frame_swapchain()` is a BORROWED view and is never destroyed by a caller. **Returns/errors:** none. **Ownership/lifetime:** the frame's borrowed swapchain lives as long as the core. **Threading:** contract does not specify.

- `virtual void release() noexcept = 0;` -- **Requirement:** drop the caller's one reference (see `buffer::release()`); the frame's borrowed swapchain carries no caller-owned reference and MUST NOT be released; callers MUST NOT rely on a no-op response. **Returns/errors:** none. **Ownership/lifetime:** do not use the handle afterwards; the contract does not say whether a rebuild generation invalidates a released handle. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error recreate() = 0;` -- **Requirement:** rebuild the chain and its images for the window's CURRENT size, in place. **Returns/errors:** `ok` = a new generation was built; `not_ready` = the window reports a zero extent (minimized or not yet sized), NOTHING was built and the caller retries later -- a deferred recreate is a STATE, not a failure; real build failures are the backend's named startup-style panics (the constructor's build path rule), not error codes; `unsupported` in a windowless core. **Ownership/lifetime:** the swapchain object survives; the images behind it change generation, so per-image state the caller derived (extent, views) must be re-derived. **Threading:** contract does not specify; it is a device-wide operation and no concurrent-use rule is stated.

- `[[nodiscard]] virtual image_extent extent() const noexcept = 0;` -- **Requirement:** the extent of the swapchain's images in pixels -- what presentation shows. The caller scales it to its own render resolution; a render scale is a RENDERER decision, so the contract does not pre-multiply it. **Returns/errors:** an `image_extent`; no error channel. **Ownership/lifetime:** a plain value, valid for the current generation. **Threading:** contract does not specify.

- `[[nodiscard]] virtual image_format format() const noexcept = 0;` -- **Requirement:** the presentation format, available BEFORE acquiring an image (ABI 28). **Returns/errors:** an `image_format`; `unknown` when the backend cannot describe it. **Ownership/lifetime:** a plain value. **Threading:** contract does not specify.

#### radix_sorter

A stable ascending `uint32` key/value GPU sort, handed out by the optional
`gpu_sorting` extension. The contract text on the type states: operands are
borrowed until GPU completion; the CALLER publishes writes before `record()` and
publishes output reads afterwards. Sorting may overwrite native bindings internally, but preserves the
recorder's logical pipeline/group/push state for later RHI consumers. Native applied state is restored through
the same boundary protocol used by other native interop; no manual rebind is required solely because a sort ran.

- `virtual ~radix_sorter() noexcept = default;` -- **Requirement:** destruction is the backend's, at the last reference. **Returns/errors:** none. **Ownership/lifetime:** the interface says nothing else; a sorter must outlive a recorded sort until GPU completion. **Threading:** contract does not specify.

- `virtual void release() noexcept = 0;` -- **Requirement:** drop the caller's one reference. **Returns/errors:** none. **Ownership/lifetime:** one owner is dropped; the last reference destroys the sorter. No use is permitted afterwards. **Threading:** caller-serialized unless documented otherwise.

- `[[nodiscard]] virtual radix_sort_requirements requirements(std::uint32_t count) const noexcept = 0;` -- **Requirement:** answer the scratch bytes, the scratch buffer alignment and the required scratch flags for `count` elements; for a NONZERO count, `scratch_bytes == 0` means the backend cannot serve that count. **Returns/errors:** a value record; unsupported counts are expressed as zero scratch bytes, not as an error code. **Ownership/lifetime:** a plain value. **Threading:** contract does not specify. `buffer_alignment` is the required scratch-address alignment; the scratch buffer MUST provide `scratch_flags` and at least `scratch_bytes` at its chosen offset.

- `[[nodiscard]] virtual error record(command_recorder&, radix_sort_info const&) = 0;` -- **Requirement:** record the sort into the given recorder, reading `keys` and writing `values` at the stated offsets. **Returns/errors:** `ok`; `invalid_argument` for null/foreign operands, out-of-range offsets/sizes or insufficient scratch bytes. Unsupported counts are discovered through `requirements`; recording-state and mechanism failures are reported as non-success. **Ownership/lifetime:** every buffer in `radix_sort_info` is borrowed until GPU completion; the native sorting mechanism may clobber bindings internally, but the recorder restores its logical state for subsequent RHI consumers. **Threading:** contract does not specify; the recorder's own recording state applies.

#### texture_group

An immutable group of up to 16 sampled 2D slots, registered once and referenced
by a shared handle (`texture_group_ref`). It is the LEGACY binding entry point,
kept while resource groups migrate callers; a `texture_group` is also one member
kind of a `resource_group`. Slots are optional, so a group may be sparse; it is
loaded for a recording through `command_recorder::load_texture_group` and has a
logical position on the descriptor space. The device and the backend must outlive
all references.

- `virtual ~texture_group() noexcept = default;` -- **Requirement:** destruction is the backend's, at the last reference of the shared handle. **Returns/errors:** none. **Ownership/lifetime:** the group retains its member images; a recorded or pending load keeps the group alive. **Threading:** contract does not specify.

- `virtual void release() noexcept = 0;` -- **Requirement:** drop the caller's one reference. **Returns/errors:** none. **Ownership/lifetime:** one owner is dropped; private descriptor slots return only after the group's last owner dies, including recorded/pending retention. **Threading:** caller-serialized unless documented otherwise.

#### resource_group

An immutable, shared composition of shader resources: numbered logical bindings
whose members may each be a `texture_group`, a sampled/storage image with a view,
a uniform/storage buffer range, a sampler, or a top-level acceleration structure.
It describes BINDINGS, not memory: descriptor heaps, sets, pools and physical
allocations stay inside the backend. Composition is immutable -- member
references, kinds, ranges and views are fixed -- but it does not freeze resource
contents or nested texture-group writes, and loading inserts no barriers and
builds no top-level structure. Created through
`resource_factory::create_resource_group` (at most 32 members), loaded through
`command_recorder::load_resource_group`; the group owns its member references,
and the core/backend must outlive every group.

- `virtual ~resource_group() noexcept = default;` -- **Requirement:** destruction is the backend's, at the last reference of the shared handle. **Returns/errors:** none. **Ownership/lifetime:** recorded and pending commands retain the group, so it dies only after their completion; member resources are held by the group. **Threading:** contract does not specify.

- `virtual void release() noexcept = 0;` -- **Requirement:** drop the caller's one reference. **Returns/errors:** none. **Ownership/lifetime:** one owner is dropped; recorded/pending work retains the group and descriptor slots return after the last owner dies. **Threading:** caller-serialized unless documented otherwise.

---

### 7.2 Recording interfaces

#### command_recorder

The SHARED recording vocabulary (ABI 49): both recording owners --
`command_buffer` and `mergeable_command_buffer` -- expose these verbs directly,
so a call site is `buffer->draw(...)` with no borrowed view in between. This base
carries NO individual submission permission and NO batch execution permission;
those belong to the concrete owners. The recording lifecycle (`begin_recording`
/ `end_recording`), the recording session and the allocation behind it are the
backend's, and no allocator, pool or reset verb appears in the contract. Since
ABI 54 the recorder may also DEFER work: state setters, push parameters and group
loads can be queued as an operation stream, `apply_operations()` (or
`end_recording()`) applies them in order, and consumer errors surface there.

Two shared rules govern every verb below. The ANSWERING RULE: a verb that
receives a contract handle answers `error` -- `invalid_argument` for a handle
this backend did not hand out, `not_ready` when this list is not currently
recording, `unsupported` for a mechanism the backend cannot serve; a verb that
receives only VALUES generally answers `void`, and an invalid call within an open recording latches failure for `apply_operations`/`end_recording`.
`push_constants` is the exception on the value side because its byte range needs
explicit validation. The FAILURE RULE: a latched or deferred application failure
makes the recording NON-EXECUTABLE; `end_recording()` returns that error, cleans
up only what a safe reset needs, and never publishes the buffer for submission.

- `virtual ~command_recorder() noexcept = default;` -- **Requirement:** destruction is the backend's, at the last reference; the recording session and the allocation behind it die with it. **Returns/errors:** none (`noexcept`). **Ownership/lifetime:** no caller-side `delete` path exists; a borrowed owner (the frame's list) is never destroyed by a caller. **Threading:** contract does not specify.

- `virtual void release() noexcept = 0;` -- **Requirement:** drop the caller's one reference: the recording session and the allocation behind it die with the LAST reference, inside the backend. **Returns/errors:** none for an owned reference; calling `release()` on the frame's borrowed list violates ownership preconditions and MAY terminate. **Ownership/lifetime:** do not record into or submit through the handle afterwards; a pending recording that still names the buffer keeps it alive only if another reference exists. **Threading:** contract does not specify; no concurrent destruction or recording guarantee is added by the contract.

- `[[nodiscard]] virtual error begin_recording(command_buffer_begin_info const& info) = 0;` -- **Requirement:** open this buffer's recording session, honouring the usage flags it can serve; a usage bit or a chain entry the backend cannot serve MUST be REFUSED BY NAME, never silently dropped. **Returns/errors:** `ok` = recording; `not_ready` = this handle holds no reference any more, the backend's recording context is not available, or the session is already open; `unsupported` = a usage bit or chain entry not servable; device-level failures (`device_lost`, `out_of_*_memory`) travel as themselves. **Ownership/lifetime:** `info.next` is borrowed only until the call returns; the session stays open until `end_recording()`. **Threading:** contract does not specify; `simultaneous_use` permits overlapping submissions/executions of the recorded buffer where supported. It does not permit concurrent recording, reset or begin/end calls; callers MUST serialize recording access.

- `[[nodiscard]] virtual error end_recording() noexcept = 0;` -- **Requirement:** close the session; the buffer becomes executable by `command_context::submit()`, while a `mergeable_command_buffer` segment becomes executable through `execute(span)` instead. Pending operations are applied automatically before the native close. **Returns/errors:** `ok`; `not_ready` = no session is open; the latched or deferred application error otherwise, in which case the recording is discarded and NOT published for submission. **Ownership/lifetime:** explicitly retained groups and merged segments stay held while recorded or pending work needs them. Directly bound pipelines and borrowed buffer/image operands remain caller-owned through GPU completion; ending recording does not transfer ownership. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error apply_operations() noexcept {` -- **Requirement:** apply the operations this recorder has buffered, IN ORDER, and preserve the resulting final state for later recording. `end_recording()` applies automatically, so callers that never call this keep the previous behaviour. **Returns/errors:** `ok`, including for an EMPTY application; the deferred validation/consumer error when one was queued, and that error makes the recording non-executable. The default body answers `unsupported`, meaning "this backend cannot defer and applies immediately". **Ownership/lifetime:** a borrowed pipeline or buffer named by a queued operation MUST live through pending recording AND GPU completion. **Threading:** contract does not specify; the operation stream is per-recorder state, so concurrent recording into one recorder is not covered.

- `[[nodiscard]] virtual error use(image const& resource, image_use from, image_use to) noexcept = 0;` -- **Requirement:** declare that `resource` moves from one role to the other and let the backend record what that needs. THE PAIR, not a single use, is the information: the caller knows what it just recorded, the backend knows what the pair costs. THE CALLER OWNS THE CORRECTNESS -- a missing pair, or a reversed one, is a wrong barrier, and this is the one failure the contract cannot catch. **Returns/errors:** `ok`; `not_ready` = no frame is being recorded, or the list is not this frame's (the window `begin_commands()` answers in); `unsupported` = a role pair this backend cannot spell; `invalid_argument` = an image this backend did not hand out. It answers rather than dropping silently (`[[nodiscard]]`). **Ownership/lifetime:** the image is not retained; the frame image is borrowed and the caller must respect the frame lifetime. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error copy_image_to_buffer(buffer& destination, image const& source, image_copy_region const& region) noexcept = 0;` -- **Requirement:** record a copy of `region` of `source` into `destination` at the source's CURRENT layout (this renderer keeps every image in one general layout). The destination is a DEVICE buffer, not host memory: "record it into this frame" and "read it on the host" are two moments, and the second is `buffer::mapped()` once the frame lands. **Returns/errors:** `ok`; `not_ready` = not recording; `invalid_argument` = a handle this backend did not hand out, a destination that is not a read-back buffer, or a region that does not fit the image (this backend refuses an out-of-range mip/layer or texel window rather than clamping); `unsupported` = an image without the transfer-source capability. **Ownership/lifetime:** both resources are borrowed; the copy reads the image inside the recorded frame, so neither may be released before GPU completion. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error begin_gpu_timing() noexcept = 0;` -- **Requirement:** open the frame's timing range: reset this frame slot's queries on the recorded timeline. Call once per frame, before any mark. WHICH pipeline stage a timestamp resolves at is a measurement detail of the backend that owns the query pool, never a caller decision. **Returns/errors:** `ok`; `unsupported` = this device cannot timestamp; `not_ready` = no frame is being recorded (the same window `use()` refuses in). **Ownership/lifetime:** no resource; the range belongs to the frame slot. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error mark_gpu_timing(std::uint32_t mark_index, std::string_view stage_name) noexcept = 0;` -- **Requirement:** write one timing mark into the frame's range, named for the stage it OPENS (stage i runs from mark i to mark i + 1 and carries mark i's name). `mark_index` MUST be the marks this frame has already written: the marks are POSITIONAL, so an out-of-order index would mislabel every later interval. `stage_name` is the caller's STATIC text -- a literal outliving the frame, the same rule as a creation descriptor's window title -- stored as a view and reported verbatim. **Returns/errors:** `ok`; `invalid_argument` = an out-of-order index (refused, never accepted silently); `unsupported` = no timestamp mechanism; `not_ready` = no frame is being recorded. **Ownership/lifetime:** the backend stores the view, not a copy, so the text must live as long as the process (the backend is never unloaded). **Threading:** contract does not specify.

- `[[nodiscard]] virtual error load_resource_group(resource_group_ref const&) {` -- **Requirement:** load one immutable resource group as logical recording state; a deferred backend MAY queue its native binding. EXACT binding-number/kind match with the current pipeline is required, and the group is validated BEFORE anything native is recorded. **Returns/errors:** `ok`; `not_ready` = no recording, or no compatible pipeline bound; `invalid_argument` = a binding/kind mismatch or an invalid group; `unsupported` = a backend without the mechanism (the default body) or an unsupported layout kind (ray-tracing pipeline layouts currently answer unsupported). **Ownership/lifetime:** success retains the group for recorded work, and pending submissions preserve it through GPU completion; loading a later group cannot release resources used by earlier recorded draws. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error begin_rendering(rendering_info const& info) = 0;` -- **Requirement:** open a rendering scope describing the attachments, their load/store operations, the render area, the layer count and whether secondaries record inside it. **Returns/errors:** `ok`; `not_ready` = not recording or a rendering scope is already active; `invalid_argument` = an attachment view the backend cannot resolve (this backend refuses a null/unresolvable view). **Ownership/lifetime:** the attachment `image_view*` handles are BORROWED and MUST stay alive through GPU completion; the descriptor's spans are borrowed for the call, and the structure grows by `struct_size`. **Threading:** contract does not specify; rendering scopes MUST NOT nest; a rejected nested begin leaves the existing scope open.

- `virtual void end_rendering() noexcept = 0;` -- **Requirement:** close the rendering scope opened by `begin_rendering`, applying pending operations first. **Returns/errors:** none (`void`); a missing local scope (including an attempted close of an inherited scope) latches `invalid_argument`; an unclosed local scope fails `end_recording()`. Neither failure publishes the buffer. **Ownership/lifetime:** attachment views stop being needed for this scope, but the recorded commands still reference them until GPU completion. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error push_constants(std::uint32_t, std::span<std::byte const>) noexcept {` -- **Requirement:** record shader parameter bytes immediately (queued as logical state under ABI 54); the span is consumed BEFORE return. Require recording, nonempty four-byte units, and containment within the capacity reported by `device_capabilities::max_push_constants_size()`. Bytes MUST NOT overlap the reserved resource-group handle fields or the legacy texture-group token of the bound pipeline. A pipeline NEED NOT already be bound. **Returns/errors:** `ok`; `invalid_argument` = empty payload, a misaligned or overflowing range, or one that collides with a reserved field; `not_ready` = not recording; `unsupported` = a backend with no suitable mechanism (the default body) or zero reported capacity. **Ownership/lifetime:** the bytes are copied or recorded before return; no pointer or parameter storage is retained by the default operation. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error bind_pipeline(pipeline const& handle) = 0;` -- **Requirement:** make `handle` the pipeline subsequent commands consume; a rebind of the SAME pipeline MUST preserve a loaded compatible group, and a pipeline SWITCH retains the logical group but validates it against the new pipeline at the next consumer. **Returns/errors:** `ok`; `not_ready` = not recording, or a previously latched recording error is re-reported; `invalid_argument` = a handle this backend did not hand out (or one outside its live registry); `unsupported` = a bind point or mechanism it cannot serve. **Ownership/lifetime:** the pipeline is BORROWED, not retained: the caller MUST keep it alive through pending recording AND GPU completion. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error bind_vertex_buffer(buffer const& handle, std::uint64_t offset) = 0;` -- **Requirement:** bind a vertex buffer at a byte offset; the binding is state that a later draw consumes. **Returns/errors:** `ok`; `not_ready` = not recording; `invalid_argument` = a handle this backend did not hand out (this backend also refuses an offset at or past the buffer's size). **Ownership/lifetime:** borrowed; the caller keeps the buffer alive through GPU completion. **Threading:** caller-serialized; an out-of-range offset MUST be rejected.

- `[[nodiscard]] virtual error bind_index_buffer(buffer const& handle, std::uint64_t offset, index_type type) = 0;` -- **Requirement:** bind an index buffer at a byte offset with the width of one index (`uint16` or `uint32`); indexed draws consume it. **Returns/errors:** `ok`; `not_ready` = not recording; `invalid_argument` = a handle this backend did not hand out. **Ownership/lifetime:** borrowed; alive through GPU completion. **Threading:** caller-serialized; an out-of-range offset MUST be rejected with `invalid_argument`.

- `virtual void draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex, std::uint32_t first_instance) noexcept = 0;` -- **Requirement:** record one non-indexed draw of `instance_count` instances, consuming the currently bound pipeline, vertex binding, the visible dynamic state and any bound push/group state. **Returns/errors:** none (`void`); invalid scope or consumer state within a recording latches failure, reported at application/end; no failed recording is published. **Ownership/lifetime:** everything the draw consumes must stay alive through GPU completion (a borrowed owner, not a shared reference). **Threading:** contract does not specify.

- `virtual void draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index, std::int32_t vertex_offset, std::uint32_t first_instance) noexcept = 0;` -- **Requirement:** record one indexed draw, consuming the index binding in addition to what `draw()` consumes; `vertex_offset` is signed and added to each index. **Returns/errors:** none (`void`); the same latched-failure rule as `draw()`. **Ownership/lifetime:** as `draw()`. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error draw_indirect(buffer const& arguments, std::uint64_t offset, std::uint32_t count, std::uint32_t stride) = 0;` -- **Requirement:** record `count` draws read from records in `arguments`, `stride` bytes apart from `offset`; the record is the contract's own 16-byte `draw_indirect_command`, and the range rule the contract supplies is `validate_draw_indirect_range` (offset a multiple of 4, stride at least the record size and a multiple of 4, last record fully contained). **Returns/errors:** `ok`; `invalid_argument` = a range that cannot hold `count` records of that stride, or a handle this backend did not hand out; `not_ready` = not recording; `unsupported` = no indirect mechanism. **Ownership/lifetime:** the argument buffer is borrowed and must stay alive through GPU completion; the range is captured as recorded state. **Threading:** contract does not specify.

- `virtual void dispatch(std::uint32_t groups_x, std::uint32_t groups_y, std::uint32_t groups_z) noexcept = 0;` -- **Requirement:** record one compute dispatch of that workgroup count, consuming the bound compute pipeline and its group/push state. **Returns/errors:** none (`void`); the same latched-failure rule as `draw()`. **Ownership/lifetime:** as `draw()`; a compute pipeline must be bound for the dispatch to mean anything, and an invalid recording scope or incompatible pipeline causes a latched failure under the shared recording rule. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error dispatch_indirect(buffer const&, std::uint64_t) {` -- **Requirement:** record ONE compute dispatch read from a 12-byte argument record (`dispatch_indirect_command`) at the given offset; the contract's range rule for it is `validate_dispatch_indirect_range`. **Returns/errors:** `ok`; `invalid_argument` = a misaligned or out-of-range offset, or a handle this backend did not hand out; `not_ready` = not recording; `unsupported` = a backend without the mechanism -- the DEFAULT BODY answers `unsupported`, and unavailable backends are required to refuse explicitly. **Ownership/lifetime:** the buffer is borrowed; alive through GPU completion. **Threading:** contract does not specify.

- `virtual void draw_mesh_tasks(std::uint32_t groups_x, std::uint32_t groups_y, std::uint32_t groups_z) noexcept = 0;` -- **Requirement:** record one mesh-shader dispatch of that workgroup count, consuming a mesh pipeline. **Returns/errors:** none (`void`); the same latched-failure rule as `draw()`. **Ownership/lifetime:** as `draw()`. **Threading:** contract does not specify. NOTE: callers query `device_capabilities::mesh_shader()` before selecting this path; an unsupported invocation fails the recording rather than silently dropping work.

- `[[nodiscard]] virtual error draw_mesh_tasks_indirect(buffer const& argument_buffer, std::uint64_t offset, std::uint32_t count, std::uint32_t stride) = 0;` -- **Requirement:** record `count` mesh-task dispatches read from records `stride` bytes apart; the record is the contract's 12-byte `mesh_task_command` (NOT the 16-byte draw record -- reusing one family's minimum for another was the measured defect the parameterized helper removes), and the contract's range rule for it is `validate_mesh_task_indirect_range`. **Returns/errors:** `ok`; `invalid_argument` = a range/stride that cannot hold `count` records, or a foreign handle; `not_ready` = not recording; `unsupported` = no indirect mesh mechanism. **Ownership/lifetime:** the argument buffer is borrowed; alive through GPU completion. **Threading:** contract does not specify.

- `virtual void trace_rays(shader_binding_table_region const& raygen, shader_binding_table_region const& miss, shader_binding_table_region const& hit, shader_binding_table_region const& callable, std::uint32_t width, std::uint32_t height, std::uint32_t depth) noexcept = 0;` -- **Requirement:** record a ray-tracing launch over `width` x `height` pixels, `depth` rays deep, reading the shader binding table the CALLER built: one region per table (ray generation, miss, hit) plus the callable table, an all-zero region when there is none. The launch is a recorded command exactly like `draw`, `dispatch` and `draw_mesh_tasks` -- ordered with the rest of the frame -- not part of an announced ability. **Returns/errors:** none (`void`); a backend whose device has no ray-launch mechanism records NOTHING, and a pass that launches rays is only built when its pipeline could be created, which is where that condition is caught. **Ownership/lifetime:** the table belongs to the caller: the general-purpose buffers behind the regions MUST stay alive through GPU completion; the regions are plain values. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error build_acceleration_structure(acceleration_structure& target) = 0;` -- **Requirement:** record the BUILD of `target` -- its geometries for a bottom-level structure, or its instance list for a top-level one -- over the memory the backend owns. **Returns/errors:** `ok` = recorded; `not_ready` = this buffer is not recording; `invalid_argument` = a handle this backend did not hand out; `unsupported` = a level or shape it cannot serve, REFUSED BY NAME and never silently skipped (a missing build would produce an empty traversal, a wrong picture rather than a missing one). **Ownership/lifetime:** the structure's own memory is the backend's; the geometry buffers the descriptor named MUST stay alive through GPU completion, and the structure must outlive the recorded build. **Threading:** contract does not specify; a build must be ordered by the caller against the reads and writes it depends on.

- `[[nodiscard]] virtual error refit_acceleration_structure(acceleration_structure& target) = 0;` -- **Requirement:** REFIT `target` in place, for structures whose description declared the update flag: the addresses and counts are unchanged and only the memory they point at has been rewritten (the zero-copy shape a compute-skinning frame produces). **Returns/errors:** `ok`; `not_ready` = not recording; `invalid_argument` = a foreign handle; `unsupported` (refused by name) when the structure was not created refittable, which is exactly what declaring the flag is for. **Ownership/lifetime:** the rewritten geometry buffers must be alive and written before the recorded refit runs. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error build_micromap(micromap& target) = 0;` -- **Requirement:** record the BUILD of `target` from the attributes, per-triangle records and index array its creation descriptor carried. A MICROMAP MUST BE BUILT BEFORE THE GEOMETRY THAT CONSULTS IT; that ordering is the caller's, and the barrier the build needs is the verb's own (the backend's, as it is for an acceleration-structure build). **Returns/errors:** `ok`; `not_ready` = not recording; `invalid_argument` = a foreign handle; `unsupported` = a shape it cannot serve, refused BY NAME. **Ownership/lifetime:** the setup memory is the micromap's and the backend owns it; the micromap must outlive the recorded build and the geometry that consults it. **Threading:** contract does not specify.

- `virtual void set_viewport(viewport const& vp) noexcept = 0;` -- **Requirement:** set the dynamic viewport state subsequent draws use (position, size and the depth range). **Returns/errors:** none (`void`); no validation rule is stated. **Ownership/lifetime:** pure state, no resource. **Threading:** contract does not specify.

- `virtual void set_scissor(rect const& scissor) noexcept = 0;` -- **Requirement:** set the dynamic scissor rectangle; signed offsets are legal (a scissor may sit at a negative origin) and a zero width or height is a legitimate "record nothing" and MUST be expressible. **Returns/errors:** none (`void`). **Ownership/lifetime:** pure state. **Threading:** contract does not specify.

- `virtual void set_cull_mode(cull_mode mode) noexcept = 0;` -- **Requirement:** set the dynamic cull mode (`none`, `front`, `back`, `front_and_back`). **Returns/errors:** none (`void`). **Ownership/lifetime:** pure state. **Threading:** contract does not specify.

- `virtual void set_depth_write(bool enable) noexcept = 0;` -- **Requirement:** set the dynamic depth-write enable. **Returns/errors:** none (`void`). **Ownership/lifetime:** pure state. **Threading:** contract does not specify.

- `virtual void set_depth_bias(float constant_factor, float slope_factor, float clamp) noexcept = 0;` -- **Requirement:** set the dynamic depth-bias factors and the clamp value. **Returns/errors:** none (`void`). **Ownership/lifetime:** pure state. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error barrier(barrier_group const& group) = 0;` -- **Requirement:** record a batch of transitions together -- image barriers, buffer barriers, an optional global memory barrier and acceleration-structure barriers -- as one dependency operation, with the batch's `stage_hint` applying to the images' shader side and to the acceleration-structure barriers. A batch with none of them is a NO-OP the backend answers `ok` for. The structure grows by `struct_size`, and a field is read only as far as the caller declared. **Returns/errors:** `ok`, including for a batch with no barriers at all; `invalid_argument` = an image, buffer or acceleration-structure handle this backend did not hand out; `unsupported` = a role pair it cannot spell (the `use()` answer); `not_ready` = not recording. **Ownership/lifetime:** the descriptor's spans are BORROWED for the call only, and the resources they name must stay alive through GPU completion. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error barrier(image_barrier const& one) = 0;` -- **Requirement:** record one image transition in the same pair vocabulary as `use()`, over a subresource range; an ALL-ZERO range means the whole image. **Returns/errors:** `ok`; `invalid_argument` = a null or foreign image; `unsupported` = a pair this backend cannot spell; `not_ready` = not recording. **Ownership/lifetime:** the image is borrowed and must stay alive through GPU completion; a borrowed frame image must respect the frame lifetime. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error copy_image(image_copy const& copy) = 0;` -- **Requirement:** record one image-to-image copy, each side with its own region; both sides are read and written at their CURRENT layouts, so the descriptor carries no layout pair. **Returns/errors:** `ok`; `not_ready` = not recording; `invalid_argument` = a null source/destination or a handle this backend did not hand out; the contract states no texel/subresource range rule for this verb (this backend applies the copy region literally). **Ownership/lifetime:** both images are borrowed and must stay alive through GPU completion. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error copy_buffer(buffer& destination, buffer const& source, std::uint64_t size, std::uint64_t source_offset, std::uint64_t destination_offset) = 0;` -- **Requirement:** record a byte-range copy between two buffers. **Returns/errors:** `ok`; `not_ready` = not recording; `invalid_argument` = a handle this backend did not hand out. Size 0 or a source/destination byte range extending beyond its buffer MUST be refused with `invalid_argument`; validation MUST avoid integer overflow. **Ownership/lifetime:** both buffers are borrowed and must stay alive through GPU completion. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error clear_color_image(image const& target, deren::utility::vessel::array<float, 4> const& color, subresource_range const& range) = 0;` -- **Requirement:** record a clear of the color image's subresource range to one RGBA value; an ALL-ZERO range means the whole image. **Returns/errors:** `ok`; `not_ready` = not recording; `invalid_argument` = a null or foreign image. **Ownership/lifetime:** the image is borrowed; it must stay alive through GPU completion, and the clear must be ordered by the caller against later reads. **Threading:** caller-serialized; `color` components are RGBA.

- `[[nodiscard]] virtual error load_texture_group(texture_group_ref const& /*group*/) {` -- **Requirement:** bind one immutable sampled texture group for following work (ABI 31); a null group clears the binding. Success RETAINS the group for recorded work. **Returns/errors:** `ok`; `not_ready` = not recording, or the group is not owned by this core, or no pipeline is bound; `invalid_argument` = a group this backend did not hand out or a token mismatch; `unsupported` = a backend without the mechanism (the DEFAULT BODY) or a pipeline whose texture-group slot is not enabled. **Ownership/lifetime:** the group is retained through recorded work and pending GPU use; the device and backend must outlive every reference. **Threading:** contract does not specify.

#### command_buffer

The individually submittable recording owner: it INHERITS the whole recording
vocabulary from `command_recorder`, adds exactly one verb (`execute`, the
recording-time merge of a batch of segments), and is submitted through
`command_context::submit(command_buffer&)`. It is a sibling of
`mergeable_command_buffer`, not a base of it, and it is the receiver of
`execute(batch)`. A caller creates primaries through
`command_context::create_command_buffer` / `make_command_buffer`; the only
borrowed instance is the frame's own primary, handed out by
`frame_context::begin_commands()`, whose `release()` releases nothing. It
declares no destructor or `release()` of its own -- both are the inherited pair,
and `~command_buffer() noexcept override = default;` adds no slot.

- `[[nodiscard]] virtual error execute(std::span<mergeable_command_buffer_ref const> /*batch*/) {` -- **Requirement:** merge a BATCH of mergeable segments into THIS recording buffer -- the recording-time counterpart of the submission-time batch. The batch rules are the same ones `command_context::submit(span)` states: span order, self-contained segments (state and render scopes do not cross a boundary), NO implicit memory barriers, whole-batch validation. **Returns/errors:** `ok`; `invalid_argument` = a segment this backend did not hand out, or a batch whose rules are violated; `not_ready` = this buffer is not recording, or it holds no reference any more; `unsupported` = a backend whose merge must happen at submit time (the DEFAULT BODY) -- that backend serves `submit(span)` instead. **Ownership/lifetime:** the segments are retained through the recording and its submission; the frame primary is borrowed and the caller must not release it. **Threading:** contract does not specify; segments are recorded by their owning threads before they are merged here.

#### frame_walker

The frame-in-flight ring's cursor, as a BORROWED VIEW: a view carries no
reference, so it must not be wrapped in `unique_object`, and `frame_context`'s
walker is the same object on every call. The ring's one home is the backend's
own frame state -- the engine borrows the view and walks, and the backend stays
the only authority, which is the "two sources of truth" shape this face exists to
end. `position()` and a successful `wait_and_acquire()`'s `frame.frame_index`
answer the SAME slot, and the consistency is asserted rather than hoped for.
THE ORDER INSIDE `wait_and_acquire()` IS LOAD-BEARING: read the cursor without
advancing; wait that slot's timeline; LATCH the slot's last completed frame's
GPU timings; acquire the next image; report. `walk_to_next()` advances one slot
after present and promises only that the ring moved -- "the frame is over" is not
what it says.

- `virtual ~frame_walker() noexcept = default;` -- **Requirement:** the view is never destroyed by a caller; it lives as long as the core. **Returns/errors:** none. **Ownership/lifetime:** no reference and no release -- a caller MUST NOT delete, release or wrap it. **Threading:** contract does not specify; the cursor is mutable backend state, and no claim is made that walking is thread-safe.

- `[[nodiscard]] virtual std::uint32_t slot_count() const noexcept = 0;` -- **Requirement:** answer how many slots the ring has -- the RUNTIME truth, and the only authority (the compile-time `max_frames_in_flight` is data pinned to it, and the engine checks the two once at startup and panics on a mismatch). **Returns/errors:** a count; no error channel. **Ownership/lifetime:** a plain value. **Threading:** contract does not specify.

- `[[nodiscard]] virtual std::uint32_t position() const noexcept = 0;` -- **Requirement:** answer the slot THIS frame is being recorded into. READ-ONLY: the cursor moves in `walk_to_next()`, never in a read. **Returns/errors:** a slot index; no error channel. **Ownership/lifetime:** a plain value; it must agree with a successful acquire's `frame_index`. **Threading:** contract does not specify.

- `[[nodiscard]] virtual frame_open_info wait_and_acquire() = 0;` -- **Requirement:** wait this slot's timeline (a value of 0 means never submitted, so nothing to wait for), latch the slot's previous frame's GPU timings -- here, between the wait and the acquire, so a frame that dies on out-of-date is still collected -- acquire the next image, and report the decision by value. **Returns/errors:** `frame_open_info` whose `result.code == ok` means `frame` names the opened frame; ANYTHING else means `frame` is zeroed and MUST NOT be used, while `result` names the failing step (`out_of_date` = rebuild and skip this frame, `device_lost` = fatal, and so on). **Ownership/lifetime:** the report is a value; the acquired image is reachable through `frame_context::frame_image()` for the frame's duration. **Threading:** contract does not specify; the ring state is the backend's and the contract adds no concurrent-walk guarantee.

- `virtual void walk_to_next() noexcept = 0;` -- **Requirement:** advance the ring exactly one slot -- the present-side close of the frame. It does NOT promise "the frame is over", only that the ring moved. **Returns/errors:** none (`void`). **Ownership/lifetime:** no resource. **Threading:** contract does not specify.

#### gpu_profiler

The GPU timing report of the LAST COMPLETED frame, as a BORROWED VIEW (no
`release()`, the `frame_walker` rule). It is the REPORT, not the recording: marks
are written while a frame is recorded, and the backend latches a slot's finished
frame when its timeline is waited, so after `wait_and_acquire()` answers this
face describes the frame that just completed. A "stage" is the interval one mark
OPENS: stage i runs from mark i to mark i + 1 and is named by mark i. The last
mark closes the frame and opens nothing. Durations are NANOSECONDS; the display
layer converts to milliseconds.

- `virtual ~gpu_profiler() noexcept = default;` -- **Requirement:** the view is never destroyed by a caller. **Returns/errors:** none. **Ownership/lifetime:** borrowed; lives as long as the core; no release. **Threading:** contract does not specify.

- `[[nodiscard]] virtual std::uint32_t stage_count() const noexcept = 0;` -- **Requirement:** answer how many marks the last completed frame wrote -- 0 means nothing latched yet, or no timing support. **Returns/errors:** a count; 0 is a legal answer, not an error. **Ownership/lifetime:** a plain value describing the latched frame. **Threading:** contract does not specify; the latch is backend state.

- `[[nodiscard]] virtual error get_stage_info(std::uint32_t index, std::string_view* name, std::uint64_t* duration_ns) const noexcept = 0;` -- **Requirement:** answer one stage's name and duration. `name` and `duration_ns` are CALLER-PROVIDED stores and either may be nullptr individually (ask only what you need); the name is the static text the mark carried and is valid as long as the process runs, because the backend is never unloaded. **Returns/errors:** `ok`; `unsupported` = this device or configuration cannot timestamp (and `stage_count()` is then always 0); `invalid_argument` = an index at or past `stage_count()`; `device_lost` = the timestamp read-back failed; `not_ready` = no marks latched yet, OR the index names the frame's LAST mark, which opens no stage. **Ownership/lifetime:** both stores are borrowed for the call; nothing is retained. **Threading:** contract does not specify.

---

### 7.3 Factories, contexts and the root

#### resource_factory

The BORROWED resource creation interface, reached as `api_core::resources`.
An EMPTY product means creation was refused or failed: the contract's factories
have no error channel, so a refusal is expressed as an empty
`unique_object` / `shared_object` and the backend may log the named reason --
callers are expected to test the handle, not an error code. Descriptors are
borrowed only until the call returns, and every descriptor carries the
append-only `struct_size` guard: a field whose whole extent is not inside the
bytes the caller declared keeps this build's default, so an older caller is
detected instead of mis-read. Products keep their existing ownership rules (one
reference, dropped through `release()`; a content-keyed backend may share them).
The interface owns nothing and must not outlive its core; its copy operations are
deleted.

- `virtual ~resource_factory() noexcept = default;` -- **Requirement:** the view is destroyed only with the core; it is not a resource and carries no allocation of its own. **Returns/errors:** none. **Ownership/lifetime:** borrowed from the core, which must outlive every product and every use of this interface. **Threading:** contract does not specify; no factory call is documented as safe to race with another.

- `[[nodiscard]] virtual unique_object<buffer> create_buffer(buffer_desc const& desc) = 0;` -- **Requirement:** create a buffer from its memory intent, extra capabilities, size and optional initial bytes; the initial bytes being part of the DESCRIPTOR is deliberate, because a content-keyed backend can only recognise identical content if creation sees it, and empty initial bytes mean ALLOCATE ONLY (a backend that deduplicates MUST NOT match an allocate-only buffer against another). A descriptor the backend cannot honour (a capability it cannot serve, zero size, nonempty initial bytes whose length differs from the size) is REFUSED rather than silently satisfied. **Returns/errors:** one reference, or an EMPTY product on refusal; no error code. **Ownership/lifetime:** the reference is the caller's, dropped through `release()`; `initial_bytes` is borrowed until the call returns. **Threading:** contract does not specify.

The buffer initialization rule is shared by the reference backend and the portable CPU probe through
`validate_buffer_initialization(size, initial_size)`:

| Description | Result |
| --- | --- |
| `size == 0` | Empty product; no allocation |
| `size > 0`, initial bytes empty | Allocate only; contents are undefined, with no zero-fill guarantee |
| Initial byte count equals `size` | Copy exactly that payload when the requested upload mechanism is supported |
| Any other nonzero initial byte count | Empty product; no partial upload or silent truncation |

The initial span is consumed/copied before the factory returns; the caller may immediately reuse its storage.
The size rule does not promise every backend supports initial uploads for every usage: an unsupported mechanism
returns an empty product without aborting on caller input. In the current Vulkan backend, storage-GPU-only,
acceleration-structure storage and scratch buffers are allocation-only; vertex/index/uniform GPU buffers support
both allocation-only and initialized creation. Later initialization is an explicit upload/copy with the required
buffer flags and synchronization. Passing empty initial bytes MUST NOT cause a staging read from a null pointer.

- `[[nodiscard]] virtual unique_object<image> create_image(image_desc const& desc) = 0;` -- **Requirement:** create an image from its shape, its format (a named byte layout, or the depth ROLE when the caller only needs "a depth attachment"), its required capabilities and its optional initial texels; the same content-at-creation and allocate-only rules as `create_buffer` apply. **Returns/errors:** one reference, or an empty product on refusal (a format/flag pair the backend cannot serve, a shape or initial-bytes mismatch). **Ownership/lifetime:** the caller's reference, dropped through `release()`; a view of the image is a separate owned object. **Threading:** contract does not specify.

- `[[nodiscard]] virtual unique_object<acceleration_structure> create_acceleration_structure(acceleration_structure_desc const& desc) = 0;` -- **Requirement:** create a bottom-level structure from borrowed geometry descriptions, or a top-level one with an instance capacity; the structure's storage, scratch and alignment stay the backend's, so a second backend with no explicit acceleration structures may answer this and the build verb however it must. **Returns/errors:** one reference, or an empty product on refusal. **Ownership/lifetime:** the geometry pointer is borrowed until the call returns (the addresses and counts are part of the structure's identity); a later byte change behind the same addresses is a REFIT, declared by the update flag at creation. **Threading:** contract does not specify.

- `[[nodiscard]] virtual unique_object<micromap> create_micromap(micromap_desc const& desc) = 0;` -- **Requirement:** create an opacity micromap, copying the attributes, per-triangle records and index array into BACKEND-owned setup memory (including the address alignment the build requires, which is a requirement on an address and therefore cannot be the caller's problem). **Returns/errors:** one reference, or an empty product on refusal. **Ownership/lifetime:** every span in the descriptor is borrowed only until the call returns. **Threading:** contract does not specify.

- `[[nodiscard]] virtual unique_object<sampler> create_sampler(sampler_desc const& desc) = 0;` -- **Requirement:** create sampler state; the descriptor is append-only and its defaults reproduce the previously implicit behaviour exactly, and a descriptor older than this build is refused by the `struct_size` guard rather than read out of bounds. **Returns/errors:** one reference, or an empty product on refusal. **Ownership/lifetime:** the caller's reference; a sampler named by a resource group must outlive the group's GPU use. **Threading:** contract does not specify.

- `[[nodiscard]] virtual unique_object<shader> create_shader(shader_desc const& desc) = 0;` -- **Requirement:** create a shader module from SPIR-V and a stage; the stage is mandatory because the ray-tracing stages must be nameable for a group table to be built. **Returns/errors:** one reference, or an empty product on refusal (invalid code, an unsupported stage, a code/stage mismatch). **Ownership/lifetime:** `desc.code` is borrowed only until the call returns; `debug_name` is log text and is not retained. **Threading:** contract does not specify.

- `[[nodiscard]] virtual unique_object<pipeline> create_pipeline(pipeline_desc const& desc) = 0;` -- **Requirement:** create a graphics, compute or ray-tracing pipeline; the THREE spellings are selected by the descriptor, not by separate verbs -- `first_stage == compute` selects the compute spelling (attachment state is then ignored), and a NON-EMPTY `ray_tracing_stages` selects the ray-tracing spelling (the colour/depth/compute/blend fields are then ignored). The entry point is not a field, because every shader this renderer builds is entered at "main"; a descriptor with no entry-point spelling means "main". The shader binding table is deliberately NOT part of the descriptor: its handles are per-pipeline data read back after creation, and its regions are the caller's memory. **Returns/errors:** one reference, or an empty product on refusal (a stage/descriptor combination the backend cannot build, a group table that does not match the stages). **Ownership/lifetime:** every span in the descriptor (code, formats, blend modes, stages, groups, bindings) is borrowed only until the call returns; `debug_name` is not retained. **Threading:** contract does not specify. NOTE: pipeline creation may be a long device operation; the contract states no concurrency rule for it.

- `[[nodiscard]] virtual std::expected<resource_group_ref, error> create_resource_group(resource_group_desc const&) {` -- **Requirement:** create an immutable shared composition from its entries; the descriptor's spans are COPIED before the call returns, and creation is atomic (a failed product publishes no group and reclaims its temporary descriptor slots). Empty groups, duplicate bindings or parameter locations, null resources, a non-owning (borrowed) member, foreign resources, invalid ranges/usages and invalid layouts MUST be rejected. **Returns/errors:** the group, or a named error: `invalid_argument` for a rejected composition, `unsupported` for a backend without the mechanism (the DEFAULT BODY) -- never a half-built group. **Ownership/lifetime:** the group owns its member references (a member must carry lifetime ownership, and a lifetime alias is accepted only if its owner really keeps the target alive); the core/backend must outlive the group. **Threading:** contract does not specify.

- `[[nodiscard]] virtual texture_group_ref assign_texture_group(texture_group_info const& /*info*/, error* result = nullptr) {` -- **Requirement:** register one legacy immutable group of up to 16 sampled 2D slots; empty slots are legal, so a sparse group is a legal group. **Returns/errors:** the shared group handle; EMPTY on refusal, with the named reason written through the OPTIONAL `error*` out-parameter when the caller supplies one -- this is the contract's only factory with an out-of-band error channel, and `unsupported` is the answer of the DEFAULT BODY. The contract does not enumerate the remaining codes; this backend answers `invalid_argument` (malformed input or a foreign image), `not_ready` (no device), `unsupported` (an image shape/format the group cannot serve), and the out-of-memory pair. **Ownership/lifetime:** the group retains the member images it was given; the device and the backend must outlive every reference; the caller's handles are borrowed for the call. **Threading:** contract does not specify.

#### command_context

The BORROWED independent command allocation and submission interface, reached as
`api_core::commands`. It allocates command buffers and submits them; it REJECTS
the acquired frame's own primary (that one belongs to `frame_context::submit()`),
validates provenance and recording state before queuing anything, and leaves the
caller-owned buffers and images with the caller's own lifetime obligation -- they
must outlive GPU use. Batches execute in SPAN ORDER, validate atomically, and
add no implicit visibility barriers. Its copy operations are deleted and it must
not outlive its core.

- `virtual ~command_context() noexcept = default;` -- **Requirement:** the view is destroyed only with the core. **Returns/errors:** none. **Ownership/lifetime:** borrowed; no allocation of its own. **Threading:** contract does not specify.

- `[[nodiscard]] virtual unique_object<command_buffer> create_command_buffer(command_buffer_desc const& desc) = 0;` -- **Requirement:** allocate one individually submittable command buffer; PRIMARY is the only kind a caller can create (the secondary kind is retired -- the multithreaded paths use mergeable segments), and the retired value MUST be refused by name rather than silently promoted. **Returns/errors:** one reference, or an empty product on refusal (the retired kind, allocation failure). **Ownership/lifetime:** the caller's reference, dropped through `release()`; the buffer owns its allocation, and no pool or reset verb exists. **Threading:** contract does not specify; calls on one recorder MUST be serialized, including when its begin declared `simultaneous_use`; that flag concerns overlapping execution, not recording threads.

- `[[nodiscard]] virtual shared_object<command_buffer> make_command_buffer(command_buffer_desc const& desc) = 0;` -- **Requirement:** the same creation as `create_command_buffer`, handed out as the contract's SHARED handle: the ONE factory reference is the one the last shared copy drops through `release()`, so there is no second lifetime rule. **Returns/errors:** the handle, or an EMPTY one on refusal -- the same answer as an empty `unique_object`, with the reason named where it happened. **Ownership/lifetime:** the api_core and backend must outlive all strong and weak references. **Threading:** contract does not specify.

- `[[nodiscard]] virtual mergeable_command_buffer_ref make_mergeable_command_buffer(command_buffer_desc const& desc) = 0;` -- **Requirement:** allocate one mergeable SEGMENT -- the batch-only recording handle, a sibling of `command_buffer` that cannot be submitted individually and cannot receive `execute()`. **Returns/errors:** the shared segment handle, or an empty one on refusal. **Ownership/lifetime:** segments are retained by the backend until the submission that merged them completes; the batch API takes them by shared reference for exactly that reason. **Threading:** contract does not specify, but these are the handles the engine's multithreaded recording paths use, one per worker.

- `[[nodiscard]] virtual error submit(command_buffer& commands) = 0;` -- **Requirement:** submit ONE caller-owned buffer to the device. Provenance and recording state are validated BEFORE queuing, and the acquired frame's own primary is REJECTED (it has its own submit protocol). **Returns/errors:** `ok`; `invalid_argument` = the borrowed frame primary, a foreign buffer, or a non-primary kind; `not_ready` = not executable yet (still recording, or already pending), or no queue is available; device-level failures travel as themselves. The contract carries NO fence or completion token here, so the caller's own pacing is the only stated ordering. **Ownership/lifetime:** the backend retains the submitted command buffer, merged segments and explicitly retained group owners until completion. Directly bound pipelines and borrowed buffer/image operands MUST be kept alive by the caller through GPU completion. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error submit(std::span<mergeable_command_buffer_ref const> batch) = 0;` -- **Requirement:** submit a BATCH of mergeable segments in SPAN ORDER, with the WHOLE batch validated before anything is queued (there is no rollback of what a previous call already queued), the segments self-contained (state and render scopes do not cross a boundary), and NO memory barrier inserted between them. The backend owns the merge: it may record the segments into a backend-owned primary, or hand them to its own queue as a list. **Returns/errors:** `ok`; `invalid_argument` = a foreign or individually-unsubmittable segment, or a batch whose rules are violated; `not_ready` = a segment is not executable; `unsupported` = a backend that only serves the recording-time merge (`command_buffer::execute`). **Ownership/lifetime:** the backend retains the handles until the submission completes. **Threading:** contract does not specify; the span order is the only ordering the contract fixes.

#### frame_context

The BORROWED frame and presentation interface, reached as
`api_core::frames`, and present even for an offscreen core (where `present()`
answers `unsupported`). It owns no resource of its own: the walker, the profiler
and the swapchain it answers are STABLE BORROWED VIEWS that must never be
released or deleted, and the frame command buffer, frame image and readback
buffer it hands out are borrowed views that hold no reference. Its copy
operations are deleted and it must not outlive its core.

- `virtual ~frame_context() noexcept = default;` -- **Requirement:** the view is destroyed only with the core. **Returns/errors:** none. **Ownership/lifetime:** borrowed; the borrowed sub-views it answers die with the core. **Threading:** contract does not specify.

- `[[nodiscard]] virtual command_buffer* begin_commands() = 0;` -- **Requirement:** answer the CURRENT frame's primary, or null outside recording -- the call starts nothing, it hands out the list the frame's recording goes into. **Returns/errors:** a borrowed pointer, null when there is no frame in flight (the contract's way of saying "there is nothing to record into"). **Ownership/lifetime:** borrowed until frame submission; the caller MUST NOT release or delete it; calling its underlying `release()` violates ownership preconditions and MAY terminate. The borrowed view must not be retained past the frame. **Threading:** contract does not specify.

- `[[nodiscard]] virtual image* frame_image() noexcept = 0;` -- **Requirement:** answer the LAST ACQUIRED image. **Returns/errors:** a borrowed pointer, null before the first acquire; no error channel. **Ownership/lifetime:** valid through submit until the next acquire; borrowed, so the caller MUST NOT release it and must not use it past that window. **Threading:** contract does not specify.

- `[[nodiscard]] virtual buffer* frame_readback_buffer() noexcept = 0;` -- **Requirement:** answer the host-visible slot the frame copy path uses. **Returns/errors:** a borrowed pointer; the contract states no null condition beyond "the frame's own buffer". **Ownership/lifetime:** borrowed, never released; COMPLETION and host visibility are the CALLER's responsibility -- the contract provides no fence here. **Threading:** contract does not specify.

- `[[nodiscard]] virtual submit_info frame_begin() = 0;` -- **Requirement:** the LEGACY acquisition: acquire the next image and report the frame by value. It is retained for compatibility and the contract itself points callers at `walk_frames()->wait_and_acquire()` for full error information. **Returns/errors:** a `submit_info` by value; a ZEROED answer means no frame started, and every failure (out-of-date, device-lost, a failed timeline wait) is squashed into that one answer -- there is deliberately no error channel here. **Ownership/lifetime:** a plain value naming the ring slot and the acquired image. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error present() = 0;` -- **Requirement:** present the image the core's own acquire took -- the verb takes no argument because the acquire state is the backend's. **Returns/errors:** `ok`; `unsupported` = a windowless core; `not_ready` = no frame has been acquired (a named refusal instead of a queue call); `out_of_date` = the chain must be rebuilt and this frame skipped; `surface_lost` = the window/surface is gone; a failed presentation never reports silence. **Ownership/lifetime:** no resource; the presented image returns to the chain. **Threading:** contract does not specify.

- `[[nodiscard]] virtual frame_walker* walk_frames() noexcept = 0;` -- **Requirement:** answer the ring's cursor view. `api_core` hands out the SAME object on every call; the backend stays the only authority for the cursor. **Returns/errors:** a borrowed pointer, non-null for a live core. **Ownership/lifetime:** borrowed, never released or deleted, lives as long as the core. **Threading:** contract does not specify.

- `[[nodiscard]] virtual gpu_profiler* profiler() noexcept = 0;` -- **Requirement:** answer the timing report view of the last completed frame. **Returns/errors:** a borrowed pointer; a core without timing support reports zero `stage_count()` rather than a null profiler (the contract states the view, the `unsupported` answer is `get_stage_info()`'s). **Ownership/lifetime:** borrowed; described by the frame the last `wait_and_acquire()` latched. **Threading:** contract does not specify.

- `[[nodiscard]] virtual swapchain* frame_swapchain() noexcept = 0;` -- **Requirement:** answer the frame's own swapchain view -- its `extent()` and `format()` are the presentation facts, including in a windowless core, where `recreate()` and `present()` answer `unsupported`. **Returns/errors:** a borrowed pointer, non-null for a live core. **Ownership/lifetime:** borrowed and stable; never released or deleted. **Threading:** contract does not specify.

- `[[nodiscard]] virtual error submit() = 0;` -- **Requirement:** submit the CURRENT frame using the backend's own acquire/presentation state, with no buffer argument -- the protocol that preserves frame waits, resource retention and completion rules. An offscreen core closes the frame after submitting it. **Returns/errors:** `ok`; `not_ready` = no frame is in flight, or the frame's recording is not finished or is not executable; WSI and device failures (`out_of_date` to rebuild and skip, `surface_lost`, `device_lost`) travel as themselves. **Ownership/lifetime:** the backend retains the frame's recording and explicitly retained group/segment owners until completion; directly bound pipelines and borrowed resource operands remain caller-owned through GPU completion; the frame's borrowed views stop being valid for the caller at the next acquire. **Threading:** contract does not specify.

#### api_core

The ROOT: device ownership, the ABI number, capabilities and global control. It
is the one object that crosses the boundary as a C++ type, reached through the
single C entry point `deren_make_api_core()`, whose first argument is
`rhi::abi_version` and which hands the root out as the contract's own
`shared_object<api_core>` -- its control block's last copy calls `release()`, so
no call site ever names a deleter. The three subviews (`resources`, `commands`,
`frames`) are `context_view<T>` members: pointer-sized, non-assignable and
non-rebindable, owning nothing, bound ONCE by a derived backend before
publication and never outliving the core. The root is also the one interface the
plan lets grow "big"; it must never learn an engine concept (no scene, no pass,
no camera).

- `virtual ~api_core() noexcept = default;` -- **Requirement:** destruction runs inside the backend, reached through `release()` from the shared handle's last copy. **Returns/errors:** none (`noexcept`). **Ownership/lifetime:** a caller never destroys the root directly; all strong and weak references must be gone first. **Threading:** contract does not specify; destroying a core under live resource references is a lifetime violation, not a race the contract resolves.

- `virtual void release() noexcept = 0;` -- **Requirement:** drop the caller's one reference; for the ROOT that call DESTROYS the core and its device. **Returns/errors:** none (`void`, `noexcept`). **Ownership/lifetime:** the root's `shared_object` control block calls exactly this on its last copy, so the drop is balanced with the reference the entry point handed out; the device, and therefore every resource, borrowed view and group reference, must be gone before it. **Threading:** contract does not specify; releasing the core while another thread uses a subview or a resource handle is not covered.

- `[[nodiscard]] virtual ability_bits abilities() const noexcept = 0;` -- **Requirement:** answer the tier-2 ability bit set. A backend MUST NOT announce a bit it cannot serve: every announced bit's `query_extension(kind)` has to answer with an object able to carry that ability out, and this backend asserts the agreement at construction and panics when it is broken. The engine asks and keeps compiling when an answer is "no". **Returns/errors:** a bit set; no error channel -- an empty set is the honest answer. **Ownership/lifetime:** a plain value. **Threading:** contract does not specify; the set is fixed for the core's life.

- `[[nodiscard]] virtual extension* query_extension(extension_kind kind) noexcept = 0;` -- **Requirement:** answer the optional interface for that kind, or null. This is the ONLY way to reach an ability, which is what keeps this vtable stable as extensions arrive; the contract's `query_extension<T>(core)` helper checks the answered object's own `kind()` and returns null on a mismatch. **Returns/errors:** a borrowed pointer, null for a kind this backend does not serve. **Ownership/lifetime:** BORROWED and stable: no reference, no release, never deleted, lives as long as the core. **Threading:** contract does not specify; a backend resolves a target against its own live registries and a foreign target is `invalid_argument` at the verb that receives it.

- `virtual void wait_idle() = 0;` -- **Requirement:** make the device idle -- the device-wide barrier a caller uses as its own pacing when the contract carries no fence (reading `buffer::mapped()`, re-reading an image, submitting a one-shot list). **Returns/errors:** none (`void`, and NOT `noexcept`); the contract states no failure channel and no error value, so a failure has no defined report. **Ownership/lifetime:** no resource; it does not release anything, and it does not end the frame by itself. **Threading:** contract does not specify. It may be called outside recording and does not implicitly apply pending operations or close an open recording.

- `[[nodiscard]] virtual std::uint32_t api_version() const noexcept = 0;` -- **Requirement:** answer the ABI number this backend implements; a host may be handed a core it did not create, so the object attests its own number and the runtime checks it after the handshake. **Returns/errors:** a number; a mismatch is refused at the entry point (`abi_mismatch`, with the backend's own number in the error information) before an object exists. **Ownership/lifetime:** a plain value. **Threading:** contract does not specify.

- `[[nodiscard]] virtual main_context* main_context() noexcept {` -- **Requirement:** answer the opaque device root a plugin may need (the GUI plugin's Dear ImGui backend is the measured case); it carries no native type and no verbs in this contract. **Returns/errors:** a borrowed pointer; the DEFAULT BODY answers null, and the contract does not say when a backend must answer one. **Ownership/lifetime:** borrowed; no reference, no release, and it must be used only as the opaque root the receiving plugin validates. **Threading:** contract does not specify.

---

### 7.4 Open/unverified

The earlier audit listed missing rules that section 6.1 and the contract headers have since resolved. Those
items are no longer open requirements; the remaining contract choices are listed once in section 9.2, and
removed interfaces are listed in section 9.3. In particular, buffer ranges, allocation-only initialization,
rendering-scope pairing and borrowed ownership are requirements, not backend-defined behavior.

Implementation verification is separate from specification completeness. The portable CPU probe can verify
factory shape rules and basic recording errors; it does not model Vulkan native-state restoration, descriptor
publication or borrowed identity generations. Device tests are required for those mechanisms. A passing
headless suite alone MUST NOT be reported as proving every native interop or submission path.

### 7.5 Recording state and application boundaries

One recorder is caller-serialized. The following states describe a session, not a new API or an exposed enum.
An inherited rendering scope belongs to the primary: a secondary begun with `render_pass_continue` may record
inside it but MUST NOT close it with `end_rendering()`.

| State / call | Contract outcome |
| --- | --- |
| Not recording: `apply_operations` or `end_recording` | `not_ready` on the deferred backend; no recording is made executable |
| Not recording, and no pending/retained use prevents reuse: `begin_recording` | Opens a fresh session and clears prior logical state and latched errors |
| Recording: invalid error-returning enqueue | Returns its error and appends nothing; a correct later call can recover unless an application error is already latched |
| Recording: invalid value-only operation | Latches failure; application/end reports it and the recording cannot be submitted |
| Recording: `apply_operations` | Validates the retained batch before replay, preserves final logical state, and leaves the session open |
| Recording: `end_rendering` without a locally opened scope (including an inherited scope) | Latches `invalid_argument`; emits no native end-rendering command |
| Recording: `end_recording` with a locally opened rendering scope left unclosed | Fails the recording; an inherited scope does not require a secondary-local close |
| Failed recording: `apply_operations` / `end_recording` | Reports the first latched error; end discards the failed session instead of publishing it |
| Executable recording: submit / execute | Validates the whole target/batch and retains the explicitly owned recording dependencies |
| GPU-pending or retained secondary recording: begin/reset attempt | Refused until legal reuse; `simultaneous_use` permits overlapping execution where supported, never concurrent recording/reset |

`apply_operations` is an intermediate observability boundary: state setters at its end may be needed by later
work or native recording. Final application at `end_recording` may discard trailing pure state with no consumer.
Copies, barriers, timestamps, builds, draws and dispatches keep their order and occurrences. A failed later
batch does not roll back native commands already emitted by an earlier application, but the failed recording
as a whole MUST NOT be submitted.

Native/plugin interop is a backend-specific protocol, not an extra portable RHI entry point. In the Vulkan
bridge, call `prepare_native()` and check success before raw recording, then call `finish_native()` when that
recording is complete. The native `get()` accessor does not flush by itself. The bridge conservatively
invalidates native applied state; the next RHI application restores the saved logical state before its
consumers. Raw calls MUST NOT reset/end the command buffer, change the RHI-owned rendering scope, or destroy
borrowed operands. The explicit GUI descriptor-heap bind remains part of that native protocol.

## 8. Optional extension interfaces

Status: **draft v1 for RHI ABI 55**. Section 8 of *The deren RHI API standard*. Sections 1-6 (scope and
source of truth, the object/ownership model, errors and validation order, recording/frames/synchronization,
ABI rules, conformance checklist) are normative as written and are NOT repeated here; where a clause below
needs one of them it says so. Where this file and `source/promise/rhi/rhi.extension.cppm` disagree, the
source is authoritative and this file is the defect.

This section is normative for `source/promise/rhi/rhi.extension.cppm`: the tier-2 interfaces a backend MAY
implement, the `extension_kind` bit that announces each one, and the per-virtual obligations of a backend
that announces it. The tier-1 interfaces (`rhi.api_core.cppm`) are section 7; the value types the abilities
carry (`synchronization::stage`, `scoped_image_info`, `heap_*_info`, `descriptor_position`,
`shader_binding_table_properties`, `submission_token`) are declared in the extension and synchronization
partitions and are used here without redefinition.

Two rules frame every clause below.

1. **A bit is a promise about service.** `api_core::abilities()` reports an `ability_bits` set; each ACTIVE
   `extension_kind` value is exactly one bit; `api_core::query_extension(kind)` returns the ability object or
   null. A backend MUST NOT set a bit it cannot serve, and the gate is two-way: every set bit MUST answer a
   non-null object whose `kind()` equals the bit, and every kind that answers non-null MUST have its bit set.
   `rhi::query_extension<T>(core)` re-checks `kind()` after the lookup and answers null on a mismatch.
2. **Absence is unsupported, not a downgrade.** `api_core::query_extension` MUST return null for every kind
   this backend does not implement, and a caller MUST treat that null as `error::unsupported` (or as the
   documented absence path for that one extension) rather than silently taking a slower route. A backend that
   answers an object, or sets the bit, and then no-ops its verbs is non-conformant.

An ability object is a labelled subview the CORE owns: it is constructed with a concrete implementation name
(its `object::type` tag), it has no `release()` verb, and a caller MUST NOT delete it or free it. The pointer
returned by `query_extension` is borrowed: it is valid for the core's lifetime and dies with the core, so
holding one past `api_core::release()` is a contract violation of the same kind as holding any other handle.

Bit values are ABI and never move: `to_bits(device_address) == 0x1`, `all_abilities() == 0x7FD3`, and
`all_extension_kinds()` enumerates twelve active kinds. `mesh_shader` (1<<2), `ray_tracing` (1<<3) and `vulkan_escape` (1<<5)
are RETIRED values with no interface behind them; they MUST NOT be reused, renumbered or announced by any
backend, and querying them answers null.

### 8.1 Ability bits

| kind | bit | interface | a backend answers it when | absence means to a caller |
| --- | --- | --- | --- | --- |
| `device_address` | 0x1 | `device_address` | it can report a buffer's device address | addresses are unavailable; use handle-based binds |
| `descriptor_heap` | 0x2 | `descriptor_heap` | its heap is created and can serve writes and binds | no bindless write/bind path; refuse, do not emulate with per-draw sets |
| `mesh_shader` | 0x4 | (none, RETIRED in ABI 54) | never | use tier-1 `command_recorder::draw_mesh_tasks` |
| `ray_tracing` | 0x8 | (none, RETIRED) | never | not a queryable ability |
| `host_image_copy` | 0x10 | `host_image_copy` | its API can copy an image to host memory directly | staging copies only; the caller must use its own staging path |
| `non_blocking_write` | 0x8000 | `non_blocking_write` | CPU writes into a backend image without a queue command; Vulkan uses `vkCopyMemoryToImageEXT` | the caller must establish the later host-write -> shader-read visibility dependency |
| `vulkan_escape` | 0x20 | (none, RETIRED) | never | not a queryable ability |
| `device_capabilities` | 0x40 | `device_capabilities` | a device exists (every method is answerable) | no device facts; treat every optional feature as absent |
| `shader_group_access` | 0x80 | `shader_group_access` | it can read back shader-group handles for a ray-tracing pipeline | no portable SBT data; no ray-tracing launch that needs a table |
| `gpu_sorting` | 0x100 | `gpu_sorting` | it can create a radix sorter | sort on the host or refuse |
| `async_presentation` | 0x200 | `async_presentation` | it has a presentation path it can run off the caller's thread | present synchronously; absence is NOT "present without waiting" |
| `scoped_synchronization` | 0x400 | `scoped_synchronization` | it can validate and record explicit scoped barriers | record barriers through the legacy role-pair face only |
| `frame_resources` | 0x800 | `frame_resources` | it has an acquire/snapshot protocol (it answers the object; `retain_image()` carries the no-frame answer) | the mutable `frame_image()` facade only; no immutable snapshot |
| `descriptor_space` | 0x1000 | `descriptor_space` | it can position and write an immutable texture group | absolute heap slots are not expressible; refuse |
| `command_submission` | 0x2000 | `command_submission` | it can submit independent work and hand back a completion token | submissions carry no token; the backend's own submission path only |

Optional/descriptor-space behavior, stated once and relied on below: **a backend without a descriptor heap
MUST NOT answer `descriptor_space`.** The space is the servable face of the heap (slot pool plus group
table), so the two bits are announced under the same condition; in this tree both are gated on the heap
being ready. A backend with a heap-like model but no texture-group table answers `descriptor_heap` and
returns null for `descriptor_space`; the "a set bit is a promise about service" rule, not a separate device
fact, decides that, and the caller's move is the tier-1 texture-group path or a named refusal.

---

#### extension

The common root of the tier-2 abilities, not an ability itself: `kind()` is the only virtual every ability
shares, which is what lets the backend add an ability without moving another one's vtable. No bit announces
`extension`, and `query_extension` never hands out a bare `extension` -- a caller that wants a specific kind
asks for that kind. The object is constructed by each derived interface with its concrete implementation name
(the `object::type` tag) and is owned by the core: there is no `release()` on this interface, so a caller has
no drop path and MUST NOT invent one (`delete` on an ability pointer is a contract violation and a panic on
the backend side at teardown). Availability is the derived interface's, never this one's.

- `virtual ~extension() noexcept = default;` -- **Requirement:** destruction has to be virtual (the derived
  interfaces are destroyed through the core's own teardown) and no caller may drive it, because the ability
  is a core subview rather than a handle with a reference count; a backend MUST NOT make an ability object
  outlive its core and MUST NOT destroy one before the core is released, because every pointer it handed out
  is borrowed and the contract gives the caller no way to observe that the object went away;
  **Returns/errors:** none, `noexcept`;
  **Ownership/lifetime:** the core owns the object for its whole life; the pointer is borrowed, is never
  reference-counted by the contract, and dies with the core; **Threading:** contract does not specify.
- `[[nodiscard]] virtual extension_kind kind() const noexcept = 0;` -- **Requirement:** answer exactly the
  `extension_kind` this interface is (each derived interface fixes it with a `final` override and a
  `static constexpr extension_id`), never another kind's value: `query_extension<T>` compares the answer and
  turns a mismatch into null, which the caller reads as unsupported; the answer MUST be constant for the
  object's life and MUST NOT depend on device state; **Returns/errors:** one `extension_kind` value, no error
  channel (a backend with nothing to say still has to answer its own kind); **Ownership/lifetime:** none;
  **Threading:** contract does not specify (the answer is declared `const noexcept`, so a backend SHOULD make
  it safe to read from any thread).

#### frame_resources

Immutable snapshots of the currently acquired output image, for scoped recording. Announced by
`extension_kind::frame_resources` (0x800); a backend answers the interface when it has an acquire protocol it
can snapshot, and `retain_image()` -- not the lookup -- carries the "no frame right now" answer. Absent, the
caller keeps the mutable `frame_image()` facade and the old acquire/submit/resize rules; there is no
snapshot, and nothing else substitutes for one. The backend's acquire/submit/resize protocol still owns
native image lifetime: a snapshot pins the CORE, not the swapchain generation.

- `[[nodiscard]] virtual shared_object<image> retain_image() noexcept = 0;` -- **Requirement:** hand back one
  owning reference to an immutable snapshot of the image the current frame draws into, capturing its native
  handle, extent, format and acquire epoch; the identity MUST be stable within one acquire (a backend MAY
  answer the same cached snapshot to repeated calls) and MUST be distinct from the mutable `frame_image()`
  facade; **Returns/errors:** an EMPTY `shared_object<image>` -- not an error, there is no error channel --
  when no frame is in flight or acquired, when the core's frame image handle is null, or when the backend
  cannot snapshot; **Ownership/lifetime:** the call hands out one strong reference under the `shared_object`
  rule of section 2 (drop it by its last copy), the snapshot MUST keep the core alive, it MUST reject scoped
  use once its acquire epoch ends (describe/record answer `invalid_argument` then), it MUST NOT extend the
  native swapchain generation's validity through a resize, and a snapshot image MUST NOT be used with an
  independently owned command buffer; **Threading:** contract does not specify (the reference backend takes
  its image registry mutex).

#### scoped_synchronization

Explicit stage/access scopes with bounded resource ranges, recorded as image and buffer barriers -- the
capability the graph executor binds leases through. Announced by `extension_kind::scoped_synchronization`
(0x400), answered only by a backend that can BOTH describe its objects' shapes and map the scope vocabulary
onto its own barrier model; a backend that can only spell one of the two answers null and the caller keeps
the role-pair path (a record attempt then fails before any command is recorded, never half-way). The
interfaces here are `const` because they publish nothing into the core: `record` writes into the caller's
command list.

- `[[nodiscard]] virtual synchronization::stage supported_stages() const noexcept = 0;` --
  **Requirement:** report the union of `synchronization::stage` bits this backend can actually map on the
  queue(s) behind this core, including the conditional ones (mesh/task only with the feature, the
  ray-tracing pipeline stage only when the device can run it); a barrier naming any stage outside this set
  MUST be refused `unsupported` by `validate`/`record` and MUST NOT be widened to a broader mask;
  **Returns/errors:** a bitmask by value; `stage::none` when the backend has no device (a released core), no
  error channel; **Ownership/lifetime:** none; **Threading:** contract does not specify.
- `[[nodiscard]] virtual error describe(image const&, scoped_image_info&) const noexcept = 0;` --
  **Requirement:** fill mip levels, array layers, aspects, usage mask and the canonical allocation identity of
  an image THIS core handed out, and prove ownership before casting the reference (a foreign object is
  rejected before any implementation cast); the values MUST be the object's real shape, because `validate`
  checks ranges and usage against them; **Returns/errors:** `ok`; `invalid_argument` for an image this core
  did not hand out, a released/dead one, or a frame snapshot whose acquire epoch has ended; `not_ready` when
  the backend has no device; **Ownership/lifetime:** the out-parameter is caller storage, nothing is
  retained, and the answer describes the object as it is now, not as it was at import; **Threading:**
  contract does not specify.
- `[[nodiscard]] virtual error describe(buffer const&, scoped_buffer_info&) const noexcept = 0;` --
  **Requirement:** fill byte size, usage mask and canonical allocation identity of a buffer THIS core handed
  out, under the same ownership-before-cast rule; a buffer whose declared usage cannot express the requested
  scope is the caller's problem to see from `usage_mask`, not a reason to guess; **Returns/errors:** `ok`;
  `invalid_argument` for a foreign, released or dead buffer; `not_ready` with no device; **Ownership/
  lifetime:** out-parameter, nothing retained; **Threading:** contract does not specify.
- `[[nodiscard]] virtual error validate(std::span<scoped_image_barrier const>, std::span<scoped_buffer_barrier const>) const noexcept = 0;` --
  **Requirement:** check EVERY entry without recording anything (this is the graph's prepare-time gate): the
  resource pointer must be non-null and this core's, each mip/layer/byte range must fit the described shape,
  the requested aspects must be the object's own, each transition's before/after scope must be consistent
  with the described usage mask, and every stage bit must be inside `supported_stages()`; a depth/stencil
  independent range the backend cannot serve MUST be answered `unsupported` rather than widened;
  **Returns/errors:** `ok`; `invalid_argument` (null/foreign resource, range out of shape, usage/state
  mismatch); `unsupported` (unmappable layout combination, stage outside `supported_stages`, depth/stencil
  split); `not_ready` with no device -- the first failing entry names the answer, and nothing is recorded;
  **Ownership/lifetime:** none; **Threading:** contract does not specify.
- `[[nodiscard]] virtual error record(command_recorder&, std::span<scoped_image_barrier const>, std::span<scoped_buffer_barrier const>) const noexcept = 0;` --
  **Requirement:** validate the complete group FIRST and then record it atomically, so a rejected group
  publishes no partial barrier work; empty groups still check command ownership and recording state (an empty
  batch is not an excuse to skip the checks); any pending recorder state MUST be applied before the barriers
  are recorded, and the barriers MUST be recorded in the caller's order; **Returns/errors:** `ok`;
  `invalid_argument` (foreign/unknown recorder, a frame snapshot offered to an independently owned buffer);
  `not_ready` (the recorder holds no reference, is not recording, has an active rendering instance, or the
  backend has no device); otherwise the code `validate` would have answered; **Ownership/lifetime:** the
  barriers affect only the recorded list; nothing is retained beyond what the recorder already retains and
  no final state is published; **Threading:** the recorder has ONE owner thread at a time (section 4), and
  the same rule covers this call.

#### async_presentation

Presentation overlapped with the next frame's preparation. Announced by
`extension_kind::async_presentation` (0x200), answered only by a backend with a presentation path it can run
off the caller's thread -- in this tree, only when the core was created with a native window (a windowless
core answers null, and that null means "present synchronously", never "present without waiting"). Absent, the
caller's present is the blocking one from section 7 and nothing about frame pacing changes. Completion of the
returned future means the HOST present call returned, not that the GPU or the display is done with the image.

- `virtual std::shared_future<error> begin() = 0;` -- **Requirement:** snapshot the submitted frame's
  presentation state (swapchain/generation, image index, queue, present-ready semaphore) BEFORE returning,
  then perform the host present call asynchronously; at most ONE presentation may be outstanding, a queued
  presentation MUST NOT authorize overwriting its image, and `wait_idle()` plus teardown MUST drain any
  pending call (recreation must drain before it touches the swapchain); **Returns/errors:** a
  `shared_future<error>` that always becomes ready; the reference backend resolves it with `error::not_ready`
  when no frame was acquired, the frame is still being recorded, or a presentation is already queued, with
  `out_of_date` for a surface that presented but needs a rebuild, and with the device errors of section 3
  (`surface_lost`, `device_lost`, `out_of_*_memory`) otherwise; a backend MUST NOT express a failure by an
  invalid future; **Ownership/lifetime:** the backend keeps its own completion for teardown, the caller MUST
  collect the future before the next frame's submission, and it MUST keep the core alive until it has
  collected and released it (the same borrowed-capability lifetime as every other ability);
  **Threading:** the contract does not specify which threads may call `begin`; the present itself happens on
  the backend's worker, and the caller's only synchronization point is the future.

#### command_submission

Independent graphics submission with completion receipts. Announced by
`extension_kind::command_submission` (0x2000); a backend that can submit caller-owned work but cannot report
completion MUST answer null. It submits caller-owned commands only -- it MUST NOT submit or present an
acquired frame, which stays the frame context's job. A backend without it leaves the caller with
`command_context::submit`, which is the same submission without a token.

- `[[nodiscard]] virtual std::expected<submission_token, error> submit(command_buffer&) = 0;` --
  **Requirement:** submit one finished, caller-owned primary recording to the backend's graphics queue and
  hand back a receipt; provenance and recording state MUST be validated before anything is queued, the
  acquired frame's borrowed list MUST be refused, and the resource/recording retention rules are exactly
  `command_context::submit`'s (retain through GPU completion, no implicit barriers added);
  **Returns/errors:** `ok` plus a `submission_token`; a failure carries the error and NO token --
  `unsupported` for the frame's borrowed list (the reference backend's name for the refusal the contract
  requires; the contract itself names no code), `invalid_argument` for a list this backend did not hand out or
  a non-primary kind, `not_ready` for a list still recording, already in flight or with no queue, plus the
  device errors of section 3; **Ownership/lifetime:** the token is a copyable, NON-owning receipt
  (`device_id`, `sequence`); dropping it neither retains resources nor cancels work, and the backend keeps
  the submitted work and its retained resources itself; **Threading:** contract does not specify.
- `[[nodiscard]] virtual std::expected<submission_token, error> submit(std::span<shared_object<mergeable_command_buffer> const>) = 0;` --
  **Requirement:** validate the WHOLE batch and then submit it as one ordered submission (segments in span
  order, nothing submitted on a failed validation); an empty batch is refused, a segment named twice is
  refused, and the batch adds no implicit visibility barriers; the backend MUST merge however it likes as
  long as the recorded order and the batch's atomicity hold; **Returns/errors:** token on success;
  `invalid_argument` (empty batch, a handle this backend did not hand out, the same segment twice);
  `not_ready` (a segment still recording, not executable, already in flight, or no queue); device errors as
  section 3; **Ownership/lifetime:** the batch's segments are retained through GPU completion (the caller may
  drop its own references as soon as the call returns `ok`), the token is the same non-owning receipt;
  **Threading:** contract does not specify.
- `[[nodiscard]] virtual error poll(submission_token token) noexcept = 0;` -- **Requirement:** answer whether
  that submission has completed, without blocking; a backend MUST be able to keep answering for old values
  after reclaiming its native per-submission records (the receipt is not a native handle);
  **Returns/errors:** `ok` = completed, `not_ready` = still pending, `invalid_argument` for an all-zero,
  foreign-device or future (never-issued) value, and any latched device error stays that error
  (`device_lost`, `out_of_*_memory`); **Ownership/lifetime:** nothing is retained; the token does not keep the
  submission or the core alive, and the caller MUST keep the core alive while polling; **Threading:**
  contract does not specify (the reference takes its submission mutex only to snapshot state).
- `[[nodiscard]] virtual error wait(submission_token token, std::uint64_t timeout_ns) noexcept = 0;` --
  **Requirement:** wait for THIS submission only, and never while holding queue/submission locks (another
  host thread may be submitting); `timeout_ns == 0` checks without waiting, `UINT64_MAX` waits indefinitely,
  and a timed-out wait MUST be retryable rather than destructive; waiting for this token MUST NOT wait for
  later submitted work; **Returns/errors:** `ok` = completed; `timeout` = not done within the deadline
  (including the zero timeout); `invalid_argument` for an invalid/foreign/future token; latched/device errors
  as in `poll`; **Ownership/lifetime:** none retained; the core MUST outlive the call and the token is not
  persistent across processes or cores; **Threading:** contract does not specify beyond the no-lock-while-
  waiting requirement.

#### gpu_sorting

A stable ascending `uint32` key/value radix sort service. Announced by `extension_kind::gpu_sorting` (0x100)
when the backend can create a sorter for its device; a backend whose sorter is unavailable (driver, missing
support) answers null, and the caller sorts on the host or refuses rather than emulating silently.

- `[[nodiscard]] virtual radix_sorter* create_radix_sorter() = 0;` -- **Requirement:** create a sorter bound
  to THIS core's device and return it with exactly ONE owning reference (the raw-pointer return is the
  contract's spelling here, unlike the factories' `unique_object`), so the object's `release()` is the single
  drop; the backend MUST NOT share one sorter between cores; **Returns/errors:** the pointer, or NULL on
  unsupported or allocation/creation failure -- null is the only failure spelling, there is no error channel,
  and the caller MUST read it as "no sorter"; **Ownership/lifetime:** one owning reference; the caller MUST
  call `radix_sorter::release()` exactly once (wrapping it in `unique_object`/`shared_object` is the
  idiomatic drop), and the sorter MUST NOT outlive the core; its recorded operands are borrowed until GPU
  completion under `radix_sorter::record`'s own rules; **Threading:** contract does not specify.

#### device_address

Device addresses for buffers. Announced by `extension_kind::device_address` (0x1); a backend answers it when
it can report an address for a buffer it created, and it declares buffer addresses ONLY -- acceleration
structure addresses live with the tier-1 acceleration-structure objects. A buffer that was not created with
`buffer_flag::device_address` has no address to give; "asked for no address" is answered as zero, not as a
failure, and a caller MUST NOT read zero as a real address.

- `[[nodiscard]] virtual std::uint64_t buffer_address(buffer const& resource, std::uint64_t offset) const noexcept = 0;` --
  **Requirement:** answer the device address of `resource` plus `offset` bytes, computed from the buffer's
  base address at call time (the backend MAY cache the base, but the answer MUST reflect the live buffer);
  the buffer MUST be one this backend handed out and MUST have been created with
  `buffer_flag::device_address` -- the contract makes both the caller's precondition, and the answer is only
  defined for a buffer this device created; **Returns/errors:** the address, or 0 when the buffer carries no
  device-address flag (the contract's spelling of "there is none", deliberately not an error) and 0 for a
  released core; there is no error channel; **Ownership/lifetime:** nothing is retained; the address is valid
  only while the buffer lives, and the contract does not specify whether `offset` is bounds-checked, so
  staying inside the buffer is the caller's obligation; **Threading:** contract does not specify.

#### shader_group_access

Portable shader-binding-table record bytes for a ray-tracing pipeline, queried through a pipeline operand so
no native type crosses. Announced by `extension_kind::shader_group_access` (0x80) when the backend can read
shader-group handles back for a pipeline it built (in this tree: only when ray-tracing pipelines are
available). Absent, a caller cannot build a table and MUST NOT fabricate one from pipeline state.

- `[[nodiscard]] virtual error read(pipeline const& resource, std::uint32_t first_group, std::uint32_t group_count, std::span<std::uint8_t> out) const noexcept = 0;` --
  **Requirement:** copy `group_count` shader-group handles starting at `first_group` into `out`, in the
  backend's own record layout, with the record size the caller got from
  `device_capabilities::shader_binding_table().handle_size`; `out` MUST be at least
  `group_count * handle_size` bytes and the verb MUST NOT write past it; the bytes are the backend's
  (implementation-defined) records, so a caller MUST treat them as opaque and place them into a table whose
  stride and base alignment also come from `device_capabilities`; **Returns/errors:** `ok`; `invalid_argument`
  for zero groups, a group range outside the pipeline, insufficient output, or a foreign/non-ray-tracing
  pipeline (the contract names all four); `unsupported` when the backend serves no ray-tracing pipeline
  mechanism; **Ownership/lifetime:** none -- the bytes are copied into caller storage, nothing is retained,
  and the pipeline MUST stay alive during the call; **Threading:** contract does not specify.

#### descriptor_heap

The optional bindless heap service: push data, descriptor writes and heap binds, all on the general
resource/address/command vocabulary (native parameters MAY only ride an explicit `pNext`, and a request that
still chains a tag is refused `unsupported` under the tagged-structure rule of section 3). Announced by
`extension_kind::descriptor_heap` (0x2) ONLY when the heap exists and can serve: a backend MUST NOT announce
the bit and implement the verbs as no-ops, and in this tree the bit, `ready()` and the `descriptor_space` bit
are the same condition.

- `[[nodiscard]] virtual bool ready() const noexcept = 0;` -- **Requirement:** answer whether the heap can
  serve writes and binds NOW; the backend MUST NOT announce `descriptor_heap` while this can answer false (a
  set bit is a promise about service), so the answer and the bit may differ only in the direction "the heap
  broke after construction", and a verb called in that state MUST answer `not_ready` rather than writing
  anywhere; **Returns/errors:** bool, no error channel; **Ownership/lifetime:** none; **Threading:** contract
  does not specify.
- `[[nodiscard]] virtual descriptor_heap_properties properties() const noexcept = 0;` -- **Requirement:**
  report the DEVICE's heap numbers -- resource size, max resource/sampler size, the two alignments, the two
  reserved windows, the per-kind descriptor strides and the embedded-sampler limit -- as the values the
  backend itself sized and aligned against, not frozen constants; a caller derives its slot grid from them
  (the index space is `byteOffset / stride` for that kind, so a stride the backend reports and one it enforces
  MUST be the same number); **Returns/errors:** a POD by value; zeroed when there is no device; no error
  channel; **Ownership/lifetime:** none; **Threading:** contract does not specify.
- `[[nodiscard]] virtual heap_bindings bindings() const noexcept = 0;` -- **Requirement:** report the two
  heap ranges actually bound -- device address, size, reserved range offset and size, for resource and
  sampler -- and they MUST be the values the backend's own bind records (a caller computes its own addresses
  and push lanes from them, so a discrepancy is a wrong descriptor read, not an error the caller can detect);
  the reserved window is the part a combined image sampler may take its sampler from; **Returns/errors:** a
  POD by value; zeroed ranges when the heap is not ready; no error channel; **Ownership/lifetime:** none --
  the addresses name backend-owned memory and MUST NOT be cached past the heap's life; **Threading:**
  contract does not specify.
- `[[nodiscard]] virtual error write_image(heap_image_write_info const& info) noexcept = 0;` --
  **Requirement:** write ONE image descriptor at `info.offset` in the resource heap, with the descriptor type
  taken from `info.type` and the subresource from the portable `image_view_desc` (`view`), against the image's
  current state; `offset` MUST be a multiple of that kind's descriptor stride and the whole write MUST fit
  the heap; the image MUST be one this core created and MUST carry the role the type asks for (sampled view
  for a sampled descriptor, storage view for a storage one) -- a type/role/flags mismatch is SILENT at the
  API level, so refusing it is the backend's job; **Returns/errors:** `ok`; `invalid_argument` (malformed
  header, null/foreign image, view range outside the image, offset or size outside the heap); `unsupported`
  (a descriptor type the mechanism cannot hold -- combined samplers and dynamic buffers -- or a view
  role/usage that does not match the type); `not_ready` (heap not ready); `operation_failed` (the native write
  failed); **Ownership/lifetime:** the write stores a descriptor, not a reference: the contract does not
  specify any retention of `info.resource` or of `info.view`, so the caller MUST keep the image and view alive
  until every recorded use of that slot has completed, and MUST NOT reuse the slot while such use is pending;
  **Threading:** contract does not specify.
- `[[nodiscard]] virtual error write_buffer(heap_buffer_write_info const& info) noexcept = 0;` --
  **Requirement:** write ONE buffer descriptor at `info.offset` from a device address, a size and a type (the
  address is what a caller gets from `device_address::buffer_address`, so this verb crosses no buffer handle);
  `offset` MUST be stride-aligned and the write MUST fit the heap; the address must belong to a live buffer of
  this device and the descriptor's storage class has to match the shader's declaration -- a mismatch reads as
  zeros with no validation finding, which is why the caller's type is the contract's only say in the matter;
  **Returns/errors:** `ok`; `invalid_argument` (malformed header, zero address or size, an address+size that
  overflows, offset/size outside the heap); `unsupported` (a type the heap cannot hold); `not_ready` (heap not
  ready); `operation_failed` (native write failed); **Ownership/lifetime:** an address is not an owner; the
  caller MUST keep the buffer alive until every recorded use of the slot has completed;
  **Threading:** contract does not specify.
- `[[nodiscard]] virtual error bind(heap_bind_info const& info) const noexcept = 0;` --
  **Requirement:** record the heap bind (resource heap and sampler heap, with their reserved windows) into
  `info.commands`, which is what lets heap-created pipelines see the descriptors; the recorder's provenance
  and recording state MUST be validated first, pending recorder state MUST be applied before the bind is
  recorded, and a bind that a later native operation invalidates MUST be re-recorded by the backend (this
  tree's radix sort restores it after its own push-descriptor use); the verb is `const` because the heap is
  not what changes -- the command list is; **Returns/errors:** `ok`; `invalid_argument` (null/foreign recorder,
  a recorder this core did not hand out); `not_ready` (heap not ready, recorder holds no target or is not
  recording); a flushing/deferred-recording error if applying pending state fails; **Ownership/lifetime:** the
  bind affects only the recorded list, retains nothing, and the recorder MUST stay alive and recording through
  the call; **Threading:** the recorder has one owner thread (section 4).

#### non_blocking_write

`non_blocking_write` is a CPU-side write into a backend-owned image that does **not** record a command
buffer or submit to a queue. Vulkan serves it through `VK_EXT_host_image_copy`'s
`vkCopyMemoryToImageEXT`; the destination must be a backend-created colour image with
`image_flag::host_transfer`, in GENERAL layout and not the frame image. The source is tightly packed for
the requested region. The call does not establish host-write -> shader-read visibility: the caller records
that dependency before sampling the image. The ABI 59 bit is announced only when the host upload path is
available (`host_image_copy_upload_available`).

#### host_image_copy

A HOST-side copy of an image into memory the caller chose: no command is recorded, nothing is submitted, so
the caller must have made the image's writes visible to the host first (a device-write-to-host-read barrier
and a wait) -- the copy is a host operation on an image no queue is using. Announced by
`extension_kind::host_image_copy` (0x10) when the backend has a direct mechanism; a backend without one
answers null and the caller stages through a buffer instead (in this tree the mechanism is a REQUIRED device
extension, so the bit is set whenever the device exists). It extends an owned image a caller made; the
acquired frame image has its own read-back protocol and is refused here.

- `[[nodiscard]] virtual error copy_image_to_memory(image const& source, std::span<std::byte> destination, image_copy_region const& region) noexcept = 0;` --
  **Requirement:** copy `region` of `source` into `destination`, performing the copy synchronously inside the
  call with nothing recorded and nothing submitted; the destination MUST be at least as large as the region
  the backend resolves, and the region MUST be copied tightly packed (rows `extent.width` texels, top-left
  origin, layers/mips in the order asked) so the bytes are the same ones `image::get_content()` and a
  `VkBufferImageCopy`-style read-back leave; the source MUST be an owned image this core created, already
  synchronized against, and (in this tree) kept in GENERAL, which is what the capability check verified;
  **Returns/errors:** `ok`; `unsupported` (no host-copy mechanism, or the source is not a native image this
  backend can copy); `invalid_argument` (a dead image, a region or destination that does not fit -- the
  contract names the too-small destination and the non-fitting region as this code's own cases -- and, in the
  reference backend, the acquired frame's borrowed image, which this ability's own contract excludes); the
  copy verb has no `not_ready` story in the contract, and a backend that cannot copy now MUST answer
  `unsupported` rather than half-copy; **Ownership/lifetime:** nothing is retained; the
  destination storage is the caller's and MUST stay valid for the call, and the copy is complete when the
  call returns (no fence, no future); **Threading:** contract does not specify.

#### device_capabilities

WHAT THE DEVICE CAN DO, in the contract's own vocabulary, answered from what the backend already decided at
construction rather than by re-querying the API. Announced by `extension_kind::device_capabilities` (0x40)
UNCONDITIONALLY once a device exists -- a device with no mesh shader answers false, which is an answer, not an
unserved ability -- and every method MUST be answerable at that point. A fact nobody reads is dead vocabulary:
the method set is the measured set, so a new need adds a method instead of a device dump. Every answer is a
semantic fact ("this device can run X", "this is the number the backend sizes against"), not a native limit.

- `[[nodiscard]] virtual bool mesh_shader() const noexcept = 0;` -- **Requirement:** answer whether this
  device can run a MESH pipeline, meaning the backend ENABLED the mechanism (the engine gates its mesh
  evaluation and dispatch path on it), not merely that the hardware might support it; it MUST agree with what
  the backend enabled at device creation; **Returns/errors:** bool, no error channel; `false` on a released
  core; **Ownership/lifetime:** none; **Threading:** contract does not specify.
- `[[nodiscard]] virtual bool ray_query() const noexcept = 0;` -- **Requirement:** answer whether this device
  can run RAY QUERIES, the gate for the ray-traced lighting path and for deciding the acceleration-structure
  input usage on a buffer; false MUST mean "do not use the path", never "maybe"; **Returns/errors:** bool, no
  error channel; `false` on a released core; **Ownership/lifetime:** none; **Threading:** contract does not
  specify.
- `[[nodiscard]] virtual std::uint32_t max_push_constants_size() const noexcept = 0;` --
  **Requirement:** answer the USABLE byte capacity of `command_recorder::push_constants` under this backend's
  active binding mechanism (this is not necessarily a native push-constant device limit, and the engine's
  parameters must fit it); a caller MUST validate every push range against this number
  (`validate_push_constants_range`) rather than a hard-coded constant, and MUST treat 0 as "no push path";
  **Returns/errors:** the byte capacity; 0 when unavailable or with no device; no error channel;
  **Ownership/lifetime:** none; **Threading:** contract does not specify.
- `[[nodiscard]] virtual std::uint32_t graphics_queue_family() const noexcept = 0;` -- **Requirement:** answer
  the queue family the backend's GRAPHICS queue belongs to; anyone creating a command pool/allocator outside
  the contract MUST name this family, and only the backend knows it (a caller MUST NOT re-derive it by
  comparing the device's families against queue handles); the answer MUST be constant for the core's life;
  **Returns/errors:** the family index; 0 when there is no device (indistinguishable from family 0, which the
  contract does not resolve); no error channel; **Ownership/lifetime:** none; **Threading:** contract does not
  specify.
- `[[nodiscard]] virtual shader_binding_table_properties shader_binding_table() const noexcept = 0;` --
  **Requirement:** answer the three numbers a shader-binding table is built from -- bytes per shader-group
  handle, the alignment/stride between handles, and the table's own base alignment -- so a caller that is not
  a pass (and holds no session context) can size and align a table; the values MUST be the ones the backend's
  own ray-tracing pipelines expect, and the handle size MUST match what `shader_group_access::read` writes
  per record; **Returns/errors:** a POD by value; all-zero is the "no ray tracing" spelling, and a caller must
  read zero as "build no table"; no error channel; **Ownership/lifetime:** none; **Threading:** contract does
  not specify.
- `[[nodiscard]] virtual std::uint64_t acceleration_structure_scratch_alignment() const noexcept = 0;` --
  **Requirement:** answer the alignment an acceleration structure's scratch offset must obey; the
  acceleration-structure module reads it when sizing and offsetting scratch storage, so it MUST be the
  backend's real requirement and MUST be a power of two (a non-power-of-two answer would make alignment
  checks meaningless); **Returns/errors:** the alignment in bytes; ZERO means "the device did not answer",
  which callers already treat as 1, so a backend MAY answer 0 and MUST NOT answer a wrong nonzero value
  instead; no error channel; **Ownership/lifetime:** none; **Threading:** contract does not specify.
- `[[nodiscard]] virtual std::uint64_t max_acceleration_structure_instances() const noexcept = 0;` --
  **Requirement:** answer the largest number of instances one acceleration structure may hold, so the caller
  can refuse an over-large frame before the driver fails (the module refuses rather than asking the driver to
  fail); 0 means "no answer/no mechanism" and MUST NOT be read as a legal capacity of zero that silently
  accepts nothing; **Returns/errors:** the instance count, 0 with no device; no error channel;
  **Ownership/lifetime:** none; **Threading:** contract does not specify.
- `[[nodiscard]] virtual bool compute_indirect() const noexcept {` -- **Requirement:** answer whether this
  backend serves INDIRECT compute dispatch (the engine's software-visibility pass gates its indirect dispatch
  on it); the declaration head above is the ABI contract's spelling and the body is the in-class default
  `return false;`, so a backend that serves indirect compute MUST override it and MUST NOT inherit the
  conservative default; this capability is independent of `buffer_int64_atomics()`; **Returns/errors:** bool, no error
  channel; **Ownership/lifetime:** none; **Threading:** contract does not specify.
- `[[nodiscard]] virtual bool buffer_int64_atomics() const noexcept {` -- **Requirement:** answer whether this
  device has shader int64 AND storage-buffer int64 atomics ENABLED (both halves: 64-bit shader integers and
  the buffer-atomics feature), which is what a shader using 64-bit buffer atomics needs; the in-class default
  body answers `return false;` -- the conservative default for a backend that did not check -- so a backend
  MUST override it with its own device answer rather than inherit the default silently;
  **Returns/errors:** bool, no error channel; `false` on a released core; **Ownership/lifetime:** none;
  **Threading:** contract does not specify.

#### descriptor_space

ABI 55 adds a fixed shader-binding profile alongside the existing dynamic texture-group positions.
This is logical slot access, not a physical-offset alias or dynamic layout registration API. The profile
preserves the current shader ABI. The legacy descriptor_heap interface remains a separate low-level service.

- `fixed_slots() const noexcept -> descriptor_slot_range`: a core-lifetime interval `{base,count}` of
  shader slots in space 0; empty means this optional profile is unsupported, even when group operations
  and `ready()` work. Vulkan exposes [16384,18432), disjoint from dynamic group storage at 32768 and above.
  Runtime MUST verify that the interval covers its shader profile, not infer this from ready() alone.
- `write_image(descriptor_image_write const&)`: write a sampled/storage image at a fixed slot. Requires
  an explicit valid view, compatible image usage, in-bounds mip/layer range and same-core live image.
- `write_buffer(descriptor_buffer_write const&)`: write a uniform/storage buffer object with nonzero
  resource-relative offset/size range. Validate same-core provenance, native address capability, declared
  usage/flags, bounds without overflow, alignment and device range limits. Physical addresses and descriptor
  offsets are resolved internally. This shader profile lowers uniforms through native storage descriptors;
  uniform roles must satisfy both logical uniform and native storage alignment/range limits. Storage reads
  may use storage allocations, explicitly storage-enabled buffers, and uniform allocations that support it.
- `write_acceleration_structure(descriptor_acceleration_structure_write const&)`: publish a same-core
  live AS object, resolving its address/range internally. Build ordering/completion remains the caller's job.
- `bind(descriptor_bind_info const&)`: bind backend-owned descriptor storage to a same-core recorder in
  recording state. This flushes prior lazy operations before binding, as the existing heap bind does.

All four requests have tagged headers; wrong tags/short coverage return `invalid_argument`, non-null next
chains return `unsupported`. Unknown spaces, slots outside the fixed interval, null/foreign objects and
bad ranges return `invalid_argument`; unsupported descriptor types/usages/profile return `unsupported`.
Unavailable bind storage or a non-recording target returns `not_ready`. Default implementations return
`unsupported` and the default interval is empty; group-only backends need not implement the fixed profile.
Encoding failure may return `operation_failed`. Validate requests before mutation: rejected validation MUST
preserve the previous descriptor, without promising transactional recovery from a driver encoding failure.

These are synchronous host writes with borrowed objects. Callers serialize updates, retain operands through
GPU completion and wait for previous uses before replacing descriptors. There is no implicit GPU barrier,
allocation/versioning, ownership retention or concurrency promise. Fixed slots need no generation because
their reservation lasts for the core lifetime; dynamic group positions retain their existing generations.
Adding these virtuals changes the descriptor_space vtable: ABI 54 and earlier binaries MUST be rejected.

The backend's DESCRIPTOR SPACE, addressed by logical positions instead of byte offsets: where an immutable
`texture_group` lives, and one portable sampled-descriptor write. Announced by
`extension_kind::descriptor_space` (0x1000), and ONLY by a backend that can both position a texture group and
write into it -- in this tree the same condition as `descriptor_heap`, so a backend without a descriptor heap
does not answer this kind at all, and the caller's move is the tier-1 texture-group path or a named refusal.
It exists BESIDE the heap, not instead of it: the heap answers the device's numbers, this answers the
engine's vocabulary. A caller stores the frozen `descriptor_position` (16 bytes, by value) instead of leaking
an absolute heap slot into material data.

- `[[nodiscard]] virtual bool ready() const noexcept = 0;` -- **Requirement:** answer whether the space can
  serve positions and writes now; as with the heap, the bit MUST NOT be announced while this can answer
  false, and a verb called in that state MUST answer `not_ready` rather than write anywhere;
  **Returns/errors:** bool, no error channel; **Ownership/lifetime:** none; **Threading:** contract does not
  specify.
- `[[nodiscard]] virtual descriptor_space_properties properties() const noexcept = 0;` --
  **Requirement:** answer the BACKEND's own shape -- live immutable groups the space can hold, sampled slots
  it can hold, the shader-facing record one group occupies, and the stride of one sampled descriptor -- so a
  caller sizes its material records from answers rather than assumptions (a second backend may size its table
  differently, which is why these are not frozen contract constants); **Returns/errors:** a POD by value;
  zeroed with no device; no error channel; **Ownership/lifetime:** none; **Threading:** contract does not
  specify.
- `[[nodiscard]] virtual descriptor_position position(texture_group const& group) const noexcept = 0;` --
  **Requirement:** answer the position the live group was registered at (`space`, `base`, `count`,
  `generation`); the position is STABLE for the group's life because the group is immutable, so repeated
  calls MUST answer the same values, and the generation MUST change only if the backend reuses the position
  for another group (that is what makes a stale position distinguishable from a live one); anything this space
  does not own -- a foreign group, an already-released one, another core's -- MUST answer the EMPTY position
  (`count == 0`) instead of casting the reference; **Returns/errors:** the POD by value; the empty position
  for anything unowned; no error channel; **Ownership/lifetime:** by value, nothing retained; the position
  does NOT keep the group (or its images) alive, so a caller that stores one MUST keep the group's ownership
  alive for as long as it plans to use the position; **Threading:** contract does not specify.
- `[[nodiscard]] virtual error write(descriptor_write const& info) noexcept = 0;` --
  **Requirement:** write ONE sampled descriptor into slot `index` of the group positioned at `info.where`
  (`index` MUST be < `where.count`) from the portable image plus optional `image_view_desc` (`view == nullptr`
  means the group profile's own view: all mips, one layer, sampled role), and publish the group's
  shader-facing record atomically with the write -- the position's generation MUST still be live (same space,
  the profile's width, a nonzero base, the group's current generation), because the generation is the only
  thing that turns a stale position into a refusal instead of a mis-cast; the write is HOST-SIDE and MUST
  happen before the position is consumed by recorded work; **Returns/errors:** `ok`; `invalid_argument`
  (malformed header, a stale/foreign/never-registered position, `index >= where.count`, null resource, an
  image this core did not create); `unsupported` (a descriptor type the space does not serve, or an explicit
  view that is not the group profile's shape -- the v1 surface is sampled writes); `not_ready` (no device);
  **Ownership/lifetime:** the write stores a descriptor and retains nothing, so the caller MUST keep the image
  (and any explicit view) alive while recorded work can read the slot, and MUST keep the group alive for as
  long as the position is used; **Threading:** contract does not specify.

---

### 8.2 Open/unverified

The historical extension audit is resolved by the ABI 54 header clarifications and section 6.1: sampled-only
descriptor-space writes, host-copy color/packing, shader-group handle sizing, frame-image epochs, asynchronous
presentation single-flight and the two-way ability-bit rule are normative. They must not be treated as
unspecified merely because a per-method paragraph predates those clarifications. Remaining contract choices
are centralized in section 9.2.

Conformance checks must distinguish discovery from execution: a bit and a non-null interface prove the backend
announces a mechanism; they do not prove a particular resource, range, device limit or recording state is valid.
Test capability refusal as well as the successful device path. The optional device-probe lane complements the
headless contract tests; it does not remove the need to test backend-specific native boundaries.

## 9. Closure: resolved, removed, and still unspecified

### 9.1 Resolved by the ABI 54 clarifications

The following previously open items now have normative text (headers section plus section 6.1): the ownership
`release()` rules; `radix_sorter::requirements`/`record`/`release`; `copy_buffer` ranges; vertex/index offsets;
rendering-scope pairing/nesting; `clear_color_image` component order; `mark_gpu_timing` text lifetime;
`wait_idle`/`main_context`; frame-view windows; `assign_texture_group`'s `error*` protocol; `command_buffer::`
execute batch rules by reference to `submit(span)`; descriptor-heap retention/stride; descriptor-space write
surface; host-image-copy aspect/packing/source state; shader-group record sizing; scoped synchronization's
depth/stencil refusal and error mapping; `retain_image` identity/epoch; async-presentation single-flight;
`compute_indirect` doc correction; the two-way ability-bit rule; `device_capabilities` boolean defaults.

### 9.2 Genuinely unspecified (a second backend MUST NOT rely on it)

- `command_buffer::execute`'s own error set beyond the `submit(span)` rules (foreign segment, already-recording
  target, segment from another core): use the submit/execute contract and treat any refusal as non-success.
- `command_recorder::use` for an image other than the frame image: only the frame-image case is specified.
- Exact error code for a wrong-state `dispatch`/`draw_mesh_tasks` beyond "latched failure; nothing is published".
- Thread safety of extension ability verbs: the contract does not grant concurrent calls; callers serialize
  unless a backend documents otherwise.
- Exact per-backend descriptor/limit numbers: always query `device_capabilities`/`properties()`; never infer them.

### 9.3 Removed interfaces and future additions

ABI 54 removes `query`, `query_desc`, `resource_factory::create_query`, `swapchain_desc`,
`frame_context::create_swapchain`, and the unused `mesh_shader` extension interface. Its bit 0x4 remains
retired and MUST NOT be announced or reused; querying it returns null. `frame_swapchain()`, GPU timing,
`device_capabilities::mesh_shader()` and recorder mesh draw verbs remain supported under their own contracts.

The resource_factory/frame_context vtables changed at ABI 54; pre-54 backends MUST be rejected before use. The active
extension enumeration now contains twelve entries. A future query or multi-window API requires a new design,
not resurrection of these incomplete placeholders. Apply section 5's actual layout/vtable rules to that design.

### 9.4 Second-backend conformance checklist

1. Exports `deren_make_api_core`; refuses ABI mismatch before touching device state.
2. Implements every mandatory interface in section 7; returns null for every extension it does not implement and
   sets no ability bit it cannot serve.
3. Satisfies every clause in section 8 for implemented extensions, including the section 6.1 clarifications.
4. Passes the ownership/ABI tests and its relevant device probes; no failed recording is published.
5. Releases native resources at last reference; callers keep the core and DLL alive through all handles and
   retained work.
6. Reports capabilities/extensions truthfully, refuses unsupported operations by name, and documents any
   threading guarantee beyond the caller-serialized default.
7. Runs the device-probe lane (`scripts/gates/check_device_probes.ps1`) or an equivalent on a device-capable
   machine before claiming conformance.


## 9. Lazy resources (ABI 56)

`lazy_data` is `std::shared_ptr<const utility::vessel::vector<std::uint8_t>>`. A backend MUST retain this
shared owner without copying the payload. Other aliases MUST NOT mutate it until the resource's last
reference is released. Source bytes remain retained after upload. This is a deliberate shared-data
exception to ordinary borrowed creation descriptors; source and backend use the matched toolchain ABI.

`lazy_buffer_desc` and `lazy_image_desc` hold `struct_size`, an ordinary resource description and `data`.
Both outer and nested descriptions MUST cover their current structure. `data` MUST be non-null/nonempty;
`resource.initial_bytes` MUST be empty. Buffer bytes MUST equal size; image bytes MUST equal its packed
normalized mip/layer payload. Invalid descriptions return `invalid_argument` before GPU allocation.

- `resource_factory::create_lazy_buffer(lazy_buffer_desc const&)` returns
  `expected<unique_object<lazy_buffer>, error>`; its default is `unsupported`.
- `resource_factory::create_lazy_image(lazy_image_desc const&)` returns
  `expected<unique_object<lazy_image>, error>`; its default is `unsupported`.
- `lazy_buffer::prepare() noexcept` and `lazy_image::prepare() noexcept` bring the native resource up from
  the retained source; they answer `ok` when ready (idempotent), serialize with a concurrent prepare on the
  same object, and default to `unsupported`. `prepare()` is the explicit prefetch entry point the engine's
  upload worker uses before a frame needs the resource.
- `lazy_buffer::state() const noexcept` and `lazy_image::state() const noexcept` report `lazy_state`
  (`unloaded`, `loading`, `ready`, `failed`) without loading. Queries and logical image-view creation
  MUST NOT upload. These types derive from buffer/image and use ordinary supported bindings.

Vulkan initially supports device-only vertex/index/uniform/storage buffers (device_address/storage
allocation flags), and sampled color images (optional cube compatibility). Mapped buffers, externally
extracted device addresses, storage/attachment images and direct copy/clear/indirect/build/native uses
of lazy resources are unsupported. Internal descriptor buffer addresses remain valid after loading;
public device_address returns zero for lazy buffers. Unsupported uses MUST NOT record invalid handles.

Logical bind setters, resource/texture-group construction and logical descriptor-space writes MUST NOT
materialize lazy resources. First surviving native binding, explicit resource barrier or descriptor-space
bind synchronously prepares its resources before recording native use. This includes scoped barriers;
a barrier-only recording can load. Group/fixed-profile binding conservatively demands all members;
there is no shader-index discovery or visibility-based streaming implicit in this API.

Concurrent first users MUST serialize physical materialization and observe ready only after initial
upload completes. Failure latches `failed` with no implicit retry; the recording reports the error and
cannot be submitted. Successful independent resources can remain ready after a later failure. Backend
helpers currently collapse physical allocation/upload refusal into `operation_failed`; factory validation
and host-object allocation errors are returned separately. No whole-device wait_idle is introduced.
If an upload was submitted but its wait fails without device loss, completion is unknown: the Vulkan
backend uses fast_panic rather than freeing/recycling potentially pending resources. This follows the
existing fatal policy for resources whose final safe wait fails; no recoverable quarantine is added.

Lazy resources MUST survive their recorded and pending GPU uses, including retained secondary commands
and reusable executable recordings. Source retention is transitive through lazy views and groups. Logical
fixed slots retain lazy objects until replaced; invalid writes leave the prior binding unchanged. Slot
replacement follows the existing caller synchronization contract. Ordinary borrowed-resource behavior
is unchanged. Objects MUST NOT outlive their core. There is no automatic eviction, asynchronous loading,
retry, file I/O or decoder in this increment.


## 10. Explicit lazy unload (ABI 57)

`lazy_buffer::unload() noexcept` and `lazy_image::unload() noexcept` return `error`. They MUST preserve
shared source data, immutable resource metadata and logical object identity. Success releases this
object's physical allocation reference and publishes unloaded; the next supported native demand
reloads the original initial bytes. GPU modifications are not saved back to the source.

- `ok`: an unloaded resource (idempotent), or a ready resource released safely.
- `not_ready`: materialization/dependency registration currently owns its mutex, or a ready resource
  has live dependency leases. The call MUST NOT wait for uploads, recordings or GPU completion.
- A failed resource returns its stored failure and remains failed. This method is not a retry API.
- Calling the implementation on an ordinary eager resource returns unsupported; normal buffer/image
  interfaces do not themselves expose unload.

The initial Vulkan policy is conservative. Live logical image views, resource/texture groups, fixed
logical descriptor slots, recorded commands and pending submissions prevent ready-resource unload.
Callers must release/reset/replace those dependencies first, including reusable executable recordings
whose GPU submissions have already finished. Ordinary shared ownership of the logical object does not
by itself prevent unloading. There is no transparent descriptor-generation invalidation or group rebuild.

The backend MUST acquire dependency leases under the same mutex as materialization/unload, and retain
logical object lifetime until each lease is released. Unload MUST NOT free native resources still named
by recordings or binding caches. Raw legacy descriptor_heap image writes reject lazy images; tracked
logical bindings use a backend native writer with retained dependencies. Fixed-slot replacement still
requires the existing caller synchronization. Physical allocation/reference release is not a guarantee
that VMA backing blocks or process VRAM usage shrink immediately; image allocations can also be shared.

The GPU radix-sort extension MUST demand lazy buffer operands and retain their allocation leases before
reading native handles. Its recorded and pending uses obey the same unload restrictions as core commands.

The uint8_t shared source remains retained even after successful unload. No background residency manager,
automatic eviction, source mutation tracking, GPU readback or whole-device wait_idle is added.

## 11. Frame checkpoint (ABI 58)

`frame_checkpoint` is an optional extension (bit14). Its synchronous `checkpoint_and_wait()` operates
only on the current recording frame outside rendering. It flushes/closes the prefix, submits it to
graphics, waits for prefix completion, releases prefix recording leases and reopens the same borrowed
recorder. Pipeline, push and binding state must be rebound by subsequent passes. Acquired-image
identity, timing-query data, frame-ring position and final-frame completion remain unchanged.

A successful return also completes previously submitted graphics work. Publication of shared scene
descriptors requires externally serialized recording/submission and no consumers on other queues.
An acquired window semaphore is consumed once by the first checkpoint (or final submit if absent);
only the final submission signals frame-slot completion and presentation. A checkpoint does not present.

Outside an active recording or inside rendering, return not_ready before ending/submitting. Recording
errors propagate. Fence allocation failure preserves the recording. Submit/reset/reopen failures return
errors and must not be treated as a usable continuation. A final prefix wait failure uses fast_panic:
the backend cannot reset/free the prefix with uncertain GPU completion. This increment does not add
a recoverable frame-abort protocol. Device teardown/recreation remains the recovery for failed frames.
Timestamp intervals spanning a checkpoint include GPU idle during the CPU feedback/upload phase;
they are elapsed frame intervals, not isolated shader execution times.
