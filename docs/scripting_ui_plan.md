# C# Scripting and Scene UI Plan

Status: implemented for the managed scene path; editor panel work remains. Managed ABI v16 and
the `scene` module are in place. A project `game_entry` owns scene construction, activation,
asset binding, scripts, and the frame/shutdown callbacks.

Baseline: `feature/csharp-scene-tree`.

## 1. Current native side

The native app still owns:

- Vulkan and the runtime;
- project configuration, native preview materials, and render settings;
- the scene module table and `runtime.set_scene`;
- the native preview scene and its asset preload, used only when no project assembly is
  configured.

For a managed project, `app_scripting` only prepares the asset library, installs the native
scene host, sets `starting_scene`, and calls the managed bootstrap. It does not create object
roots, collect object counts, build a binding array, or publish transforms.

`asset_manager.try_load` decodes the selected library file and resolves its materials into the
managed asset registry. `bind_managed_asset` imports that source under the managed root through
`runtime.import_scene` and returns success only when renderable primitives were created. Native
project object slots and `[[scene.objects]]` preloads are not part of this path. Asset handles
and scene trees remain managed objects; no native pointer or `scene_node` crosses the ABI.

## 2. Current managed side

The project assembly implements exactly one `game_entry`:

```csharp
internal unsafe interface game_entry {
    script_status initialize(native_api* api, managed_api* scripts);
}
```

The test project (`source/scripting/test_game/game_host.cs`) demonstrates the intended flow:

1. `scene_api.initialize(api)`.
2. `scene_tree.try_create`.
3. `asset_manager.try_load` for each chosen asset.
4. Create roots and children with `try_create_node`.
5. `try_activate`.
6. `try_bind_asset` for each renderable instance.
7. Attach scripts and register `scripts->update` / `scripts->stop`.

Game code chooses its own assets and object layout. `[[scene.objects]]` is native project
metadata and is not forwarded to a managed project.

## 3. Frame and shutdown

`managed_api` contains only:

```cpp
script_status(__cdecl* update)(frame_info const*);
script_status(__cdecl* stop)();
```

`update` receives the native script-scene id and one frame of borrowed input. The managed tree
id is separate and can be read with `get_active_scene`. A null input pointer is valid and means
no input actions this frame. `stop` is program-wide: scripts receive
`on_destroy` in reverse order, then the project detaches and destroys its tree.

The native host calls `stop` once during shutdown and never calls `update` after it.

## 4. Scene tree API

`scene_tree` exposes create/activate/destroy, node create/destroy, local transform read/write,
subtree visibility, and asset binding. It never exposes native handles.

`asset_manager` exposes `list`, `try_load`, and `release`. A handle is a shared source model;
binding copies the hierarchy into a tree, so releasing the handle after binding is safe.

## 5. Editor and asset authoring

The existing native editor and asset library remain the authoring tools:

- `[scripts] assembly` selects the project entry assembly.
- The asset library supplies ids and preloaded models.
- The managed project decides which ids to load and where to place them.
- A future editor panel can display managed scene nodes through a read-only snapshot; it must
  not mutate the managed tree behind the project entry's back.

## 6. Historical ABI path

- v5 to v11 grew the module resolver and scene module table.
- v12 added the host main tree and native spawn slots.
- v13 moved the project assembly path into `initialize_from_engine`.
- v14 added `scene_builder` and the `start_scene` output-node buffer.
- v15 removed the host main tree, `open_main_scene`, `own_scene`, and `use_builder`; C# built
  and activated a standalone tree.
- v16 removed `start_scene`/`update_scene`/`stop_scene`, `scene_builder`, object counts,
  bindings, and descriptor structures. `initialize_from_engine` calls the project
  `game_entry`; the project builds and activates its scene and registers its own callbacks.

The previous host-main-tree design is intentionally not a compatibility path. There is no
native fallback scene, empty object root, or hidden spawn root for a scripted project.

## 7. Tests

`managed_host_*` covers bootstrap failures, ABI mismatch, module availability, node lifetime,
activation/deactivation, visibility, asset load/bind/release, and frame/stop. `test_docs` checks
links and formatting; `clang-format-check` covers native sources.
