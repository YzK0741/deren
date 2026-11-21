# C# API enhancement plan

The managed layer is a hand-written mirror of the native ABI: `source/scripting/managed_host.cppm`
declares the tables, and `source/scripting/managed/` restates them in C#. This document records what
that costs, what Godot's C# binding does about the same problem, and which of its ideas this project
adopts, in which order, and why the rest are rejected.

## 1. What the mirror costs today

Measured before this plan started (managed ABI 32, `feature/csharp-scene-tree`):

- 43 C# files, 3961 lines, all hand-written.
- `scene_api` has 63 function pointers, every one of them declared twice: once in the C++ struct and
  once in `source/scripting/managed/Interop/scene_api.cs`.
- Four layout mirrors (`native_api`, `managed_api`, `scene_api_layout`, `ui_api_layout`) plus the
  value structs are aligned by eye. Nothing but a comment says they must match.
- Every string argument repeated `Encoding.UTF8.GetBytes` into a `byte[]` and pinned it with `fixed`.
- Every wrapper collapsed `script_status` into `bool`, so a caller could not tell `asset_not_found`
  from `module_unavailable`; the reason only went to the log.
- The only compatibility check was one integer: `managed_abi_version`. Adding a field to a struct
  changed its size without changing the version, and the failure mode was a silently reinterpreted
  struct.

## 2. What Godot does

The reference checkout is `D:\deren-references\godot` at `c51ff06`; paths below are relative to it.
Godot solves the same problem at a much larger scale (hundreds of classes) and pays for it with a
code generator, a source analyzer, and two trampoline tables.

- The managed-to-native table is generated, not remembered.
  `modules/mono/glue/GodotSharp/GodotSharp/Core/NativeFuncs.cs:18-38` declares an empty
  `partial struct UnmanagedCallbacks` plus 226 `static partial` methods; the generator emits the
  fields and the bodies (`Godot.SourceGenerators.Internal/UnmanagedCallbacksGenerator.cs:159-254`
  for the bodies, `:327-364` for the field list). The hand-written part is only the native array
  (`runtime_interop.cpp:1790-1818`) and the order comment next to it. `Initialize(IntPtr, int)`
  compares the passed size against `sizeof(UnmanagedCallbacks)` (`NativeFuncs.cs:30-31`) and throws
  when they differ, so a missing or reordered entry is a startup failure rather than a wrong call.
- Returned strings are owned objects, not borrowed buffers.
  `modules/mono/glue/GodotSharp/GodotSharp/Core/InteropStructs.cs:524-580` wraps `godot_string` and
  `godot_string_name` in `ref struct`s with an explicit `Dispose()` that calls the destroy entry;
  marshaling in `Marshaling.cs:19-31` negotiates the offsets once at startup instead of hard-coding
  them.
- Handles are typed by ownership, not by an integer the caller trusts.
  `InteropUtils.cs:50-56` allocates a weak `GCHandle` for reference-counted objects and a strong one
  for engine objects, so the GC's view and the engine's view stay consistent; disposal is routed
  through `CustomGCHandle.cs` and `DisposablesTracker.cs`.
- A Roslyn analyzer enforces the convention that makes all of the above work.
  `ClassPartialModifierAnalyzer.cs:38-58` rejects a class that derives from `GodotObject` but is not
  `partial`, because the generator silently skips such a class and the member would just be missing.
- The bulk API is generated from the engine itself: `modules/mono/glue/bindings_generator.cpp:5227-5232`
  reads `ClassDB::get_class_list()`, so the binding cannot drift from the engine it binds.

Godot's answers split into two groups. The cheap, structural ones (size check at startup, one
manifest of layouts, generated table, analyzer guarding the generator) apply to any project. The
expensive ones (per-method trampolines keyed by `(name, argc)`, `Variant` as a universal argument,
global `StringName` interning) exist because Godot dispatches methods by name at run time, which
deren does not.

## 3. The plan

| Step | What it does | ABI change | Cost | Status |
| --- | --- | --- | --- | --- |
| P1 | Publish every layout's size and compare it at bind time | yes (33) | small | done, `b1252ae4` |
| P2 | One UTF-8 copy per call instead of a pinned array | no | small | done, `98fc436f` |
| P3 | Hand the native `script_status` back from every wrapper | no | small | done, `dc5cf079` |
| P4 | Generate the C# side of the table from one declaration list | no | medium | pending |
| P5 | A Roslyn analyzer that keeps the declarations and the ABI aligned | no | small | pending |
| P6 | Handle generations, so a freed node cannot be confused with a live one | yes | medium | pending |

### P1: layout manifest (done)

`scene_api.abi_layout` returns a list of `abi_layout_entry { abi_type type; std::uint32_t size; }`
for the ten layouts that cross the boundary, and `native_api.get_module` now also reports the size of
the table it returns. The C# side (`Host/abi_manifest.cs`) compares each entry against `sizeof` of
its own struct before it accepts the module, and refuses to load with a log line naming the layout
that disagrees. A version integer alone cannot catch "same version, different padding"; a size list
can, and it costs one call at startup.

`scene_api`, `ui_api` and `camera_state` are deliberately not in the list: the first two report their
own size through `get_module`, and `camera_state` has no entry of its own either - the managed mirror
is `deren.orbit_camera`, and only the native `static_assert` checks the record's size.

### P2: owned UTF-8, one copy (done)

`Interop/native_string.cs` allocates the UTF-8 bytes once with `NativeMemory.Alloc`, exposes them as
`utf8_view`, and frees them on `Dispose`. Every wrapper now reads `using var text = native_string.create(value);`
and passes `text.view`. The previous shape allocated a `byte[]`, pinned it, and relied on the call not
storing the pointer; the new shape makes the lifetime explicit in one place instead of 63.

### P3: status, not just bool (done)

Every wrapper that can fail has a twin taking a trailing `out script_status status`. The short form
is `=> name(args, out _)`, so the two cannot drift. `script_status` is public for this reason. A
wrapper that rejects its own arguments reports `invalid_argument`; a managed exception caught at the
boundary reports `script_failure`; otherwise the caller sees exactly what the host returned.

### P4: generate the C# table (pending)

The Godot-shaped version of P1-P3 is a generator: the developer writes the intent once and the tool
emits the layout and the wrappers. For deren the smallest useful version is:

- one C# file lists the entries (`[NativeEntry(nameof(spawn), ...)]` or a plain table) and the
  generator emits `scene_api_layout`, the static field initialisation and the wrapper stubs;
- the C++ side stays hand-written with `static_assert(sizeof(...) == ...)`, because generating into
  `source/` would make a nine-thousand-line review and break "the repository builds from what is
  committed";
- the generated file is committed, not gitignored, so a reviewer sees the ABI change as a diff.

The reason this is `medium` and not `small`: C# source generators run inside the compiler, so the
project needs an analyzer project, a `csproj` reference to it, and a rule that the generated file is
rebuilt in CI. Godot pays that cost for hundreds of classes; deren would pay it for 63 entries. It is
worth doing only together with P5, which is what makes the generation trustworthy.

### P5: an analyzer that guards the convention (pending)

Godot's `ClassPartialModifierAnalyzer` is the model: a convention that cannot be expressed in the
type system is enforced by a diagnostic instead of a comment. For deren the rule is "every
`scene_api` entry has exactly one wrapper, one layout field, and the same name", reported as a
warning-as-error in the managed project. Precedent exists in this repository for the pattern
(`source/tests/test_docs.cpp` enforces documentation invariants the same way, from the test side);
the analyzer is the C# side of it.

### P6: handle generations (pending)

A `node_handle` is an opaque integer today. A stale handle that happens to name a live node after
reuse is indistinguishable from a live one, and `is_alive()` cannot tell "freed" from "wrong scene".
The fix is to split the integer: 48 bits of slot id, 16 bits of generation, checked by the host and
exposed as `node_id.is_valid` / `ThrowIfFreed`. It changes the handle's meaning, so it is its own ABI
version and its own PR.

## 4. What is not adopted, and why

- **Per-method trampolines and `(name, argc)` dispatch.** Godot needs them because a script calls
  `obj.method(...)` by name. deren's managed side is a fixed lifecycle: `preinit`, `initialize`,
  `update`, `stop`, `signal`, plus one frame callback. There is no per-method dispatch to build.
- **`Variant` as a universal argument.** deren passes typed structs across the boundary and validates
  them field by field on the native side. A tagged union would move that validation into every
  caller and lose the compile-time checks the structs give.
- **Global `StringName` interning.** deren's strings are paths, pipelines and labels, each used once
  per call. Interning would add a lifetime and an ownership question to solve a cost that does not
  exist here.
- **Negotiated struct offsets.** We never share a C++ container's layout across the boundary; the
  structs are plain arrays of scalars. `static_assert` plus the P1 manifest is enough, and it fails
  at build time rather than at startup.
- **Generating the C++ side and gitignoring it.** The repository rule is that the committed tree
  builds. A generated-and-ignored C++ table would make every ABI change invisible in review.

## 5. Order and risks

P1-P3 are done on `feature/abi-layout-manifest` and are independent of each other in cost: P1 touches
the ABI once, P2 and P3 are managed-only. P5 is the cheapest next step and the prerequisite for P4
being worth anything; P4 without P5 replaces a hand-written mirror with a generated mirror that
nothing checks. P6 is independent and should be its own PR because it changes what a handle means.

The risk in P4 is a generator that disagrees with the hand-written C++ table: the generator has no
way to know the C++ order except by reading a file the build produces, which is exactly the drift
this plan is trying to remove. If P4 is started, the first commit must be a test that compares the
generated table against `abi_layout` at bind time, so the generator cannot pass by accident.

## 6. Open questions

- Should P4 generate the C++ `scene_api` too, behind a CMake step that writes into a gitignored
  directory and a test that fails when the committed file is stale? (Godot does exactly this with
  `bindings_generator.cpp`; it is rejected above for review reasons, but a stale-check test would
  recover most of the safety.)
- Should the sample project (`source/scripting/test_game/`) grow one call per status overload, so the
  new API has a use in the repository rather than only in the tests?
- Where does P6's `is_valid` live: on the host (one check per call) or on the managed `node_id`
  (a cached generation)? The host check is authoritative; the cached one saves a call but can lie
  after a destruction the managed side did not see.

## 7. Folder layout

The managed sources are grouped by role, not kept flat; `file(GLOB_RECURSE)` in
`source/scripting/CMakeLists.txt` picks up whatever lives under `source/scripting/managed/`.

| Folder | Holds |
| --- | --- |
| `Api/scene/` | scene tree, camera, lighting, materials, post chain, environment, probes, and the value types they exchange |
| `Api/assets/` | the asset manager and its handle |
| `Api/input/` | input events and the held-key state |
| `Api/runtime/` | the script base class, pacing, and the project configuration |
| `Api/ui/` | the panel API |
| `Api/diagnostics/` | `log` and `nothrow_log` |
| `Interop/tables/` | the function-pointer tables the host publishes, and the module handles |
| `Interop/types/` | the value structs, the layout mirror, and the UTF-8 helper |
| `Host/` | bootstrap, ABI manifest, frame dispatcher, game host |
| `Config/` | the TOML reader and the settings it produces |

Namespaces do not follow the folders: what a project scripts against stays in `deren`, everything
that only the host uses stays in `deren.scripting`.

