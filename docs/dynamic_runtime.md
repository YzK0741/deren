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
- a new capability is a new query, and a backend that does not implement it says so.

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

## Capabilities: abilities and extensions

A backend is not asked what it is; it is asked what it can do.

* **Abilities** are the coarse tier: `abilities()` answers a set of bits (mesh shaders, a descriptor
  heap, ...). The engine asks and keeps compiling when an answer is "no".
* **Extensions** are the optional interfaces behind those bits: `query_extension<T>()` hands one out or
  nothing. The raw-handle escape (`vulkan_escape`), the device facts (`device_capabilities`) and the
  shader-group access are the ones this build uses.
* **A fact the engine needs is an answer, not a native query.** A graphics-API concept appearing in the
  engine is a contract gap: it gets an ability or a tier-1 object, never an `escape()` call and never a
  native type.

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
containers cross the seam. The build's compiled `shaders/` directory sits beside them, because the
executable locates it by walking up from its working directory.

## Writing a second backend

1. Implement `deren::promise::rhi::api_core`: object creation, command recording, frame walking and
   `abilities()`.
2. Export `deren_make_api_core` with the contract's `abi_version` as the first argument.
3. Answer `query_extension<T>()` for the extensions you implement and nothing for the rest, and report
   the device's facts through `device_capabilities` rather than letting a caller query the API.
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
| the render gates (`check_render.ps1`, `check_gui.ps1`, `check_rt.ps1`) | that the application really runs the backend it resolved: 14 frozen pixel-identical scenarios, a GUI on/off pair, and a GPU ray-traversal probe |

## Still open

The C++ module namespaces still read `vulkan.*` and four doxygen groups still read `vulkan_render_*`.
Renaming them is a decision about what is the backend versus what is the engine, which is why it is
taken separately from the boundary itself.
