# C# Module ABI

Status: design proposal, not implemented.

Baseline: `master` at `07bc77e4 feat(scripting): add input actions and safe scene instancing (#77)`,
with managed ABI v4.

## 1. Why this proposal exists

The current `native_api` is one flat, versioned structure:

- `source/scripting/managed_host.cppm:45` declares it on the native side.
- `source/scripting/managed/Interop/native_api.cs` mirrors it on the C# side.
- `source/scripting/managed/Host/managed_bootstrap.cs` validates the structure and hands the
  function pointers to the managed scene runtime.

Today that structure carries `log`, `get_transform`, `set_transform`, `try_spawn`, and
`try_despawn`. Every new subsystem would add another field to the same structure. That has three
costs:

1. Every addition changes the global ABI shape, even when the subsystem is unrelated.
2. A missing optional feature and a broken core table are indistinguishable until a call site is
   reached.
3. The structure becomes a list of unrelated functions with no ownership boundary.

The goal is to keep the small core bootstrap stable while each subsystem owns its own versioned
interface.

## 2. Options considered

### 2.1 Keep growing `native_api`

This is the simplest option and is acceptable for one or two more fields. It does not solve the
ownership or versioning problem, and every subsystem still needs a global ABI revision.

### 2.2 One `get_method` lookup per method

Add a resolver such as:

```text
get_method(context, module_id, method_id) -> void*
```

A C# static class per module calls it during initialization and casts the result to a function
pointer. This is better than growing the core table, but a per-method resolver loses type checking
at every call site. A typo in a method id, a wrong signature, or a version mismatch becomes undefined
behavior rather than a failed initialization.

### 2.3 One typed module table per subsystem

Add one resolver:

```text
get_module(context, module_id, module_version, table, size) -> status
```

Each subsystem publishes a static, typed table of function pointers. C# resolves and validates
the table once per scene, then calls through the validated scene binding.

This keeps a single lookup mechanism, gives every subsystem a clear ownership boundary, and moves
layout checks to startup. This is the recommended design.

## 3. ABI shape

### 3.1 Core structure

The v5 core structure keeps only bootstrap data and the module resolver:

```cpp
struct native_api {
    std::uint32_t size = sizeof(native_api);
    std::uint32_t version = managed_abi_version;
    void* context = nullptr;
    void(__cdecl* log)(void* context, char const* utf8, std::uint32_t length) = nullptr;
    script_status(__cdecl* get_module)(
        void* context,
        std::uint32_t module,
        std::uint32_t version,
        void const** table,
        std::uint32_t* size) = nullptr;
};
```

The C# mirror uses `delegate* unmanaged[Cdecl]` fields and matches this layout exactly:

```csharp
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct native_api {
    public uint size;
    public uint version;
    public void* context;
    public delegate* unmanaged[Cdecl]<void*, byte*, uint, void> log;
    public delegate* unmanaged[Cdecl]<void*, uint, uint, void**, uint*, script_status> get_module;
}
```

`context` is the native scene context used by the resolver and every module call. The application
keeps it valid until `stop_scene` returns. Table addresses may be shared across scenes while each
scene has its own context and module availability.

### 3.2 Module table

A module table starts with its own size and version, followed by function pointers:

```cpp
struct scene_api_v1 {
    std::uint32_t size = sizeof(scene_api_v1);
    std::uint32_t version = 1;
    script_status(__cdecl* get_transform)(void* context, node_handle entity, transform_value* value);
    script_status(__cdecl* set_transform)(void* context, node_handle entity, transform_value const* value);
    script_status(__cdecl* try_spawn)(void* context, utf8_view asset, transform_value const* value, node_handle* entity);
    script_status(__cdecl* try_despawn)(void* context, node_handle entity);
};
```

The C# side declares the same sequential layout with function pointer fields. The table is owned by
the native module and must remain valid for the lifetime of the managed host. It is not allocated per
scene and must not be freed while C# can call it.

Appending a field is a size change that older C# code can detect with `size`. Changing an existing
field, its calling convention, or its meaning requires a new table version.

### 3.3 Module ids

Use stable numeric ids, not strings:

```cpp
enum class native_module : std::uint32_t {
    scene = 1,
    input = 2,
    ui = 3,
};
```

Keep the numeric IDs stable. The first migration publishes only `scene`. Input continues to use the
frame snapshot, and the `input` ID is reserved for a future native query API.

## 4. Managed module binding and lifetime

`initialize_from_engine` validates the core ABI and registers the managed lifecycle entry points.
`start_scene` validates its payload and resolves the required and optional modules using that scene's
`native_api`. Keep the typed table pointers, context, and optional availability in `scene_state`.
Managed wrappers such as `scene_access` and `node_id` use the binding for their owning scene.

Resolve and validate all bindings before publishing the scene state or invoking script lifecycle
callbacks. A failed bind leaves no scene state behind. During `stop_scene`, keep the binding valid
through script teardown, then discard queued UI events, release managed subscriptions, and remove
the state. Public wrappers must check scene liveness before calling a stored context. A scene
reopened with a new identity receives a new binding.

## 5. Error handling

The resolver reports an unavailable module and an unsupported requested version as distinct
statuses. On success it returns the table address and readable byte count. It clears the out
values on failure when the output arguments are valid.

The managed consumer checks a successful result in this order:

1. Verify that the table address is non-null and the returned byte count covers the common header.
2. Verify the header's exact requested version and confirm that its declared size fits within the
   returned byte count and covers the required fields.
3. Check every required function pointer before publishing the typed binding.

An unavailable optional module leaves that scene's capability disabled. A required module's
absence, an unsupported version, or a malformed success response fails scene binding and logs a
diagnostic through the core logger. Expected errors return a status at the ABI boundary. Managed
exceptions are caught on the managed side.

## 6. Versioning and compatibility

Core ABI v5 introduces `get_module` together with `start_scene(scene_start_info const*)` and
`update_scene(frame_info const*)`. The two size- and version-tagged payloads are defined in
[`scripting_ui_plan.md`](scripting_ui_plan.md#6-native-scripting-abi-v5-target). P0 uses the input
snapshot in the initial `frame_info`. P3 appends a borrowed UI event pointer and count behind the
size check. The event batch lives through the update call. Module tables start at version 1.

There are two compatibility choices:

1. Break at v5 and require native host and managed bridge to be built together. This is the simplest
   and matches the in-tree build.
2. Keep a v4 compatibility path in the v5 host. The host inspects `native_api.size` and synthesizes
   a legacy scene view from the old flat fields. This is only worth doing if out-of-tree managed
   assemblies must keep running against a newer engine.

The first option is recommended until there is a real external compatibility requirement.

Version rules:

- `native_api::size` protects append-only additions to the core structure.
- Each module table has its own `version`; a module can change independently of the core.
- A table is returned only for an exact version match. If multiple versions must coexist, the native
  side owns that decision and returns the requested table.
- Use the core version for the handshake and the module's version for its own table. The app
  release version is independent of both.

## 7. Migration plan

1. Add `scene_api_v1` to `source/scripting/managed_host.cppm` and declare its static instance in the
   native host.
2. Replace the flat gameplay fields with `get_module` and introduce the v5 scene and frame payloads
   in the same change. Build the native host and managed bridge together.
3. Mirror the structures on the C# side and verify their sizes and offsets against C++.
4. Move the four existing gameplay callbacks into `scene_api_v1`. Preserve their behavior and the
   public C# API while migrating `scene_access` and `node_id` to the owning scene's binding.
5. Validate the handshake, scene start and stop, and two-scene isolation. Complete the gameplay API
   hardening in P0 after the migration is verified.
6. In P3, resolve the optional `ui_api_v1` through `get_module`. Route input through native RmlUi
   before creating the gameplay snapshot. Append the queued UI events to `frame_info` and dispatch
   them in C# before behaviour updates.

## 8. Tests

Extend `source/tests/test_managed_host.cpp` with:

- a required module that is missing from the native registry;
- an unknown module id;
- a module version mismatch;
- null table on success, a short readable span, a header size outside that span, a wrong header
  version, and a null required function pointer;
- an unavailable optional module and a malformed optional table;
- failed scene binding with no published state, followed by a successful retry;
- two scenes interleaving calls through the same table with different contexts, then stopping one
  scene and confirming that the other still works;
- native and managed layout checks for the core tables, `scene_api_v1`, and both lifecycle
  payloads.

The existing round-trip, ABI mismatch, scene lifecycle, and input tests must continue to pass.

## 9. Non-goals

- A general reflection-based method dispatcher.
- String-based method lookup.
- Per-call module lookup from hot paths.
- Exposing C++ types, STL containers, exceptions, or object addresses across the boundary.
- Loading arbitrary native libraries from C#.
- Hot reload in the first implementation.

## 10. Open questions

- Whether the first external managed assembly requirement justifies a v4 compatibility shim.

## 11. Related documents

See `docs/scripting_ui_plan.md` for the scene/entity model, the C# gameplay API, and the native UI
plan that this ABI supports.
