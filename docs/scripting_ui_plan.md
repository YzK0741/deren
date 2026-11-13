# C# Scripting and Native UI Plan

Status: design proposal, not implemented.
Baseline: `master` at `07bc77e4 feat(scripting): add input actions and safe scene instancing (#77)`.
The current managed ABI is v4 and the current UI is the Dear ImGui GUI plugin.

## 1. Goals

- Let C# drive gameplay logic without exposing C++ object addresses, STL types, exceptions, or the
  `runtime` class across the boundary.
- Keep entity access handle-based and generation-checked; make lifetime errors explicit rather than
  undefined behavior.
- Keep the native frame loop deterministic: scripts update once per frame, and structural scene edits
  are applied at a known point.
- Add a native, retained-mode game UI that can be styled and animated, and that C# can control through
  a separate versioned ABI.
- Keep the existing Dear ImGui overlay for diagnostics and developer tooling.

## 2. Non-goals for the first version

- Arbitrary C# components or reflection-based component access.
- Async `Task` callbacks or script code on worker threads.
- Hot reload, collectible assembly unload, or mod sandboxing.
- A full editor, accessibility/UIA, or a complete CSS implementation.
- Exposing `scene_node*`, `primitive*`, `runtime*`, or backend objects to C#.

## 3. Current baseline

- `source/scripting/managed_host.cppm` defines `managed_abi_version = 4`, `native_api`, and
  `managed_api`.
- `native_api` currently contains `log`, `get_transform`, `set_transform`, `try_spawn`, and
  `try_despawn`.
- `managed_api` currently contains `start_scene`, `update_scene`, and `stop_scene`.
- `source/app/app_scripting.cpp` binds configured scene objects to managed instances and publishes
  transform changes back to `scene_object_settings`.
- C# currently exposes `script_behaviour`, `scene_access`, `node_id`, `input_state`, and `transform_value`.
- `try_spawn` currently reuses a predeclared `spawn_slot`; it does not create arbitrary scene objects.
- The app frame already calls `script_scene.update` and `publish_transforms` before the render work.

## 4. Decisions

### D1: C# sees gameplay entities, not scene nodes

C# receives an opaque `node_id` carrying scene identity, entity index, and generation. It never receives a scene-tree pointer or
a native object address. The entity may have an optional `scene_link`; the link is a scene-node handle,
not a pointer.

### D2: flat behaviour scheduling

C# scripts attach to gameplay entities. The host calls:

```text
on_create
on_update(delta_time) once per frame
on_destroy
```

The native game scene can have a parent/child hierarchy while the host dispatches behaviours from a
flat list. Parent relationships determine transform inheritance. The host determines the update order
and can add a future `on_physics_update` phase or event callbacks.

### D3: managed ABI v5 uses versioned argument structures

The function-table shape stays one table per direction. New fields and arguments go into versioned
structs instead of growing positional signatures. The ABI version is checked before any callback is
stored.

### D4: spawn semantics stay narrow in v1

v1 `try_spawn` only consumes predeclared `spawn_slot` entries. It returns a status instead of a bare
boolean. Arbitrary entity/asset creation is a later deferred-command feature.

### D5: no user-defined C# components in v1

C# behaviour state lives in C# fields. Engine components remain native and are exposed through a small
typed API (`transform`, liveness, later visibility/hierarchy). This avoids GC layout, reflection, and
cross-language component storage.

### D6: game UI is a separate native module

Add a `deren.ui` module for retained-mode game UI. It depends on the RHI and text interfaces, not on
Vulkan. The Vulkan backend supplies the renderer. The existing GUI plugin remains the debug overlay.

### D7: RmlUi is the preferred UI implementation

Use RmlUi for the retained document/element tree, layout, and RCSS styling, with a custom deren RHI
renderer and input adapter. This is a dependency decision to be confirmed by a small spike before the
full integration. If RmlUi is rejected, the fallback is a minimal in-house retained core; do not start
with a full CSS engine.

### D8: C# UI uses an optional function table

The scripting `native_api` exposes `get_module`. Each scene resolves the optional, independently
versioned `ui_api_v1` at scene start. `Deren.UI` remains the managed assembly for UI. UI calls run
on the scene thread. See [C# Module ABI](scripting_module_abi.md).

## 5. C# gameplay API (v1.1 target)

Keep the existing lowercase naming and add to it.

```csharp
namespace deren;

public readonly struct node_id : IEquatable<node_id>
{
    public bool is_valid { get; }
    public bool is_alive();
    public bool try_get_transform(out transform_value value);
    public bool try_set_transform(in transform_value value);
}

public enum spawn_result
{
    success = 0,
    unavailable = 1,
    unknown_asset = 2,
    invalid_pose = 3,
    limit_reached = 4
}

public abstract class script_behaviour
{
    protected node_id self { get; }
    protected scene_access scene { get; }
    protected transform_value transform { get; set; }

    public virtual void on_create() { }
    public virtual void on_update(float delta_time) { }
    public virtual void on_destroy() { }
}

public readonly struct scene_access
{
    public node_id self { get; }

    public spawn_result try_spawn(string asset_id, in transform_value pose,
                                  out node_id target);
    public bool try_despawn(node_id target);
}

public static class input_state
{
    public static bool is_down(string action);
    public static bool was_pressed(string action);
    public static bool was_released(string action);
    public static float axis(string negative, string positive);
}
```

Semantics:

- `transform` is a convenience for reading and writing the local transform of `self`. Programmer errors throw.
- `scene.try_get_transform` and `scene.try_set_transform` use parent-relative local values.
  A future world-transform query reads a derived result.
- `scene.try_*` returns `false` for stale handles or expected failures; it does not throw.
- Unknown input actions are rejected at scene start. Runtime lookup of an unknown action logs once and
  returns `false`.
- `node_id` is a value handle; it is not serializable and does not reveal scene/index/generation.

## 6. Native scripting ABI (v5 target)

```cpp
struct scene_start_info {
    std::uint32_t size;
    std::uint32_t version;
    native_api const* api;
    std::uint64_t scene;
    utf8_view assembly;
    script_binding const* bindings;
    std::uint32_t binding_count;
    utf8_view const* action_names;
    std::uint32_t action_count;
};

struct frame_info {
    std::uint32_t size;
    std::uint32_t version;
    std::uint64_t scene;
    float delta_time;
    std::uint8_t const* input;
    std::uint32_t input_count;
};

struct managed_api {
    std::uint32_t size;
    std::uint32_t version;
    script_status(__cdecl* start_scene)(scene_start_info const*);
    script_status(__cdecl* update_scene)(frame_info const*);
    script_status(__cdecl* stop_scene)(std::uint64_t);
};
```

The v5 core table exposes the module resolver defined in [C# Module ABI](scripting_module_abi.md).
Gameplay callbacks move to `scene_api_v1`. The P0 gameplay hardening adds `is_alive` to that table
and extends the statuses returned by `try_spawn`.

```cpp
bool(__cdecl* is_alive)(void* context, node_handle entity); // scene_api_v1
```

Rules:

- All payloads are POD, fixed-width, and explicitly laid out.
- UTF-8 views carry pointer plus byte length.
- No C++ exceptions cross the boundary; no C# exceptions cross the boundary.
- The C# mirror must be generated or checked against the C++ layout. A hand-maintained duplicate is
  acceptable only with a layout/size test.
- `size` guards allow appending to a table; `version` rejects incompatible tables.
- P3 appends a pointer and count for borrowed UI events to `frame_info`. The managed reader checks
  the payload size before reading the fields and treats a missing suffix as an empty batch.

## 7. Lifecycle, errors, and threading

- `on_create` runs after the configured entities, parent links, and components are ready. It may
  request a spawn. The new instance becomes visible after scene changes are published.
- `on_update` runs once per gameplay frame, in binding order. An acquire or present retry reuses the
  frame's script results.
- `on_destroy` runs once before the handle is invalidated. When a subtree is destroyed, callbacks
  run from children to parents.
- An `on_create` exception fails scene start and rolls back.
- An `on_update` exception is logged and disables that instance; other instances continue.
- An `on_destroy` exception is logged; destruction continues.
- All callbacks and scene APIs run on the scene thread. No `Task`, `async`, or thread-pool continuation
  may call scene APIs.
- A stale `node_id` returns `false` or `spawn_result.unavailable`; it must not panic or dereference
  native memory.

## 8. Entity and ECS boundary

- The native game scene is the sole authority for gameplay entity identity, parent relationships, and
  local transforms, including entities associated with render nodes. Scene configuration supplies the
  initial values. Transform-only entities can serve as attachment points.
- The game scene derives a world transform by multiplying the parent's world transform by the
  entity's local transform. World transforms may be cached and recomputed when their inputs change.
- A gameplay entity may carry an internal `scene_link` handle. The render scene consumes the published
  gameplay world transform and owns the imported model's internal nodes, bone transforms, and render
  leaves. Picking and culling use the same published placement.
- Local transform writes are visible to later scripts in the same update batch. World-transform
  queries use the committed hierarchy and the latest local values.
- Spawning, reparenting, and removal take effect at the scene update boundary after script dispatch.
  Validate scene identity, entity generation, and parent cycles before publishing a structural change.
- Cross-frame references use stable handles with generation checks. Refresh the render BVH and culling
  data through `runtime::scene_changed()` when a published change invalidates them.

Frame order target:

```text
poll_events
input_state.begin_frame
script_scene.update
commit scene commands
resolve gameplay world transforms
publish world transforms and visibility to rendering
scene_changed when published changes invalidate BVH or culling
pace_and_acquire
animation update
begin_recording
```

## 9. Native game UI

### 9.1 Layer diagram

```text
app / runtime
    -> deren.ui::context
         document / element tree
         style + layout + animation
         focus / input / hit testing
         draw list

Vulkan backend
    -> deren.ui::renderer
         RHI textures, font atlas, clipping, batching

C# Deren.UI
    -> optional ui_api_v1 resolved per scene through get_module
```

The UI module must not include Vulkan headers or depend on the GUI plugin. It records backend-neutral
draw data through the RHI.

### 9.2 Retained mode, not ImGui

- Widgets persist across frames.
- C# updates properties/events; it does not rebuild the whole tree every frame.
- Layout runs after property changes, not on every draw call.
- Styles support normal/hover/pressed/disabled states and transitions.
- The debug overlay remains Dear ImGui and is not the public game UI API.

### 9.3 RmlUi integration

RmlUi provides the document model, HTML-like markup, RCSS, and layout. deren supplies:

- a renderer implementing RmlUi's draw interface over the RHI;
- a texture/font atlas bridge;
- an input adapter for pointer, keyboard, focus, scroll, and IME;
- a file/resource interface for `ui/` documents and styles;
- a C ABI that exposes document and element handles, property updates, and events to C#.

RML and RCSS define layout and presentation. The native scene UI owns the RmlUi document tree,
element lifetime, focus, hit testing, and pointer capture. C# gameplay owns game state and updates
the UI from that state. An event such as a button click reports player intent. Gameplay decides
whether the action succeeds and updates the display after the result is known.

### 9.4 C# binding

The first managed API loads project-relative RML, looks up elements by ID, sets plain text, and
subscribes to clicks. It follows the existing snake_case conventions.

```csharp
if (ui.try_load("ui/hud.rml", out var hud) &&
    hud.try_find("helmet-count", out var label) &&
    hud.try_find("spawn-helmet", out var button)) {
    label.set_text($"Helmets: {helmet_count}");

    button.clicked += () => {
        if (scene.try_spawn("damaged-helmet", spawn_pose, out _)) {
            label.set_text($"Helmets: {++helmet_count}");
        }
    };
}
```

- Resolve the optional `ui_api_v1` from `get_module` during `start_scene`. Store the validated
  table and its context in `scene_state`. An unavailable UI module makes `ui.try_load` return
  `false`, including in builds with RmlUi disabled. A malformed table fails scene binding.
- Pass UTF-8 views, status codes, and opaque document and element handles across the ABI. The native
  UI validates the scene and handle generation on each call. Closing a document or unloading a
  scene invalidates its handles. Release managed subscriptions when their owner is destroyed.
- Run UI calls on the scene thread. Route input to RmlUi before publishing the gameplay action
  snapshot and exclude consumed input from that frame's actions. Keep release handling paired with
  accepted presses so that gameplay actions cannot remain held when UI focus changes.
- Queue native UI events during RmlUi dispatch. Pass the batch in `frame_info` and deliver it at the
  start of the script update phase, before behaviour updates. Handlers may update the UI and call
  gameplay APIs.
  Discard queued events for closed documents or unloaded scenes. Catch and log handler exceptions
  in managed code.
- Apply script changes before RmlUi updates layout and paints. Preserve the document tree across
  frames and update text when the underlying value changes.

### 9.5 Text, fonts, and input

The UI plan must explicitly cover:

- font fallback and CJK glyph coverage;
- text shaping and line breaking;
- DPI scaling and pixel snapping;
- IME composition and candidate windows;
- pointer capture, focus, hover, and keyboard navigation.

English is the primary language of the sample and first playable UI. Use an optional CJK fallback
font to verify Chinese glyph rendering. Schedule IME with the later text input work.

## 10. Build and packaging

- Keep C# optional: `DEREN_ENABLE_CSHARP=ON` adds the scripting targets; a C++-only build remains valid.
- `Deren.Managed.dll` remains the managed host/contract assembly; user assemblies reference it.
- `Deren.UI` is the managed assembly over the optional UI function table. It uses the existing
  `Deren.Managed.dll` bootstrap and scene lifecycle.
- RmlUi is an optional native dependency behind a CMake option; if disabled, game UI is unavailable but
  the engine and debug GUI still build.
- The managed runtime files are copied beside the executable under `managed/` as they are today.

## 11. Phases

### P0: ABI and gameplay API hardening

- Introduce `get_module`, `scene_start_info`, and `frame_info` in ABI v5. Migrate the existing
  callbacks to `scene_api_v1` first, then add `is_alive` during gameplay API hardening.
- Add `spawn_result`, `scene.try_get_transform`, `scene.try_set_transform`, and `input_state.axis`.
- Generate or layout-test the C# interop structures.
- Tests: stale handles, unknown actions, spawn statuses, null/invalid arguments.

### P1: Scene/ECS integration

- Give scripted and unscripted gameplay objects stable entity handles. Resolve authored `id` and
  `parent` references and support transform-only attachment points.
- Make the game scene own runtime local transforms and parent links. Resolve world transforms in
  parent-first order and publish them to rendering before culling.
- Validate duplicate IDs, missing parents, and cycles. Verify that a scripted parent's translation
  and rotation carry an unscripted prop through a transform-only attachment point.
- Add deferred structural commands when runtime reparenting or arbitrary spawning is introduced.

### P2: Native UI foundation

- Add `deren.ui` context, element tree, draw list, RHI renderer interface.
- Integrate RmlUi behind a CMake option.
- Render a static document with fonts and clipping.

### P3: UI input and C# binding

- Add pointer input, click dispatch, and gameplay action consumption for the first managed binding.
- Publish the optional `ui_api_v1` table and expose `try_load`, `try_find`, `set_text`, and
  `clicked`. Append the UI event batch to `frame_info`.
- Use the two-helmet demo to load an English HUD with a `Spawn helmet` button and a count label.
  A successful spawn increments the count and updates the label in the same frame. A rejected
  spawn leaves the count unchanged. Closing and reopening the scene clears old subscriptions,
  rejects stale handles, and gives one callback per click in the new scene.
- Follow with focus, pointer capture, scroll, keyboard navigation, and IME. Add style updates once
  the text and click path is verified end to end.

### P4: Tooling and polish

- Transitions and animation.
- Hot reload only in editor builds.
- UI tests and golden images.
- Accessibility evaluation.

## 12. Risks and open decisions

1. **RmlUi dependency**. The native renderer spike is complete. Verify IME integration during the
   text input work.
2. **Spawn model**: v1 is limited to predeclared slots. Arbitrary runtime spawning uses the deferred
   scene command path.
3. **Entity access level**: v1 exposes handles, local transforms, liveness, and spawn/despawn. The
   scene file can declare a hierarchy in P1. Public hierarchy queries, name lookup, and runtime
   reparenting can follow the native implementation.
4. **UI thread**: the UI must share the scene thread; a separate UI thread would require a command queue
   and is not planned.
5. **Text**. Verify CJK glyph coverage with the optional fallback font. Validate IME composition
   and candidate positioning when adding editable text.
6. **Interop drift**: hand-written C++/C# mirrors are error-prone; prefer generation from a small ABI
   description and keep a layout test either way.

## 13. Reference notes

Godot gives scripts broad access to the SceneTree because nodes are native objects with a retained
object model. deren does not copy that: it exposes handle-based gameplay entities and keeps scene-tree
ownership native. The useful Godot lessons are the hostfxr bootstrap, one function table per direction,
weak/strong GC handle discipline, and a retained UI model; the full Node API and reflection surface are
not goals.
