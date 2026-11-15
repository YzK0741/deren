# C# Module ABI

Status: implemented for the `scene` and `ui` modules; managed ABI v32.

Baseline: `feature/csharp-scene-tree`. Native code owns rendering, the Vulkan runtime, the
module resolver, and primitive/scene import. Managed code owns scene construction, asset
selection and decoding, scene activation, scripts, and the frame/shutdown callbacks. The
native preview path is gone: `[[scene.objects]]` is parsed for tooling only and the host
imports nothing from it.

## 1. Ownership boundary

The host has no fallback scene for a scripted project. Startup is split so the project owns
every startup decision before the window and the Vulkan runtime exist:

1. Load `Deren.Managed.dll` and the project assembly.
2. Find exactly one concrete `game_entry` in the project assembly.
3. Call `game_entry.preinit(startup_options*, int argc, byte** argv)` with the original command
   line. The project fills `startup_options`; it must not touch the scene or the renderer.
4. The host creates the window and the Vulkan runtime from those options.
5. Call `game_entry.initialize(native_api*, managed_api*)`.
6. Validate that the project registered `managed_api.update` and `managed_api.stop`; the
   managed bootstrap installs the `managed_api.signal` entry used for frame boundaries.

The project entry:

1. Resolves `scene_api` from `native_api.get_module` during `initialize`.
2. Creates a standalone `scene_tree`.
3. Selects and loads assets through `asset_manager`.
4. Creates every root and child node.
5. Calls `scene_tree.try_activate()`.
6. Binds renderable assets with `scene_tree.try_bind_asset`.
7. Attaches scripts.
8. Writes its `update` and `stop` function pointers into `managed_api`; the bootstrap installs
   the frame signal entry.

There is no `start_scene`, `scene_builder`, object count, binding array, or native object-root
allocation. `[scripts]` selects the project assembly (the managed path); `[[scene.objects]]`
remains parsed project metadata for tooling only, and a managed project decides its own object
graph.

## 2. Core structures

```cpp
struct native_api {
    void(* log)(char const* utf8, std::uint32_t length) = nullptr;
    script_status(* get_module)(std::uint32_t module, void const** table) = nullptr;
    script_status(* request_screenshot)() = nullptr;
    void(* set_input_event_callback)(void(* callback)(void* context, input_event const* event),
                                     void* context) = nullptr;
};

enum class input_event_type : std::uint32_t {
    mouse_move = 1,
    mouse_button,
    scroll,
    key,
    character,
    focus_lost,
};

struct input_event {
    input_event_type type;
    std::int32_t button;
    std::uint8_t down;
    std::uint8_t padding[3];
    std::int32_t key;
    float x, y, dx, dy, wheel;
    std::uint32_t codepoint;
};

enum class frame_signal : std::uint32_t {
    after_frame = 1
};

struct managed_api {
    script_status(* update)(frame_info const* frame) = nullptr;
    script_status(* stop)() = nullptr;
    script_status(* signal)(std::uint32_t signal) = nullptr;
};

struct startup_options {
    std::int32_t capture_frames = 0;
    std::int32_t capture_windowed = 0;
    std::int32_t camera_set = 0;
    float camera_yaw_degrees = 0.0F;
    float camera_pitch_degrees = 0.0F;
    float camera_distance = 2.2F;
    std::int32_t target_set = 0;
    vector_3 target{};
    float sweep_yaw_deg_per_frame = 0.0F;
    float animation_seconds_per_frame = 0.0F;
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

The managed bridge exposes the `preinit`/`initialize` lifecycle entries plus the frame signal.
It does not publish the scene table itself; the project loads and initializes `scene_api`.

`native_api.set_input_event_callback` carries the WINDOW'S EVENT STREAM to managed code: key,
mouse button, cursor move (with the delta since the previous move), scroll, typed character, and
focus loss. The callback arrives on the render thread, in the order the window produced the events,
and `context` is handed back untouched. The host keeps no input state of its own - what a wheel
notch or an escape key means is the game's decision - and this stream is separate from the
named-action bytes in `frame_info`, which the engine's own action map fills. A null callback stops
the forwarding.

`native_api.request_screenshot` is the scripted counterpart of the F12 key: it asks the renderer
to capture the next frame recorded after the request, which is then saved under
`[paths] screenshot_dir` with a timestamped file name. C# projects call
`deren.screenshot.try_request()`; there is no path argument and the call is a request, not a
synchronous save.

`scene_api.set_environment_hdr(path, slot)` replaces ONE image-based-lighting slot. The path names
a baked `.ibl` or an equirectangular `.hdr`; a relative path is resolved against the executable
directory, and an empty path loads the procedural sky into that slot. The call only validates and
queues the request: the host prepares and uploads the IBL between frames, so a success means the
slot and the path were accepted, and a later decode failure keeps that slot's current environment.

Slot 0 is the scene-wide environment every rendered scene shares, and it is the only slot with a
BRDF LUT (the LUT is shared, so the custom slots upload none). Slots 1..3 are the per-material
lanes: a material record carries the slot number in its flags (`material_override.environment_slot`
on the C# side), and the shaders sample that slot's cubemap pair out of the descriptor heap. The
engine's `layout::heap_environment_capacity` is the bound; a larger slot is rejected with
`invalid_argument`. There is still no per-scene-tree override - the lanes are selected per material
- and one request is in flight at a time: a newer call replaces a queued one, whatever slot it names.
C# projects call `deren.global_environment.try_set_hdr(string? path)` for slot 0 and
`deren.global_environment.try_set_slot_hdr(int slot, string? path)` for any slot.

Each asset's `asset.toml` may declare the renderer pipelines it supports and the one a bind starts
on (`default_pipeline`, `pipelines`). `bind_asset` applies the declared default to the imported
subtree, and `set_pipeline` switches every imported primitive under a bind root to another declared
name. The pipeline names are the ones the project registers with the renderer (`pbr`, `unlit`,
`game_toon`); a gaussian asset declares none. C# projects read the declarations through
`asset_manager.supported_pipelines` / `asset_manager.try_get_default_pipeline` and switch with
`scene_tree.try_set_pipeline`.

An `asset.toml` may also declare the up axis its model was authored in (`up_axis = "y"` or `"z"`,
default `"y"`). The engine, its shaders and the captured game-toon profiles all assume a Y-up world,
while Blender writes Z-up by default, so a `"z"` asset is turned upright by the loader: the -90 degree
about X correction is pre-multiplied into every SCENE ROOT node's local transform, and the world
matrices are rebuilt. The vertices, the inverse bind matrices, the joints and every animation channel
are left untouched, so the whole asset stays in one frame - joint worlds, inverse bind matrices, the
mesh node and the clips keep agreeing with each other - and the correction arrives on screen exactly
once, through the mesh node's world transform. The captured hair shader reads its root frame as the
world axes for the same reason: normalising the up axis at load is what makes the asset frame and the
world frame coincide, and reading the per-draw model matrix there would re-introduce the authored
axes. The one caveat is an animation channel that targets a scene root node, which would overwrite the
prepended rotation; no current asset does that. `asset/laevatain` declares `up_axis = "z"`.

The `[render]` and `[lighting]` post-processing settings are runtime state too: exposure, tone
mapping, IBL calibration (`set_environment`), sun intensity, bloom, FXAA, TAA, SSAO, and the
G-buffer debug view can change between frames. Each setter validates its arguments against the
renderer's documented range and rejects an out-of-range value with `invalid_argument` instead of
clamping it; a host that publishes no owner for the global state, or no setter, reports
`scene_not_found`, and a host that cannot apply the accepted value reports `script_failure`. C#
projects call the `deren.post_processing.try_set_*` entries.

The `[lighting]` sun direction, the square area emitter, and the clustered-light switch are runtime
state too. `set_sun_direction` takes the direction FROM the surface TOWARD the sun (finite and
non-zero); `set_area_light` takes the emitter centre, its half side length (`half <= 0` clears it,
in which case the axis and penumbra are ignored), the irradiance energy mode, the emitter's normal
axis (non-zero for a live emitter), and a `penumbra >= 0`; `set_clustered_lights` flips the
clustered path. A relight drops the accumulated TAA history, and the status model matches the
post-processing knobs. C# projects call the `deren.lighting.try_set_*` entries.

The punctual light set is runtime state as well, and it is the one lighting entry that carries data
rather than a knob: `set_point_lights` replaces the WHOLE set from an array of `point_light_desc`
records (a count of zero clears it), and the managed side pushes a rig -- a project file, in the
demo's case -- with `deren.lighting.try_set_point_lights`. A record is validated field by field, so a
non-finite value, a non-positive range, or a spot with a zero direction rejects the call and leaves
the previous set applied; a count above the engine's `max_punctual_lights` (128) is rejected rather
than truncated, because a silently dropped light is a picture that no longer matches its data.

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
    get_camera;
    set_camera;
    set_environment_hdr;
    asset_pipeline_count;
    asset_pipeline;
    asset_default_pipeline;
    set_pipeline;
    set_exposure;
    set_tone_mapping;
    set_environment;
    set_sun_intensity;
    set_bloom;
    set_fxaa;
    set_taa;
    set_ssao;
    set_gbuffer_debug;
    set_sun_direction;
    set_area_light;
    set_clustered_lights;
    set_point_lights;   // (point_light_desc const* lights, uint32 count)
    set_material_override;   // (uint64 tree, node_handle node, material_override_desc const* value)
    set_max_fps;            // (double fps)
    set_toon_shading;       // (float steps, float softness)
    set_shadow_enabled;     // (uint8 enabled)
    asset_face_frame;       // (asset_handle asset, face_frame_desc* value)
    set_head_basis;         // (uint32 index, head_basis_desc const* value)
    set_capture_camera;     // (uint8 enabled, float const position[3], uint32 face, float near, float far)
    request_radiance_capture;
    acquire_radiance_image; // (uint32* width, uint32* height, float* rgba, uint32 float_count)
    reset_temporal_history;
    write_radiance_cube;    // (utf8_view path, uint32 size, float const* pixels, uint32 float_count)
};
```

The order is part of the ABI. C# mirrors it with one layout and validates every pointer before
publishing the table. The camera, environment, pipeline, post-processing, and lighting entries are
optional: a host that predates them leaves the pointer null, and the C# wrapper reports
`module_unavailable`.

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
  instance root. Native object slots and their preloaded models are not involved. When the asset's
  manifest declares a default pipeline, the bind applies it before returning.
- `asset_pipeline_count` / `asset_pipeline` enumerate the pipelines one loaded asset declares, in
  manifest order. The names are borrowed from the managed asset registry and stay valid while the
  handle is loaded; a gaussian asset reports zero.
- `asset_default_pipeline` reports the pipeline a bind starts on. It fails with `asset_invalid`
  for an asset that declares none.
- `set_pipeline` switches every imported primitive under a `bind_asset` root to one declared
  pipeline. The root must be the handle `bind_asset` returned on the active tree; an undeclared
  name is `invalid_argument`, a stale or foreign node is `invalid_node`, and a tree that is not
  the active scene is `scene_not_found`. Hidden subtrees switch too.
- `set_exposure` (0.05..20), `set_tone_mapping` (0..2), `set_environment` (rotation in degrees,
  strength 0..64, background 0..3, colour channels 0..1, blur 0..1), `set_sun_intensity` (0..3),
  `set_bloom` (intensity 0..4, threshold 0..0.75), `set_fxaa` (subpixel and edge threshold 0..1),
  `set_taa` (`blend_static` 0..0.99 and `0 <= blend_min <= blend_static`), `set_ssao` (radius >= 0,
  intensity 0..1, samples <= 16), and `set_gbuffer_debug` write the global runtime state the
  `[render]`/`[lighting]` startup settings used to own. They are not per tree; an out-of-range value
  is `invalid_argument`.
- `set_sun_direction` (finite, non-zero), `set_area_light` (finite centre/half/penumbra, `half <= 0`
  clears, a live emitter needs a non-zero axis), and `set_clustered_lights` write the global lighting
  state the `[lighting]` startup settings used to own. A direction or emitter change drops the TAA
  history.
- `set_point_lights` replaces the whole punctual set from `point_light_desc` records: position,
  direction and colour triples, intensity, range, the spot flag, the cone's outer/inner half-angle
  cosines (`has_inner_cos` says whether the inner one is used), and the toon stages' `npr_mode` /
  `npr_data`. A count of zero clears every light. A count above 128, a null array with a non-zero
  count, a non-finite field, a non-positive range, or a spot with a zero direction is
  `invalid_argument`, and the previous set stays.
- `set_max_fps` sets the frame rate cap: zero runs uncapped, a positive value paces the loop. A
  negative or non-finite value is `invalid_argument` (the runtime clamps what it is given, so the
  caller's mistake must not silently become "uncapped"). `set_toon_shading` sets the toon stages'
  diffuse band count (0 keeps them smooth, 32 is the ceiling) and the step softness (0..1);
  `set_shadow_enabled` turns the directional shadow pass on or off, and turning it off clears the
  maps rather than only hiding their contribution.
- `set_material_override` writes per-object material factors for a node and its whole subtree: a
  tint, metallic/roughness/emissive scales, an environment lane (0..3), and the unlit flag. The
  factors replace the imported material rather than accumulating on it, and the host keeps one
  cloned row per leaf, so repeated calls rebuild that clone from the imported row. An `enabled` of
  zero hands the subtree back to its imported materials. A non-finite factor or a lane above 3 is
  `invalid_argument`; a node outside the active tree is `invalid_node`. Hidden subtrees are
  rewritten too, so they come back with the same factors.
- `asset_face_frame` returns the face frame the glTF loader measured while decoding the asset: the
  local `forward`/`right` axes, their world-space counterparts, the world head centre, and the head
  joint/skin index plus a `head_relative` flag. An asset the loader could not measure reports
  `asset_invalid` rather than a made-up frame, and an unknown handle is `asset_not_found`.
- `set_head_basis` publishes the frame one character slot is shaded against, in world space: a
  front/right/up basis, the head centre, and the `captured`/`has_center` flags that tell the shader
  whether the basis came from the measured face and whether the centre is known at all. The index
  must be below the engine's `max_game_toon_characters` (16); a non-finite value, a degenerate
  front/right axis, or a flag above 1 is `invalid_argument`. A slot that is never published keeps
  the glTF basis, so a project that ignores the feature gets the same picture as one that does not.

- `set_capture_camera` installs the offscreen cube-capture camera (`enabled == 0` restores the
  orbit camera and turns the capture pass off). A live camera needs a finite position, a face in
  0..5, a positive near plane, and a far plane beyond it. `request_radiance_capture` arms the readback
  for the next frame; it accepts one outstanding request at a time. `acquire_radiance_image` copies
  the settled face out as canonicalized RGBA floats (mirrored into the cube convention, alpha exactly
  1) and fills the dimensions it actually had. `reset_temporal_history` drops the accumulated TAA
  history between faces. `write_radiance_cube` writes six collected faces as an IBL1 file.
- The capture renders at the render extent, not at a free size: the face is the frame the window is
  already drawing, so a cube needs a square window whose side is a power of two in 16..512 and a
  mismatched extent is reported instead of being stretched. An existing output file is never
  overwritten (the writer stages a `.part-<clock>` sibling and renames it).

## 4. Panel module table

`get_module` answers one more module, `ui` (2). The table describes a debug panel - a stack of
labelled widgets the host draws into the window - and every entry is optional:

```cpp
struct ui_api {
    script_status(* begin_panel)(utf8_view title, float default_width, float default_height) = nullptr;
    script_status(* end_panel)() = nullptr;
    script_status(* text)(utf8_view value) = nullptr;
    script_status(* checkbox)(utf8_view label, std::uint8_t* value) = nullptr;
    script_status(* slider)(utf8_view label, float* value, float minimum, float maximum) = nullptr;
    script_status(* vec3)(utf8_view label, float* value, float speed) = nullptr;
    script_status(* combo)(utf8_view label, utf8_view const* items, std::uint32_t count,
                           std::int32_t* selected) = nullptr;
    script_status(* button)(utf8_view label, std::uint8_t* pressed) = nullptr;
    script_status(* wants_mouse)(std::uint8_t* value) = nullptr;
};
```

A zero `default_width` or `default_height` leaves that axis to the host's project default
(`[gui] panel_width` / `panel_height`), and a zero project default auto-fits the axis to the
panel's content. The width is what a panel with labels beside its widgets needs; the height
usually follows the rows.

THE WIDGET LIST IS PER FRAME: a panel is `begin_panel`, one call per widget, then `end_panel`, and
the host rebuilds the panel's contents from that description. The host owns the widget pointers
only until the frame it draws ends, so a value the widget edits must outlive the update call -
managed code keeps one unmanaged sixteen-byte cell per control (`deren.ui_value`) instead of a
GC object or a stack local. A button is a one-shot: its press is written into the caller's byte
while the panel is drawn and is readable on the NEXT update.

A label is never empty and a value pointer is never null; `minimum <= maximum`, `speed > 0`, and a
combo carries at most 64 items - anything else is `invalid_argument`. A host without the GUI plugin
still publishes the table, and its entries reject with `module_unavailable` (or report false from
`wants_mouse`), so a game can tell "no panel this session" from "no panel API at all". C# projects
call the `deren.ui` facade and `deren.ui_value`.

## 5. Managed API

```csharp
public sealed class scene_tree : IDisposable {
    public static bool try_create(out scene_tree? tree);
    public bool try_activate();
    public bool destroy();
    public bool try_create_node(node_id parent, string? name, in transform_value local, out node_id node);
    public bool destroy_node(node_id node);
    public bool try_bind_asset(node_id parent, asset_handle asset, in transform_value offset, out node_id root);
    public bool try_set_pipeline(node_id node, string pipeline);
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
string[] asset_manager.supported_pipelines(asset_handle asset);
bool asset_manager.try_get_default_pipeline(asset_handle asset, out string? pipeline);
```

The environment is global:

```csharp
bool global_environment.try_set_hdr(string? path);
```

`path` is a baked `.ibl` or an equirectangular `.hdr`; an empty string restores the procedural
sky. The call queues the change and returns whether the host accepted the path.

The post-processing knobs are global too:

```csharp
bool post_processing.try_set_exposure(float exposure);
bool post_processing.try_set_tone_mapping(int mode);
bool post_processing.try_set_environment(float rotation_degrees, float strength, int background,
                                         float color_r, float color_g, float color_b, float blur);
bool post_processing.try_set_sun_intensity(float scale);
bool post_processing.try_set_bloom(float intensity, float threshold);
bool post_processing.try_set_fxaa(bool enabled, float subpixel = 0.75f, float edge_threshold = 0.166f);
bool post_processing.try_set_taa(bool enabled, float blend_static = 0.9f, float blend_min = 0.5f);
bool post_processing.try_set_ssao(bool enabled, float radius = 0.5f, float intensity = 1.0f, uint samples = 8);
bool post_processing.try_set_gbuffer_debug(bool enabled);
```

Each call returns whether the host accepted the value; a rejected call leaves the previous state
and logs `Game: ... rejected (status)` through the bridge.

The lighting knobs use the same pattern:

```csharp
bool lighting.try_set_sun_direction(vector_3 to_sun);
bool lighting.try_set_area_light(vector_3 centre, float half, bool irradiance, vector_3 axis,
                                 float penumbra = 0.0f);
bool lighting.try_clear_area_light();
bool lighting.try_set_clustered_lights(bool enabled);
public const int lighting.max_point_lights = 128;
bool lighting.try_set_point_lights(point_light[]? lights);
```

`point_light` is the managed authoring record -- position, direction, colour, intensity, range, the
spot flag, cone half angles in DEGREES, and the toon stages' `npr_mode` / `npr_data`. The wrapper
converts the angles to the cosines the light UBO stores, so a rig only ever states what the picture
needs. `light_rig.try_load(path)` reads a `[[light]]` TOML file plus its `[rig]` staging
(`up_axis`, `offset`) and pushes the whole set; `light_rig.try_load_project_rig()` finds
`toon-light-rig.toml` in the project directory.

The head frame follows the same split: the loader measures, the project decides.

```csharp
bool head_basis.try_publish(asset_handle asset, uint character_index = 0);
public const uint head_basis.max_characters = 16;
```

`try_publish` reads the asset's measured face frame, converts it to the world basis the shader
wants (`up = cross(right, front)`), and publishes it for that character slot. When the measurement
carried a head position it also sends the centre, so the shader's head-sphere term has a real
origin; when it did not, the `has_center` flag stays 0 and that term is switched off rather than
measured from the world origin.

The reflection probe is driven entirely from managed code; the host only installs the camera and
performs the readback:

```csharp
public static unsafe class reflect_probe {
    public const uint min_size = 16;
    public const uint max_size = 512;
    public static bool try_install(vector_3 position, uint face, float near_plane = 0.05f,
                                   float far_plane = 100.0f);
    public static bool clear();
    public static bool try_request();
    public static bool try_read(uint size, float[] pixels);
    public static bool reset_history();
    public static bool try_write(string path, uint size, float[] cube);
}

public sealed class reflect_probe_session {
    public reflect_probe_session(vector_3 position, uint size, string path,
                                 float near_plane = 0.05f, float far_plane = 100.0f);
    public string? failure { get; }
    public bool complete { get; }
    public bool active { get; }
    public uint faces_collected { get; }
    public bool begin();
    public bool step();   // one call per frame
}
```

The session owns the six-face cadence (a face settles for eight frames, its radiance is armed for the
ninth, and the readback is collected on the tenth), so a project only calls `step()` in its frame
callback and reacts to `failure`/`complete`.

Frame pacing and the two render switches:

```csharp
bool frame_scheduler.try_set_max_fps(double fps);          // 0 = uncapped
bool post_processing.try_set_toon_shading(float steps, float softness = 0.15f);
bool lighting.try_set_shadow_enabled(bool enabled);
```

Per-object materials hang off the tree that owns the node:

```csharp
bool scene_tree.try_set_material_override(node_id node, in material_override value, bool enabled = true);
material_override material_override.imported;   // tint 1,1,1, scales 1, lit, lane 0
```

`material_override` states what the object needs: `tint`, the three scales, `environment_slot`
(0..3), and `unlit`. The wrapper writes the `enabled` flag alongside the factors so a subtree can be
restored to its imported materials without discarding what the game authored.

A loaded handle owns one decoded and resolved source model. Binding imports that source into the
target tree; the handle can be released after binding. Releasing it does not invalidate bound
nodes. The asset is selected by library id, so a managed project does not need a matching
`[[scene.objects]]` entry.

## 6. Project entry

```csharp
internal unsafe interface game_entry {
    script_status preinit(startup_options* options, int argc, byte** argv);
    script_status initialize(native_api* api, managed_api* scripts);
}
```

The interface is internal to `Deren.Managed`; `InternalsVisibleTo("Game")` grants the project
assembly access. The project defines exactly one concrete type with a parameterless constructor.
`preinit` parses the original process command line and fills `startup_options` (capture frames,
windowed mode, initial orbit camera, target, and frame-indexed sweeps); it runs before the
renderer exists. `initialize` builds the scene and must register both callbacks before returning
success. If either phase fails, the host reports the returned status and does not run a scene.

### Project files

`deren.toml` sits next to the project assembly. The managed bootstrap already knows that
directory, so it reads the manifest and the scene file it names (`[project] main_scene`) itself,
before `preinit`, and exposes the parsed `[lighting]` table (plus the calibration file named by
`[lighting] profile`) as `deren.project_config.lighting` and the `[render]` runtime knobs as
`deren.project_config.render`. Once the scene API is bound, and before `game_entry.initialize`,
the bootstrap applies both as the framework baseline: the environment/background settings, the
sun direction and intensity, the derived square area light, and the look knobs the native host
used to set once at startup (exposure, tone mapping, bloom, FXAA, TAA, SSAO, the G-buffer debug
view, and clustered light culling). The project's own `initialize` and frames can still move all
of it through the runtime setters.

The split is by consumer: the `[render]` keys that describe the window or the frame's sizing
(window size/title, vsync, `render_scale`, `upscale`, ...), the shadow cascade/map sizing, the
GPU-driven paths (`rt_*`, `visibility_buffer`, `subtilis`, `software_raster`), and the
`unlit`/`character_forward`/`game_toon` stage selection stay native - they must exist before the
project's scripts run or are device/engine features rather than per-frame knobs. The G-buffer
channel (`gbuffer_channel`) is the one debug sub-parameter still applied natively.

The native host keeps its own parse of the keys it consumes and never receives this structure:
the two readers agree on the file format, not through the ABI.

`managed_api.stop` is program-wide, not per tree. It runs script `on_destroy` in reverse attach
order, detaches the active tree, and destroys it. A failed shutdown returns `script_failure`.

## 7. Error model

- `module_unavailable`: the native host does not publish the scene module.
- `module_invalid`: the table is missing a required pointer.
- `asset_not_found`: the library id or enumeration index is absent.
- `asset_invalid`: the asset handle is stale or zero.
- `scene_not_found`: the tree id is stale or foreign.
- `invalid_node`: the node handle is stale or belongs to another tree.
- `invalid_argument`: an argument is malformed, such as a pipeline name the asset does not declare
  or a post-processing/lighting value outside the renderer's documented range.
- `spawn_unavailable`: the current host has no native spawn slots; managed projects spawn by
  creating nodes and binding assets themselves.

Status values are returned through the ABI; no exception crosses the native boundary.

## 8. Layout and versioning

- One global `managed_abi_version`, currently 32.
- No `context` parameter and no per-module version.
- No `size` fields; one version means one exact layout.
- A field addition, removal, reorder, or signature change bumps the global version.
- The managed bootstrap compares the ABI version before it loads the project assembly and
  returns `abi_mismatch` on any difference; `configure_from_engine` runs before `initialize_from_engine`.

## 9. Version history

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
- v17 routed the orbit camera through `scene_api`; v18 forwarded raw `argc`/`argv` per project.
- v19 split the project entry into `preinit` (before the renderer) and `initialize` (after it),
  added `startup_options`, and moved the managed host load ahead of window creation.
- v20 added the after-frame signal. The host only emits `frame_signal::after_frame`; managed code
  owns and dispatches its `do_after_frame` queue, including exception handling and shutdown.
- v21 added `native_api.request_screenshot` and the C# `deren.screenshot.try_request()` entry.
- v22 added `scene_api.set_global_environment_hdr` and the C# `deren.global_environment.try_set_hdr`
  entry: managed code names the global environment, and the host prepares and uploads it between
  frames.
- v23 added the per-asset pipeline contract: `asset.toml` declares `default_pipeline` /
  `pipelines`, `bind_asset` applies the declared default, and `scene_api` gained
  `asset_pipeline_count`, `asset_pipeline`, `asset_default_pipeline`, and `set_pipeline` with the
  C# `asset_manager.supported_pipelines`, `asset_manager.try_get_default_pipeline`, and
  `scene_tree.try_set_pipeline` entries.
- v24 added the runtime post-processing/environment contract: `scene_api` gained `set_exposure`,
  `set_tone_mapping`, `set_environment`, `set_sun_intensity`, `set_bloom`, `set_fxaa`, `set_taa`,
  `set_ssao`, and `set_gbuffer_debug`, with the C# `deren.post_processing.try_set_*` entries. The
  `[render]`/`[lighting]` settings are now startup defaults the game may move between frames.
- v25 added the runtime lighting contract: `scene_api` gained `set_sun_direction`, `set_area_light`,
  and `set_clustered_lights`, with the C# `deren.lighting.try_set_*` entries. The `[lighting]` sun
  direction, area emitter, and clustered switch are now runtime state.
- v26 added the punctual light rig: `scene_api` gained `set_point_lights` with the
  `point_light_desc` record, and C# gained `deren.lighting.try_set_point_lights` plus
  `deren.light_rig` for a `toon-light-rig.toml` project file. A rig is data the project pushes; the
  engine only receives the records and applies them to its punctual set.
- v27 added per-object materials: `scene_api` gained `set_material_override` with the
  `material_override_desc` record, and C# gained `scene_tree.try_set_material_override` plus the
  `deren.material_override` factors. The host clones the imported material once per leaf and rebuilds
  that clone from the factors, so an object's tint, roughness, unlit path, or environment lane is
  runtime state rather than an import-time decision.
- v28 added frame pacing and two render switches: `scene_api` gained `set_max_fps`, `set_toon_shading`,
  and `set_shadow_enabled`, with the C# `frame_scheduler.try_set_max_fps`,
  `post_processing.try_set_toon_shading`, and `lighting.try_set_shadow_enabled` entries. The
  `[render] max_fps` key now has its consumer again, and the toon band count and cast shadows are
  runtime state rather than startup-only settings.
- v29 added the face frame and the per-character head basis: `scene_api` gained
  `asset_face_frame` (the loader's measurement of where the painted face looks) and
  `set_head_basis` (the world frame one character slot is shaded against), with the C# entries
  `head_basis.try_publish`. The measurement, the fallback to the glTF basis, and the placement are
  the managed side's business; the loader only reports what it measured, and the runtime only stores
  the frame it is handed. A slot that is never published keeps the glTF basis rather than a zero
  frame, so a project that ignores the feature cannot produce NaNs.
- v31 added the managed debug panel and the window's event stream: `get_module` answers the `ui`
  module with `ui_api` (nine panel verbs), `native_api` gained `set_input_event_callback` for the
  raw `input_event` stream, and the C# side gained the `deren.ui` / `deren.ui_value` /
  `deren.input_events` entries. The GUI plugin's own ABI moved to 7 for `panel::add_button`, which
  is what makes a press reportable at all.
- v30 added the offscreen reflection-probe capture: `scene_api` gained `set_capture_camera`,
  `request_radiance_capture`, `acquire_radiance_image`, `reset_temporal_history`, and
  `write_radiance_cube`, with the C# `reflect_probe` / `reflect_probe_session` entries. The six-face
  loop, its frame cadence, the probe placement, and the output path are the managed side's business;
  the host installs the camera, runs the readback pass, and hands back the canonicalized face. The
  capture is taken at the render extent, so a probe needs a square window at the probe size, and the
  readback alpha is normalized to 1 by the engine's own face canonicalization instead of being
  trusted from the copy.
- v32 made the environment entry slot-aware: `set_global_environment_hdr(utf8_view path)` became
  `set_environment_hdr(utf8_view path, uint32 slot)`, and the C# side gained
  `global_environment.try_set_slot_hdr(int slot, string? path)` beside `try_set_hdr` (which is slot
  0). Slots 1..3 are the per-material lanes the engine already sampled - a material record carries
  the slot in its flags bits 10..11 and the shaders pick `custom_env_cube + slot - 1` out of the
  heap - so this closes the last gap: before it, nothing could load an environment into those
  slots, and every material had to sample the scene-wide one. A custom slot uploads only the
  environment pair (slot 0's BRDF LUT is shared), an out-of-range slot is `invalid_argument`, and a
  decode failure keeps that slot's previous environment.

