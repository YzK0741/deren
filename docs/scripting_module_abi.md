# C# Module ABI

Status: implemented for the `scene` module; managed ABI v16.

Baseline: `feature/csharp-scene-tree`. Native code owns rendering, the Vulkan runtime, the
module resolver, and primitive/scene import. Managed code owns scene construction, asset
selection and decoding, scene activation, scripts, and the frame/shutdown callbacks. Native
asset preloading from `[[scene.objects]]` serves only the native preview path.

## 1. Ownership boundary

The host has no fallback scene for a scripted project. `initialize_from_engine` is the last
bootstrap step:

1. Load `Deren.Managed.dll` and the project assembly.
2. Find exactly one concrete `game_entry` in the project assembly.
3. Call `game_entry.initialize(native_api*, managed_api*)`.
4. Validate that the project registered `managed_api.update` and `managed_api.stop`.

The project entry:

1. Resolves `scene_api` from `native_api.get_module`.
2. Creates a standalone `scene_tree`.
3. Selects and loads assets through `asset_manager`.
4. Creates every root and child node.
5. Calls `scene_tree.try_activate()`.
6. Binds renderable assets with `scene_tree.try_bind_asset`.
7. Attaches scripts.
8. Writes its `update` and `stop` function pointers into `managed_api`.

There is no `start_scene`, `scene_builder`, object count, binding array, or native object-root
allocation. `[scripts]` and `[[scene.objects]]` remain native project metadata for the native
preview path; a managed project ignores them and decides its own object graph.

## 2. Core structures

```cpp
struct native_api {
    void(__cdecl* log)(char const* utf8, std::uint32_t length) = nullptr;
    script_status(__cdecl* get_module)(std::uint32_t module, void const** table) = nullptr;
};

struct managed_api {
    script_status(__cdecl* update)(frame_info const* frame) = nullptr;
    script_status(__cdecl* stop)() = nullptr;
};

struct frame_info {
    std::uint64_t scene = 0;
    float delta_time = 0.0F;
    std::uint32_t input_count = 0;
    std::uint8_t const* input = nullptr;
};
```

`frame_info` is 24 bytes and fixed for the ABI version. `scene` is the native script-scene id
(`script_scene::scene_id`), not the managed tree id; `get_active_scene` reports the tree id. The
input bytes are borrowed for one update call.

The managed bridge exposes exactly two lifecycle entries. It does not publish the scene table
itself; the project loads and initializes `scene_api`.

## 3. Scene module table

The table is static, typed, and versioned only by the global `managed_abi_version`:

```cpp
struct scene_api {
    get_transform;
    set_transform;
    try_spawn;
    try_despawn;
    create_scene_tree;
    activate_scene;
    get_active_scene;
    deactivate_scene;
    destroy_scene_tree;
    create_node;
    destroy_node;
    get_node_transform;
    set_node_transform;
    set_node_visible;
    list_asset;
    load_asset;
    release_asset;
    bind_asset;
};
```

The order is part of the ABI. C# mirrors it with one layout and validates every pointer before
publishing the table.

- `create_scene_tree` creates an empty independent tree and returns a process-wide tree id.
- `activate_scene` attaches the tree to the current project scene and calls the native
  `runtime.set_scene` host callback. It succeeds at most once per tree and only during
  `game_entry.initialize`.
- `get_active_scene` returns the currently active tree id, or 0.
- `deactivate_scene` detaches the active tree before `destroy_scene_tree`; the renderer must not
  begin another frame with that tree after the call returns.
- `destroy_scene_tree` rejects an active tree.
- Node handles are 64-bit process-wide ids. A zero parent creates a root.
- `set_node_visible` writes the node's visible flag; the renderer hides the whole subtree.
- `load_asset` decodes the file named by the library id, resolves its materials, and stores the
  resulting source model in the managed asset registry. It does not consult `[[scene.objects]]`.
- `bind_asset` is the only path that turns a loaded asset into renderable primitives. It imports
  the registry's scene and resolved material slice under the requested parent and returns the
  instance root. Native object slots and their preloaded models are not involved.

## 4. Managed API

```csharp
public sealed class scene_tree : IDisposable {
    public static bool try_create(out scene_tree? tree);
    public bool try_activate();
    public bool destroy();
    public bool try_create_node(node_id parent, string? name, in transform_value local, out node_id node);
    public bool destroy_node(node_id node);
    public bool try_bind_asset(node_id parent, asset_handle asset, in transform_value offset, out node_id root);
    public bool try_get_transform(node_id node, out transform_value value);
    public bool try_set_transform(node_id node, in transform_value value);
    public bool try_set_visible(node_id node, bool visible);
}
```

`asset_manager` is the asset source:

```csharp
string[] asset_manager.list();
bool asset_manager.try_load(string id, out asset_handle asset);
bool asset_manager.release(asset_handle asset);
```

A loaded handle owns one decoded and resolved source model. Binding imports that source into the
target tree; the handle can be released after binding. Releasing it does not invalidate bound
nodes. The asset is selected by library id, so a managed project does not need a matching
`[[scene.objects]]` entry.

## 5. Project entry

```csharp
internal unsafe interface game_entry {
    script_status initialize(native_api* api, managed_api* scripts);
}
```

The interface is internal to `Deren.Managed`; `InternalsVisibleTo("Game")` grants the project
assembly access. The project defines exactly one concrete type with a parameterless constructor.
The entry must register both callbacks before returning success. If initialization fails, the
host reports the returned status and does not run a scene.

`managed_api.stop` is program-wide, not per tree. It runs script `on_destroy` in reverse attach
order, detaches the active tree, and destroys it. A failed shutdown returns `script_failure`.

## 6. Error model

- `module_unavailable`: the native host does not publish the scene module.
- `module_invalid`: the table is missing a required pointer.
- `asset_not_found`: the library id or enumeration index is absent.
- `asset_invalid`: the asset handle is stale or zero.
- `scene_not_found`: the tree id is stale or foreign.
- `invalid_node`: the node handle is stale or belongs to another tree.
- `spawn_unavailable`: the current host has no native spawn slots; managed projects spawn by
  creating nodes and binding assets themselves.

Status values are returned through the ABI; no exception crosses the native boundary.

## 7. Layout and versioning

- One global `managed_abi_version`, currently 16.
- No `context` parameter and no per-module version.
- No `size` fields; one version means one exact layout.
- A field addition, removal, reorder, or signature change bumps the global version.
- The host compares the managed `initialize_from_engine` version before it loads the project
  assembly and returns `abi_mismatch` on any difference.

## 8. Version history

- v5 introduced the core table plus typed module resolver.
- v7 replaced positional update arguments with `frame_info`.
- v12 added the host main tree and native spawn slots.
- v13 moved the project assembly path into `initialize_from_engine`.
- v14 added the `scene_builder` startup path and output-node buffer.
- v15 removed the host main tree, `open_main_scene`, `is_main`, `try_open_main`, `own_scene`,
  and `use_builder`; C# built and activated a standalone tree.
- v16 removed `start_scene`, `update_scene`, `stop_scene`, `scene_builder`, object counts,
  bindings, and descriptor structures. `initialize_from_engine` now calls the project
  `game_entry` directly; the project builds, activates, and binds its scene and registers
  `update`/`stop` itself. `deactivate_scene` was added for orderly shutdown.
