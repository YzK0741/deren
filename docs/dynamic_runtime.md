# The dynamic API: the contract, the by-name-loaded backends, and the run-time package

The graphics API reaches this renderer through a contract, never through Vulkan types. The executable
links no graphics library: it loads a backend DLL by name, hands it a contract it implements, and talks
to the device through that interface alone. Everything a second backend has to satisfy is on this page.

**Measured at this version** (on the artifacts, not read from the code): `deren.exe` imports **no**
graphics API, the engine's object files carry **no** unresolved `vk*` symbol, and **no** engine source
names the graphics API. A backend *module* is not importable by the engine either - the loader is the
only door, and `scripts/check_backend_boundary.py` is the gate that keeps it that way.

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

The contract is versioned by `rhi::abi_version`, and a change to it is one of two kinds: **appending a
slot to an existing interface bumps the number**, while **adding a new ability or extension does not**
- a new capability is a new query, and a backend that does not implement it says so. A third kind is the
same as the second: a NEW type or a free helper function (a `constexpr`/`inline`/template declaration)
bumps nothing, because nothing already crossing the boundary changes shape.

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

One deliberate exception: the raw-handle escape (`vulkan_escape::native_buffer` / `native_image` / ...)
and `device_address::buffer_address` still `static_cast` on a DOCUMENTED PRECONDITION with no runtime
check. Their operands are handles the contract says only the owning backend may be asked about, so an
invalid one is a caller bug rather than data to validate; those functions' own notes state it.

## The C entry: one symbol, resolved by name

`source/promise/rhi/backend_entry.hpp` declares the boundary's entire C ABI, and it is deliberately tiny.

* **`deren_make_api_core` is the only export.** Its FIRST argument is `rhi::abi_version`, so a
  mismatched backend is refused BEFORE an object exists, with the backend's own number in the
  `error_info` it fills.
* **It returns an owning `std::shared_ptr<api_core>`**, so there is no second destroy entry and no
  raw-pointer ownership question.
* **The object attests its own number too**: the runtime checks `api_core::api_version()` after the
  handshake, because a host may be handed an `api_core` it did not create.
* **The GUI plugin takes the same shape**: `deren_gui_vulkan.dll` exports one name, `deren_make_gui`.

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
`recreate()` and the core's `present()` return `error::unsupported` in windowless mode. The async
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

A command buffer records; `api_core::submit(command_buffer&)` submits one. For a batch there is a second,
parallel type: `mergeable_command_buffer` (a sibling of `command_buffer`, not a subtype) can only be submitted
through `api_core::submit(std::span<mergeable_command_buffer_ref const>)` - in span order, with the whole batch
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
  nothing. The device facts (`device_capabilities`) and the shader-group access are what the engine itself
  uses; the escape (`vulkan_escape`) is down to ONE slot - `native_command_buffer`, because the GUI
  overlay's Dear ImGui Vulkan backend records its own draw data into the frame's command buffer. Its device
  and resource handles are gone (abi 39 handed the device root over as `rhi::main_context`; abi 40/42
  deleted the rest with their last test callers).
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
   `shared_ptr`. A missing DLL is a message, not a system dialog.

The GUI is reached the same way: `deren.engine.gui_loader` resolves `deren_gui_<api>.dll`, and the
overlay crosses as the `deren::gui::overlay` interface plus one C entry.
The device root crosses as the RHI's OPAQUE `rhi::main_context` (abi 39): the contract carries only the
concrete-identity tag, and the API-specific plugin casts it to the layout it shares with its backend
(`deren.vulkan.main_context` for the Vulkan plugin) - so no native handle or format number is part of
the GUI contract.

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

1. Implement `deren::promise::rhi::api_core`: object creation, command recording, frame walking and
   `abilities()`.
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
| `scripts/check_native_boundary.py` | the executable's import table, the engine objects' unresolved symbols, and the engine sources' vocabulary |
| `scripts/check_backend_boundary.py` | the symbol ratchet, the whitelist (empty now) and the export table of each plugin |
| the render gates (`check_render.ps1`, `check_gui.ps1`, `check_rt.ps1`) | that the application really runs the backend it resolved: fifteen frozen legacy-path scenarios plus the visibility-path A/B, a GUI on/off pair, and a GPU ray-traversal probe |

## The rename, done

The namespaces split the way the module names do: the portable half reads `deren.engine.*` /
`deren::engine`, and only the native Vulkan implementation keeps `deren.vulkan.*` / `deren::vulkan`.
The doxygen groups follow the same split (`engine_*` and `vulkan_*`; no group is spelled
`vulkan_render_*` any more), so the rename that used to be the open item here has been made.
