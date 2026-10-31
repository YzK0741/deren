# The dynamic API: the contract, the by-name-loaded backends, and the run-time package

The graphics API reaches this renderer through a contract, never through Vulkan types. The executable
links no graphics library: it loads a backend DLL by name, hands it a contract it implements, and talks
to the device through that interface alone. Everything a second backend has to satisfy is on this page.

**Measured at this version** (on the artifacts, not read from the code): `deren.exe` imports **no**
graphics API, the engine's object files carry **no** unresolved `vk*` symbol, and **no** engine source
names the graphics API. A backend *module* is not importable by the engine either - the loader is the
only door, and `scripts/checks/check_backend_boundary.py` is the gate that keeps it that way.

## Log and fatal-report files

All build types write normal logs and `[ERROR]` messages asynchronously to `deren-debug.log` in the
working directory; startup rotation uses `deren-debug.log.old`. No logging route falls back to a
console. If the ordinary file fails, the worker attempts the emergency file instead.

`panic` first writes its bounded message and source location directly to `deren-crash.log`, then raw
stack records and best-effort readable symbols. It never initializes or locks the async sink; an
existing sink gets up to 250ms of atomic-prefix polling before termination. This limits log waiting,
not kernel file I/O or the symbolizer's separate budget. `fast_panic` remains bounded plain text only,
without formatting, stack capture, symbols or waiting for logs. Both files are best effort if storage
is unavailable. The native emergency handle lives until process exit; paths follow the working
directory at first open. POSIX signal reports only use output preopened during handler installation.

`wait_log_all()` captures the submitted sequence under the queue lock and waits for that prefix's
write/flush attempts to finish. Later messages do not extend the wait; a worker cannot wait for itself.
Explicit generic printing APIs and command-line diagnostic tools retain their normal stream behavior.

## Utility and engine error policy

`deren.utility` (module interface 3.0.0) and `deren.utility.shared` have no RHI dependency.
Generic assertions, logging, allocation and panic remain in utility. RHI-specific outcome policy
belongs to `deren.engine.outcome`, provided by `engine_outcome_core`.

Consumers of the former `deren::utility::{result, verdict, pass_success, pass_failure, fatal,
propagate, enforce}` must import `deren.engine.outcome` and use `deren::engine::outcome::` instead.
Code naming RHI types also imports `deren.promise.rhi` explicitly. No compatibility re-export is
provided, so a utility-only consumer does not need the renderer contract. Classification, error
rewriting and fatal behavior are unchanged; this source interface change does not alter the RHI ABI.

Utility source interface 3.0.0 also replaces optional/unbounded binary reads with
`read_binary_to_vector(path, max_bytes)` and `read_binary_to_string(path, max_bytes)`, both returning
`std::expected<the original vessel container, deren::utility::file_error>`. The required limit bounds
the accepted payload, not allocator bookkeeping or string capacity overhead. Use `file_error_name`
and `.cause` for diagnostics; callers decide whether an I/O failure is fatal. No RHI types are involved.

## The contract: `deren.promise.rhi`

`source/promise/rhi/` is the contract, and BOTH sides of the boundary compile it: the engine's targets and
each backend DLL read the same interface unit instead of a shared header, which is what keeps the two
compilations from drifting.

| unit | what it declares |
| --- | --- |
| `deren.promise.rhi` | the front door; it re-exports the partitions below |
| `:api_core` | the virtual surface a backend implements: object creation, command recording, frame walking, `abilities()`, `query_extension<T>()` |
| `:contract` | the value vocabulary: handles, descriptors, errors (`error`, `error_info`), the object model |
| `:core_desc` | `create_info`, the ONE creation structure - the program fills it and the backend builds its context from it |
| `:extension` | the optional interfaces and the queries that hand them out |

The contract is versioned by `rhi::abi_version` (59 today), and a change to it is one of three kinds:
**appending a slot to an existing interface bumps the number**, and so does **adding a field to a
by-value structure** - its layout is what both halves pass, even when a `struct_size` guard lets the
backend keep the old default; abi 45 (the `command_buffer_begin_info::inherit_descriptor_heaps`
append) is exactly that case. **Adding a new ability or extension does not** bump it - a new capability
is a new query, and a backend that does not implement it says so - and neither does a NEW type or a free
helper function (a `constexpr`/`inline`/template declaration), because nothing already crossing the
boundary changes shape. The GUI plugin boundary versions separately: `gui::gui_abi_version` is 5 today,
and the two numbers do not imply each other.

## Resource memory statistics (ABI 59)

`resource_factory::memory_statistics()` returns a coherent value snapshot of buffer, image and combined
allocation counters: current and peak bytes, device-local bytes and allocation counts. The default
answers `supported == false`. ABI 59 appends this vtable slot, so host, backend and plugins must be
rebuilt together; the counts are backend allocation accounting, not texture payload or WDDM residency.
See the RHI standard for the per-counter rules.

## Frame checkpoint (ABI 58)

`frame_checkpoint` is the optional extension (bit 14) whose synchronous `checkpoint_and_wait()` closes,
submits and waits for the current recording prefix outside rendering, releases the prefix leases and
reopens the same borrowed recorder. Pipeline, push and binding state must be rebound afterwards; only
the final submission signals frame-slot completion and presentation. See the RHI standard section 11
for the timing-query, acquired-semaphore and failure semantics.

## Explicit lazy unload (ABI 57)

`lazy_buffer::unload()` / `lazy_image::unload()` preserve the shared source and resource description,
release the physical allocation reference, and return the object to unloaded for later reload. They
return not_ready while loading or while views, groups, fixed descriptor slots or recorded/pending
commands retain dependencies. Reset/release those dependents first; this first version does not
invalidate their native caches transparently. Failed resources remain failed. ABI 56 backends must be
rebuilt with the host. See RHI standard section 10 for nonblocking and ownership rules.

## Lazy resources (ABI 56)

`core.resources->create_lazy_buffer` and `create_lazy_image` accept descriptions with
`shared_ptr<const utility::vessel::vector<uint8_t>>` payloads. They retain the shared source without
copying it, defer GPU allocation/upload until actual native-command demand (including barriers), and
report unloaded/loading/ready/failed state. Logical views, groups and descriptor writes remain lazy;
container binding conservatively demands every member. No scene migration or automatic eviction is
included. ABI 55 and older backends must be rebuilt with the host. See the RHI standard section 9.

## Core subviews (ABI 50)

ABI 55 extends `descriptor_space` with fixed shader-slot writes for image, buffer and acceleration-structure
objects plus logical binding. Runtime no longer computes physical descriptor offsets or constructs address
ranges for these writes; Vulkan resolves and validates them internally. The fixed profile is optional and
queried separately from texture-group support. Old ABI 54 DLLs must be rebuilt. The update/lifetime rules
are stated in the RHI standard; the dated checkpoint record is not distributed with this repository.

The root owns the device and exposes three borrowed public members with ordinary names:

```cpp
auto buffer = core.resources->create_buffer(desc);
auto commands = core.commands->make_command_buffer({});
auto result = core.commands->submit(*commands); // independent primary
// After recording and ending the acquired frame's commands:
auto frame_result = core.frames->submit();      // current frame, no buffer argument
```

| member | responsibility |
| --- | --- |
| `resources` | resource factories and texture groups |
| `commands` | independent command allocation and primary/batch submission |
| `frames` | swapchains, frame commands/images/readback, walker, profiler, submit and present |

`api_core` retains release, ABI, capabilities/extensions, native device context and device-wide idle.
`context_view<T>` is a pointer-sized wrapper: callers can use `->`, `get()` and a boolean test,
but cannot assign, reset or rebind it. It owns nothing and must not outlive its core. Const access
protects the binding while still allowing the context's device operations.

A derived backend can implement the interfaces as bases or own context objects as members. In its
constructor body, after those bases/members have been constructed, it calls
`bind_views(resource_context, command_context, frame_context)` once before publication. There is no
`const_cast`, mutable binding or extra allocation per view. Vulkan implements all three interfaces
on the same core allocation. Context methods must not be used before binding.

Independent submission rejects the borrowed current-frame primary with `invalid_argument`;
`frames->submit()` uses the current frame protocol. This preserves command/resource retention,
frame waits and token completion rules. Offscreen cores also expose the frame view; presentation
still returns `unsupported`. The split changes root layout/vtables: rebuild executable, backend
and plugins together for ABI 50. The previous root factory/frame/submit entry points are removed.

## General resource groups (ABI 51)

`resource_factory::create_resource_group` creates an immutable shared resource composition; an existing
`texture_group` is one member kind alongside buffer ranges, image views, samplers and top-level AS.
Pipelines declare logical numbered bindings and shader parameter locations; `load_resource_group`
validates a complete matching group and retains it for recorded/pending work. Backend descriptor
allocations remain private. See [resource groups](resource_groups.md) for usage, ownership and limits.
ABI 51 itself recorded immediately; deferred/lazy state recording arrived in ABI 53 (see the ABI 53
section below). Resource groups originated in ABI 51 and the subview split in ABI 50. The ABI 54
removal retired the reserved factory-slot surface; current binaries use ABI 59.

## Removal of incomplete interfaces (ABI 54)

The incomplete `query`/`query_desc`/`create_query` and `swapchain_desc`/`create_swapchain` surfaces are removed.
The unused `mesh_shader` extension is also removed; its bit 0x4 remains retired and answers null.
The frame's `swapchain`, GPU timing, mesh capability query and tier-1 mesh draw verbs are unchanged.
Resource/frame vtables changed: rebuild the executable, backend and plugins together; ABI 53 is rejected.
The active extension list contains twelve entries and its mask is 0x7FD3.

## Deferred recording application (ABI 53)

`command_recorder::apply_operations()` applies the operations a recorder has buffered to the backend in
order and preserves the resulting state for later recording. `end_recording()` applies them automatically, so
callers that never use the postponed form keep the previous behaviour. A deferred validation error makes the
recording non-executable and must be reported; a recorder that cannot defer returns `error::unsupported` and
applies operations immediately. Borrowed pipeline/buffer owners must stay alive through pending recording and
GPU completion. Immediate/native interop boundaries flush pending operations before the native call, and
`command_buffer::execute`/`mergeable_command_buffer` forwarding keeps the same order. The API change is one
appended virtual on the shared `command_recorder`, so backends without deferral remain valid and simply
advertise it through the default `unsupported` result. Push constants and resource-group/texture-group
loads become queued logical state under ABI 53: a compatible group is retained across a pipeline switch and
the backend validates/republishes it at the next consumer, errors surface at apply/end, and a re-bound same
pipeline preserves the loaded compatible group.

## Unified shader parameters (ABI 52)

Push shader parameters through the common recorder, for primary, frame and mergeable commands alike:

```cpp
auto* capabilities = rhi::query_extension<rhi::device_capabilities>(core);
auto capacity = capabilities ? capabilities->max_push_constants_size() : 0u;
auto result = commands.push_constants(offset, std::as_bytes(std::span(&parameters, 1)));
```

`push_constants` returns an error and consumes the bytes before return. Under ABI 53 the payload is queued
as logical state (ABI 52 recorded it immediately); it
requires recording, nonempty four-byte units and a range within the reported capacity. A pipeline
need not already be bound. When one is bound, application bytes cannot overlap its resource-group
handle fields or legacy texture-group token. Internal group publication owns those protected bytes.

The public descriptor-heap push entry, request structure and push-limit field are removed. The
structure_type value for the retired heap request remains reserved. The generic range helper is
`validate_push_constants_range`. Query usable capacity through device_capabilities, not descriptor
storage: Vulkan uses maxPushDataSize for its active native mechanism and reports zero if unavailable.
It continues using vkCmdPushDataEXT internally; no second public Vulkan-style spelling is needed.
Other implementations can use their native constant mechanism or return unsupported explicitly.

CPU probes capture the recorder operation directly. Engine parameter packing and all resource/group
lifetime rules are unchanged. ABI 52 itself did not implement lazy state, adjacent-write merging or changes
to draw/dispatch return types; ABI 53 now queues push/group state and applies it at explicit/native
boundaries (see the ABI 53 section above). ABI 52 changes recorder/heap vtables and heap-property layout,
so rebuild application, backend and plugins together. Developer-only controls for this mechanism are
`DEREN_RENDER_OPERATION_STATS` (per-frame request/lowering counters) and
`DEREN_RENDER_OPERATIONS_UNOPTIMIZED` (diagnostic: disables simplification and native-state skipping; logs
once when armed). Neither is a product option.

## Recording and submission (ABI 49)

`command_recorder` declares the shared recording lifecycle and verbs. Both owners expose them
directly, so callers still write `buffer->draw(...)`; there is no extra recording view.

| contract | recording | individual submission | receive `execute(batch)` |
| --- | --- | --- | --- |
| `command_recorder` | yes | no | no |
| `command_buffer` | inherited | yes | yes |
| `mergeable_command_buffer` | inherited | no; submit as a batch | no |

Heap bind/push requests carry one `command_recorder* commands`. Scoped synchronization, radix sorting
and the mesh dispatch extension also accept the common recording interface. This does not grant
submission permissions: factories and submit/execute inputs keep their specific owner types.
The engine's recording target retains typed owners and exposes `get()` for common recording calls.

Vulkan resolves extension targets against the core's live registries before casting. A foreign target,
including one with an identical concrete name, returns `invalid_argument`; an owned target outside
recording returns `not_ready`. Heap pushes protect the pipeline's reserved group-token bytes for both
primary and mergeable recording. Pool ownership, recorded-resource retention and pending submission
rules are unchanged. The existing internal mergeable adapter still delegates to its retained secondary;
this change shares the public vocabulary and target validation without rebuilding submission machinery.

ABI 49 changes the inheritance and extension signatures/request layouts. Rebuild the executable,
backend and GUI plugin together; the backend factory rejects mismatched RHI ABI versions.

## Submission completion values

The optional `command_submission` extension submits caller-owned graphics commands and returns a
copyable, non-owning `submission_token { device_id, sequence }`. It supports an ordinary command
buffer or an ordered mergeable batch; it does not submit/present the acquired frame. Existing
`core.commands->submit(...)` provides submission without a completion token. A backend without this
extension returns null from the query. The extension itself did not require an ABI bump; the subsequent
subview split uses RHI ABI 50 and preserves this extension.

```cpp
auto* completion = rhi::query_extension<rhi::command_submission>(core);
if (!completion) { /* use the backend's existing submission path */ }
else {
    auto submitted = completion->submit(*commands);
    if (!submitted) { return submitted.error(); }
    auto token = *submitted;
    auto ready = completion->poll(token);             // ok or not_ready, or an error
    auto waited = completion->wait(token, 1'000'000); // at most 1 ms; timeout is retryable
}
```

Only a successful submission produces a token. All-zero, foreign-device and future sequences return
`invalid_argument`. Polling pending work returns `not_ready`; a timed wait returns `timeout`.
`UINT64_MAX` requests an indefinite wait. Native device loss remains `device_lost`, never a successful
completion. Submission/poll/wait failures can also report memory or other device errors.

Keep the originating core and its backend alive while using its tokens. Copying or dropping a token
neither retains resources nor cancels work. The existing command/secondary/group retention contract
still applies; other referenced resources must remain alive until their GPU use is finished. Tokens
do not add memory barriers: readback still needs the proper device-write-to-host-read transition.
These values are process-local receipts, not persistent identifiers or tamper-proof capabilities.

Vulkan lazily creates one completion timeline per core. Successful tracked submits signal increasing
values, without creating one native fence per token. Native pending records are reclaimed normally;
the timeline still answers queries for old completed values. Waiting releases queue/submission locks
and does not wait for later submitted work. A shared-utility identity counter prevents device-ID reuse
when cores are recreated. The GPU sort diagnostic uses this extension for readback, with the old
submit/`wait_idle` fallback for backends lacking it. Frame readback, resize and teardown retain their
existing waits in this first iteration.

## Object identity: concrete names, and the validate-then-cast pair

Every contract handle derives from `rhi::object`, which carries ONE public member: `type`, a
`std::string_view` holding the CONCRETE implementation name the backend chose (`deren_image_vulkan`,
`deren_frame_image_vulkan`, ...). The name lives in static storage, and it is the identity the backend
validates before it casts a handle to its own concrete layout. The granularity is CONCRETE, not the
interface, because one interface has several layouts behind it: an owned image and the borrowed
swapchain image are both `rhi::image`, and the frame's recording list and a caller-owned command buffer
are both `rhi::command_buffer`.

The check-then-cast operation belongs to the contract, in `:contract`, so every backend shares one
copy of the rule:

| name | answer |
| --- | --- |
| `rhi::is_concrete(object const&, string_view name)` | false = a handle this backend did not hand out; the caller-facing answer is `error::invalid_argument` (a factory answers `nullptr`) |
| `rhi::do_concrete<concrete_t>(object const& / object&, name, source_location = current())` | the handle as its concrete adapter type. A mismatch PANICS: by the time this is called the provenance (registry membership, an object-identity comparison) has already been established, so a mismatch is the backend's own bookkeeping being inconsistent - a bug in the code holding the invariant, not a caller condition |

The names are the BACKEND's, not the contract's: the Vulkan backend keeps its 29 constants in
`deren::vulkan::concrete` and passes them to both calls, which is what lets a second backend apply the
same rule with its own names. Moving the two helpers into `:contract` moved no `abi_version` - they are
inline and template free functions over the existing field (no vtable slot, no by-value POD, no
entry-point signature) - and the only dependency they add to `:contract` is `deren.utility.shared`, the
process-wide panic sink both halves already link.

One documented-precondition cast remains: `device_address::buffer_address` `static_cast`s its operand on
a DOCUMENTED PRECONDITION with no runtime check. Its operand is a handle the contract says only the
owning backend may be asked about, so an invalid one is a caller bug rather than data to validate; the
function's own note states it. (The raw-handle escape that used to share this rule is deleted - abi 43.)

## The C entry: one symbol, resolved by name

`source/promise/rhi/backend_entry.hpp` declares the boundary's entire C ABI, and it is deliberately tiny.

* **`deren_make_api_core` is the only export.** Its FIRST argument is `rhi::abi_version`, so a
  mismatched backend is refused BEFORE an object exists, with the backend's own number in the
  `error_info` it fills.
* **It returns an owning `rhi::shared_object<api_core>`**, the contract's own shared container, so there
  is no second destroy entry and no raw-pointer ownership question. The container drops the root through
  `api_core::release()` (for the root that call destroys it), exactly like every other RHI handle.
* **The object attests its own number too**: the runtime checks `api_core::api_version()` after the
  handshake, because a host may be handed an `api_core` it did not create.
* **The GUI plugin takes the same shape**: `deren_gui_vulkan.dll` exports one name, `deren_make_gui`,
  whose FIRST argument is the GUI boundary's own `gui::gui_abi_version` (5 today) - a mismatched caller
  is refused before an overlay exists, exactly as the RHI entry refuses a mismatched backend.

The keyword that makes a symbol visible across a DLL boundary lives in ONE header,
`source/utility/abi_export.hpp`, chosen from the build mode rather than from a platform `#ifdef` at every
declaration.

## Windowed and windowless rendering

The caller owns any window. A non-null `create_info::native_window` binds the backend to that
window; null selects windowless rendering. The Vulkan backend then creates ordinary device-local
images, with no GLFW initialization, surface, swapchain or WSI acquire/present calls. Output size
comes from `window_width` and `window_height`; the images reuse the existing two frame slots and
timeline completion waits. `frame_image()` and readback retain their normal recording contract.
For compatibility, `frame_swapchain()` still supplies the output format and extent, but its
`recreate()` and `core.frames->present()` return `error::unsupported` in windowless mode. The async
presentation extension is absent. The runtime closes an offscreen frame after submitting its commands.

The application's `config.toml` selects startup mode and optional window presentation overlap:

```toml
[render]
async_present = false # experimental; window presentation only
offscreen = false   # true creates no application window
```

`--capture-frames N` automatically selects windowless rendering and keeps image readback, animation
stepping and GPU timings. `--capture-windowed` retains a hidden window for explicit resize/presentation
regressions when `offscreen` is false. The GUI is disabled without a window. The Vulkan driver may
create its own internal temporary HWND; the renderer creates none. Async presentation now reads the
configuration key; `DEREN_ASYNC_PRESENT` no longer selects it in current builds. Historical benchmark
snapshots retain the environment switch they were measured with.

## Recording and submission

A command buffer records; `command_context::submit(command_buffer&)` submits one. For a batch there is a second,
parallel type: `mergeable_command_buffer` (a sibling of `command_buffer`, not a subtype) can only be submitted
through `command_context::submit(std::span<mergeable_command_buffer_ref const>)` - in span order, with the whole batch
validated before anything is queued. The backend owns the merge: the Vulkan backend records the segments into a
backend-owned primary and submits that, while a D3D12 backend can hand the command lists straight to
`ExecuteCommandLists`. Segments are self-contained (state and render scopes do not cross a boundary), no memory
barrier is inserted between them, and the backend retains the handles until the submission completes. The
engine's multithreaded recording paths use EXACTLY this: the main pass's per-worker segments, the shadow
per-cascade segments and the transparent segment are mergeable buffers, merged into the frame's primary with
`command_buffer::execute(span)` while its rendering instance is open (the pixel result is unchanged - the same
`vkCmdExecuteCommands` sequence, one batch call). Single-threaded passes still record straight into the frame
command buffer, and `submit(span)` stays available for a backend whose merge is submission-time.

## Capabilities: abilities and extensions

A backend is not asked what it is; it is asked what it can do.

* **Abilities** are the coarse tier: `abilities()` answers a set of bits (mesh shaders, a descriptor
  heap, ...). The engine asks and keeps compiling when an answer is "no".
* **Extensions** are the optional interfaces behind those bits: `query_extension<T>()` hands one out or
  nothing. The device facts (`device_capabilities`), the shader-group access and the descriptor space are
  what the engine itself uses. THE ESCAPE IS GONE (abi 43): its last slot existed only so the GUI overlay's
  Dear ImGui Vulkan backend could get a host command buffer, and the overlay now gets no host recording
  target at all. The GUI receives the opaque `rhi::main_context` (abi 39) for the device root plus its OWN
  two contract `mergeable_command_buffer_ref` targets in `gui::create_info` (gui abi 5); `overlay::record()`
  starts the current target, records ImGui into that segment and returns it, and the host executes the
  segment in a dedicated LOAD + secondary-contents rendering instance AFTER the pass's own instance ends
  (a dynamic-rendering instance flagged for secondary contents may contain no primary draws, and the pass
  keeps its own draw in the primary). The plugin still names no native type to the contract - it checks the
  target's concrete tag and casts it to the backend's shared `deren.vulkan.mergeable_command_buffer` class
  to `get()` the native handle, the same opaque-handle + concrete-tag + shared-layout rule
  `rhi::main_context` follows (the device root moved there in abi 39, and abi 40/42 deleted the
  resource/channel handles with their last callers). That segment's begin is the abi-45
  `command_buffer_begin_info::inherit_descriptor_heaps` opt-out: it passes false, because the Dear ImGui
  Vulkan backend binds its own classic descriptor sets and inheriting the engine's heap bind made every
  one of those binds fail validation. `extension_kind::vulkan_escape` survives only as a retired bit value
  - never announced, never queried.
* **A fact the engine needs is an answer, not a native query.** A graphics-API concept appearing in the
  engine is a contract gap: it gets an ability or a tier-1 object, never an `escape()` call and never a
  native type.
* **The descriptor space gives descriptors logical positions.** `descriptor_space` is the servable
  face of the descriptor heap: `position(texture_group)` answers the frozen `descriptor_position` an
  immutable group was registered at (space, base, count, generation), and `write(descriptor_write)`
  publishes one sampled descriptor there from a portable `image_view_desc` instead of a native view.
  A position is opaque to the engine beyond identity: material data can carry it instead of an
  absolute heap slot, and the generation turns a stale position into `invalid_argument` rather than
  a mis-cast. It is a new ability bit, so it moved no ABI number.

## The loaders: how a backend becomes an object

`deren.engine.backend_loader` is the policy and `deren.utility.dynamic_link` is the mechanism under it.
Four guarantees, each measured while the boundary was built:

1. **An absolute path into the loader.** The DLL is looked for beside THIS executable
   (`executable_directory() / "deren_vulkan.dll"`) and passed as an absolute path, so the loader
   searches the DLL's own directory and the system directories, and NEVER `%PATH%`.
2. **Once per process.** The load and the symbol lookup happen in one function-local static, so the
   image is opened once however many times a root is asked for.
3. **Never unloaded.** The handle is detached from the loader object, because `FreeLibrary` after a
   context had been built and torn down never returned.
4. **A named diagnosis, never a null pointer.** A failing step fills the contract's `error_info` (code,
   raw native error, the backend's text, the failure point) and reports it before returning an empty
   `rhi::shared_object<api_core>`. A missing DLL is a message, not a system dialog.

The GUI is reached the same way: `deren.engine.gui_loader` resolves `deren_gui_<api>.dll`, and the
overlay crosses as the `deren::gui::overlay` interface plus one C entry.
The device root crosses as the RHI's OPAQUE `rhi::main_context` (abi 39): the contract carries only the
concrete-identity tag, and the API-specific plugin casts it to the layout it shares with its backend
(`deren.vulkan.main_context` for the Vulkan plugin) - so no native handle or format number is part of
the GUI contract.

The overlay's recording crosses the same way (gui abi 5): the host creates the two
`mergeable_command_buffer` recording targets through `command_context::make_mergeable_command_buffer()` and
hands them over in `gui::create_info`; the plugin holds them, records ImGui into one per frame and
returns the recorded segment. The host's own command buffer is never handed to the plugin - only the
`command_buffer_begin_info` inheritance of the instance the overlay segment has to match. The two
targets alternate per frame, and the frame slot ring's own timeline wait makes alternating reuse safe.

## The run-time package

Five images plus the C++ runtime, all beside each other:

| image | how it is reached | why it is separate |
| --- | --- | --- |
| `deren.exe` | - | the application and the engine |
| `deren_assets.dll` | import library (loads before `main`) | glTF/GLB loading; there is exactly one loader and nothing selects it, so the loader-error regime has nothing to preserve |
| `deren_vulkan.dll` | BY NAME at run time | the Vulkan backend, one export |
| `deren_gui_vulkan.dll` | BY NAME at run time | the Dear ImGui overlay and its `vulkan-1` link, one export |
| `shared_utility.dll` | import library | the process-wide log sink, the rotation claim and the allocator hook - one copy per process by construction |

`libc++.dll` is imported by all of them, which is the premise that lets a contract value carrying STL
containers cross the seam. The build's compiled `source/shaders/` directory sits beside them, because the
executable locates it by walking up from its working directory.

## Writing a second backend

The normative per-interface standard for every virtual function is [The deren RHI API standard](rhi_api_standard.md);
this page is the architectural overview. The standard records ABI 54's removal of incomplete query/swapchain factories and the unused mesh extension,
and the genuinely unspecified behavior a second backend must not rely on.

1. Implement `deren::promise::rhi::api_core` and its resource, command and frame interfaces. Bind
   those views in the derived constructor before returning the core to callers.
2. Export `deren_make_api_core` with the contract's `abi_version` as the first argument.
3. Answer `query_extension<T>()` for the extensions you implement and nothing for the rest, and report
   the device's facts through `device_capabilities` rather than letting a caller query the API. A
   backend with a descriptor-heap-like model also answers `descriptor_space` (positions for immutable
   texture groups and the portable sampled write); a backend without one does not, and the ability rule
   keeps that a named failure instead of a silent downgrade.
4. Change nothing in the executable: the application picks the backend by the file it loads, and the
   engine's side of the boundary is unchanged.

## How the migration got there

The port ran in slices, which is what let each one be verified. The contract came first; the render
chain's targets were created, held and released by the engine through it; the runtime was ported
partition by partition until it named no backend type at all; and the last slice made `deren_vulkan` a
DLL, deleted the legacy runtime and retired the `-DVR_RUNTIME=legacy|dynamic` switch with it - one file
list, one configuration.

Two things that bite at this seam are worth knowing before touching it. **A backend module cannot be
imported by the engine** - the BMI would have to come from the DLL - which is why the import meter had
to reach zero rather than merely shrink. And **the backend carries its own GLFW image**, so a
`GLFWwindow*` cannot cross the boundary: the window is translated to a native handle where the boundary
is crossed.

## What the gates check

| gate | question |
| --- | --- |
| `scripts/checks/check_native_boundary.py` | the executable's import table, the engine objects' unresolved symbols, and the engine sources' vocabulary |
| `scripts/checks/check_backend_boundary.py` | the symbol ratchet, the whitelist (empty now) and the export table of each plugin |
| the render gates (`check_render.ps1`, `check_gui.ps1`, `check_rt.ps1`) | that the application really runs the backend it resolved: fifteen frozen legacy-path scenarios plus the visibility-path A/B, a GUI on/off pair, and a GPU ray-traversal probe |

## The rename, done

The namespaces split the way the module names do: the portable half reads `deren.engine.*` /
`deren::engine`, and only the native Vulkan implementation keeps `deren.vulkan.*` / `deren::vulkan`.
The doxygen groups follow the same split (`engine_*` and `vulkan_*`; no group is spelled
`vulkan_render_*` any more), so the rename that used to be the open item here has been made.

## Independent buffer transfers

The engine [buffer transfer service](buffer_transfers.md) composes resource leases, scoped synchronization
and completion receipts for independent graphics upload/readback batches. It retains pending operands and
staging through completion without changing the RHI ABI or frame screenshot path.
