# C# Scripting and Scene UI Plan

Status: implemented for the managed scene path and the managed debug panel; editor panel work
remains. Managed ABI v32, the `scene` module and the `ui` module are in place. A project `game_entry` owns startup parsing, scene construction,
activation, asset binding, scripts, and the frame/shutdown callbacks.

Baseline: `feature/csharp-scene-tree`.

## 1. Current native side

The native app still owns:

- Vulkan and the runtime;
- project configuration, native preview materials, and render settings; the window and runtime
  consume `startup_options` filled by the project's `preinit`;
- the scene module table and `runtime.set_scene`;
- the native preview scene and its asset preload, used only when no project assembly is
  configured.

For a managed project, `app_scripting` loads the managed host before the window exists, calls the
project's `preinit` to fill `startup_options`, and later installs the native scene host and calls
`initialize`. It does not parse project startup flags, create object roots, collect object counts,
build a binding array, publish transforms, or choose a camera fallback.

`asset_manager.try_load` decodes the selected library file and resolves its materials into the
managed asset registry. `bind_managed_asset` imports that source under the managed root through
`runtime.import_scene` and returns success only when renderable primitives were created. Native
project object slots and `[[scene.objects]]` preloads are not part of this path. Asset handles
and scene trees remain managed objects; no native pointer or `scene_node` crosses the ABI.

## 2. Current managed side

The project assembly implements exactly one `game_entry`:

```csharp
internal unsafe interface game_entry {
    script_status preinit(startup_options* options, int argc, byte** argv);
    script_status initialize(native_api* api, managed_api* scripts);
}
```

The test project (`source/scripting/test_game/game_host.cs`) demonstrates the intended flow:

1. `preinit` parses the original command line into `startup_options`.
2. `scene_api.initialize(api)`.
3. `scene_tree.try_create`.
4. `asset_manager.try_load` for each chosen asset.
5. Create roots and children with `try_create_node`.
6. `try_activate`.
7. `try_bind_asset` for each renderable instance.
8. Attach scripts and register `scripts->update` / `scripts->stop`; the bootstrap installs
   `scripts->signal` for after-frame dispatch.

Game code chooses its own assets and object layout. `[[scene.objects]]` is native project
metadata and is not forwarded to a managed project.

## 3. Frame and shutdown

`managed_api` contains:

```cpp
script_status(* update)(frame_info const*);
script_status(* stop)();
script_status(* signal)(std::uint32_t signal);
```

`update` receives the native script-scene id and one frame of borrowed input. The managed tree
id is separate and can be read with `get_active_scene`. A null input pointer is valid and means
no input actions this frame. `stop` is program-wide: scripts receive
`on_destroy` in reverse order, then the project detaches and destroys its tree.

The native host emits `signal(after_frame)` after a successfully presented frame. Managed code
owns the callables queued by `frame_scheduler.do_after_frame`; it detaches the queue and runs each
callable outside the lock. Callables queued during dispatch run on the next signal, and managed
exceptions are logged instead of crossing the native boundary. The native host calls `stop` once
during shutdown and never calls `update` or `signal` after it.

## 4. Scene tree API

`scene_tree` exposes create/activate/destroy, node create/destroy, local transform read/write,
subtree visibility, and asset binding. It never exposes native handles.

`asset_manager` exposes `list`, `try_load`, and `release`. A handle is a shared source model;
binding copies the hierarchy into a tree, so releasing the handle after binding is safe.

## 5. Debug panel

The debug panel is a HOST-DRAWN, MANAGED-DESCRIBED surface: the native overlay plugin renders the
window content, and the widget list is the C# game's (`deren.ui`), rebuilt every frame. Nothing
about the panel's content lives on the native side; the host owns the widget backend and the input
plumbing, the project owns which knobs exist.

- `[gui] show` in the project config decides whether the overlay is loaded at all. With it off the
  `ui` module stays published and every call reports `module_unavailable`, so a game can run the
  same code in both modes.
- The host caches one panel per process: `begin_panel` creates the overlay panel on first use and
  clears its contents every frame, so a panel is a description of the current frame, not state.
- `[gui] panel_width` / `panel_height` are the size a panel gets when the game names none, and a
  zero axis auto-fits to the content. The demo project pins the width (labels sit beside their
  widgets) and lets the height follow its rows.
- A control's value lives in unmanaged memory owned by the game (`deren.ui_value`): the overlay
  binds the pointer while it draws, which is after the update call returned.
- A button reports its press through the caller's byte during the draw; the game reads it on the
  next update, exactly like a one-shot key.
- `panel::add_button` is why the GUI plugin ABI moved to 7. The panel verbs are the plugin's
  existing label/checkbox/slider/vec3/combo set; there is no markup layer (no RmlUi), because a
  widget set the engine already ships does not need a second UI toolkit.

Window events reach the game through `native_api.set_input_event_callback`: one handler, on the
render thread, for keys, mouse buttons, cursor moves (with deltas), scroll, typed characters, and
focus loss. The host keeps no input state - the engine's named-action map stays a separate reader
of the same window - and the callback stops when the game passes null.

## 6. Editor and asset authoring

The existing native editor and asset library remain the authoring tools:

- `[scripts] assembly` selects the project entry assembly.
- The asset library supplies ids and preloaded models.
- The managed project decides which ids to load and where to place them.
- A future editor panel can display managed scene nodes through a read-only snapshot; it must
  not mutate the managed tree behind the project entry's back.

## 7. Historical ABI path

- v5 to v11 grew the module resolver and scene module table.
- v12 added the host main tree and native spawn slots.
- v13 moved the project assembly path into `initialize_from_engine`.
- v14 added `scene_builder` and the `start_scene` output-node buffer.
- v15 removed the host main tree, `open_main_scene`, `own_scene`, and `use_builder`; C# built
  and activated a standalone tree.
- v16 removed `start_scene`/`update_scene`/`stop_scene`, `scene_builder`, object counts,
  bindings, and descriptor structures. `initialize_from_engine` calls the project
  `game_entry`; the project builds and activates its scene and registers its own callbacks.
- v20 added the after-frame signal and the managed `do_after_frame` queue; native code emits only
  the frame boundary and never stores individual callables.
- v21 added `deren.screenshot.try_request()`, forwarding to the same one-shot screenshot path as
  the F12 key.

The previous host-main-tree design is intentionally not a compatibility path. There is no
native fallback scene, empty object root, or hidden spawn root for a scripted project.

## 8. Tests

`managed_host_*` covers bootstrap failures, ABI mismatch, module availability, node lifetime,
activation/deactivation, visibility, asset load/bind/release, and frame/stop. `test_docs` checks
links and formatting; `clang-format-check` covers native sources.
