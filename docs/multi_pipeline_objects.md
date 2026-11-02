# Multi-pipeline objects: Phase A and Phase B implementation

Status: Phase A and Phase B are integrated on `master` (PR #28); the full
local gate passed before the squash merge. Phase C is not started.

This document records the per-object rendering-path design and the Phase A/B
implementation. The three pre-Phase-A gates were answered: raise the character
cap to 16, land Phase A first, and keep the per-object asset work as the base.
Phase B (multiple Gaussian objects) was approved separately by the user after
Phase A. Phase C still needs its own design and review.

## 0. Base state

The committed base already contains the per-object asset feature:

- `scene_object_settings` accepts `model` or `asset` per object.
- Distinct glTF files are deduplicated, loaded into an asset pool, grounded on a
  common floor, and used for per-object materials, bounds, camera fit, shadows,
  and animation controllers.
- Before Phase A, the feature was PBR-only when assets were mixed; Phase A
  removes that restriction for `game_toon` objects.

The committed implementation keeps that work as the base and overlays pipeline
routing on top of it. Phase A and Phase B were reviewed and merged together.

## 1. Goal

Allow each scene object to select its own rendering path in one frame:

- deferred PBR,
- deferred unlit,
- GameToon character,
- Gaussian splatting.

The design must support multiple objects of each class in the same scene:
multiple GameToon characters and multiple Gaussian clouds, plus PBR and unlit
objects.

## 2. Non-goals for v1

- No per-object forward/deferred switch. The renderer keeps the existing global
  hybrid order.
- No new render graph. The existing passes and their order stay intact.
- No per-character shadow map or per-character outline pass. The existing scene
  shadow map and character outline group are reused.
- No attempt to port Unreal Engine's Substrate or its per-pixel shading-model
  dispatch. The reference is used for ownership and routing, not for copying a
  different renderer.
- No Gaussian lighting, shadows, or reflections. Splats remain a separate
  compositing stage with the existing limitations.
- No transparent mesh sorting changes.

## 3. References and what they establish

### 3.1 Unreal Engine 5.8.2

Source checkout: `D:\UnrealEngine-5.8.2-release`.

- `Engine/Source/Runtime/Engine/Classes/Engine/EngineTypes.h:707` defines
  `EMaterialShadingModel` (`MSM_Unlit`, `MSM_DefaultLit`, `MSM_Eye`, ...).
- `Engine/Source/Runtime/Renderer/Private/BasePassRendering.cpp:1932`
  (`FBasePassMeshProcessor::AddMeshBatch`) and its `TryAddMeshBatch` path use the
  material's shading models to decide which pass the mesh enters.
- `Engine/Source/Runtime/Renderer/Public/MeshPassProcessor.h` defines the
  mesh-pass processor abstraction; each pass owns its pipeline state.
- `Engine/Shaders/Private/ShadingModels.ush:1139` (`IntegrateBxDF`) dispatches
  on the G-buffer `ShadingModelID`, including `SHADINGMODELID_DEFAULT_LIT` and
  the Substrate toon model.
- `Engine/Source/Runtime/Renderer/Private/DeferredShadingRenderer.cpp:495`
  selects forward versus deferred globally with `IsForwardShadingEnabled`, not
  per object.

Conclusion: a material declares how it is shaded; a pass owns the pipeline; the
G-buffer carries the per-pixel shading identity. A per-object draw cannot bind
an arbitrary pipeline inside the G-buffer.

### 3.2 Cel-shading production practice

The miHoYo technical talk on Honkai Impact 3rd describes:

- multi-channel ramp textures for layered cel shading,
- a character-specific shadow map with a tight frustum and PCSS filtering,
- back-face outlines plus geometry-shader edges for hard-surface creases,
- eye and hair materials with their own shading models,
- a shared character stage rather than one full render pass per character.

Reference:
[miHoYo technical director talk](https://www.gameres.com/807189.html).

Conclusion: multiple characters share one character stage. Per-character data
(materials, head frame, shadow fit) is data, not a new pass per character.

### 3.3 Gaussian splatting with multiple objects

Open3D's 3DGS design describes:

- per-object Gaussian geometry buffers,
- one merged GPU buffer for projection, sorting, and compositing,
- a shared mesh depth texture so the composite can reject splats behind meshes,
- multiple Gaussian scenes in one view.

Reference:
[Open3D 3D Gaussian Splatting rendering design](https://www.open3d.org/docs/latest/cpp_api/md__2home_2runner_2work_2_open3_d_2open3_d_2cpp_2open3d_2visualization_2rendering_2gaussian__splat_2_gaussian_splat_design.html).

The same shape already exists in deren:

- `source/engine/gaussian_splatting/raster.cppm:51` defines
  `raster_instance { asset, model, stable_id }`.
- `source/engine/gaussian_splatting/raster.gpu.cpp:143` calls
  `sort_draw_references` over the whole instance span, so sorting is global
  across instances.
- `source/engine/runtime/runtime.frames.cppm:2452` and `:2505` already pass the
  whole `gaussian_instances` vector to `prepare_raster_frame` and
  `prepare_gpu_frame`.

Conclusion: multiple clouds need one snapshot containing multiple instances,
not multiple passes. The only missing piece is a runtime setter that can add
more than one instance.

## 4. Why not a per-object pipeline binding

The current G-buffer pass draws every opaque leaf through the one G-buffer
pipeline. A leaf that asks for a different pipeline is refused:

```text
runtime: leaf requests pipeline '{}' during the G-buffer pass - draw skipped
(only default-semantics leaves write the G-buffer)
```

See `source/engine/runtime/runtime.frames.cppm:2280`.

Per-object pipeline binding in the G-buffer would require:

- one pipeline per leaf in the scene pass,
- per-leaf descriptor sets for every pipeline family,
- a way to keep the deferred lighting pass from shading toon pixels twice,
- a new depth strategy for leaves that do not write the G-buffer.

That is a render-graph rewrite. The existing hybrid path already solves the
visual problem: all opaque meshes write G-buffer depth, deferred lighting runs,
and the character pass re-shades its subset at depth-equal. The design below
extends that path instead.

## 5. Architecture

Objects declare a shading class. The owning stage chooses the pipeline.

```text
Object pipeline     shading_path     Stage
---------------     ------------     ---------------------------------
pbr                 deferred         G-buffer + deferred lighting
unlit               deferred         G-buffer + deferred (material bit9 unlit)
game_toon           character        G-buffer depth + character_forward
gaussian            splat            Gaussian snapshot (no mesh leaf)
```

### 5.1 Primitive

Add to `source/engine/primitive/primitive.cppm`:

```cpp
enum class shading_path : std::uint8_t {
    deferred,
    character,
    splat,
};

struct primitive {
    // ...
    shading_path path = shading_path::deferred;
};
```

The app sets `path` at import time from the object's resolved pipeline. The
runtime never changes it during a frame.

A `splat` object produces no mesh leaf; its asset is consumed by the Gaussian
stage instead.

### 5.2 Scene and deferred passes

- Every opaque mesh leaf, including `character` leaves, continues to write the
  G-buffer and depth. This preserves depth, shadows, picking, and outline
  occlusion with no new pass.
- Deferred lighting continues to shade all G-buffer pixels. A character pixel is
  shaded by PBR first and then overwritten by the character pass. This is
  intentional v1 waste, not a correctness requirement.
- `splat` leaves are absent from the G-buffer, so they cannot create depth
  holes.

A later optimization can write a G-buffer material flag that makes deferred skip
character pixels. That optimization is not part of v1.

### 5.3 Character stage

`source/engine/runtime/runtime.frames.cppm:808` currently builds
`frame_character` from every opaque, non-environment leaf. Change it to:

```cpp
if (!m->transparent && !m->environment && m->path == shading_path::character) {
    frame_character.push_back(m);
}
```

The outline collection around `runtime.frames.cppm:2591` must apply the same
filter: only character leaves whose material states a non-zero outline width
enter `frame_outline`.

`make_character_forward_frame()` at
`source/engine/runtime/runtime.frames.cppm:2537` still selects between
`game_toon` and `character_forward`. It now sees only character leaves.

`source/engine/pass/character_forward.cpp:130` still sets
`env.default_name` once for the whole group. That is correct because all
character objects in v1 use the same toon pipeline. The per-object variation is
material data and head index, not pipeline state.

If no character object exists, the character pass has no leaves and records
nothing.

### 5.4 Multiple GameToon characters

The cap is raised to the scene-object limit:

```cpp
// source/engine/primitive/primitive.cppm:270
export constexpr std::uint32_t max_game_toon_characters = 16;
```

v1 supports up to sixteen character objects in one frame, matching
`max_scene_objects`, each with its own head slot and its own material lanes. The
shader's character index is 8 bits wide, so the only storage that grows is the
per-frame head array: 16 * 64 = 1024 bytes per frame slot. A scene with more
GameToon objects than the cap is a startup error with a clear message; it must
not silently share a head frame.

Every scene object gets a slot at import, not only the objects that start as
GameToon, so the object panel can switch `pipeline` live without making an
object read another character's head frame.

A per-object `pipeline = "game_toon"` is itself the opt-in for the character
stage; the global `[render] character_forward` switch is not a prerequisite. The
captured tone curve and hair-rim lane follow the effective GameToon selection
while that stage is active. Each object publishes its head basis from its own
asset rig and placement, so rotation and scale move the face frame with the
rendered character.

### 5.5 Per-asset toon sidecar

Before Phase A, the app loaded one sidecar for the default model and dropped it
when multiple assets were present. Phase A implements the per-asset form below:

- each pool entry owns `std::optional<deren::toon::sidecar>`.
- each pool entry owns its `toon_lookup_state` and is installed as the active
  lookup immediately before that asset is imported.

Per-object toon requires the sidecar to belong to the asset that carries the
toon materials:

- Extend `scene_asset_entry` with
  `std::optional<deren::toon::sidecar> sidecar`.
- For each asset used by at least one `game_toon` object, load its sidecar by
  the asset's path.
- Keep a per-asset `toon_lookup_state` and route material lane lookups through
  the state of the asset being imported.
- A `game_toon` object whose asset has no usable sidecar fails at startup. It
  must not fall back to the default asset's sidecar or to a PBR look.

If v1 is limited to multiple instances of one toon asset, this can be simplified
to one sidecar shared by those instances. The design keeps the per-asset form
because the goal is multiple character types, not only multiple copies.

### 5.6 Gaussian objects

Phase B replaces "set the cloud" with "manage the cloud list":

```cpp
void clear_gaussian_assets() noexcept;

void add_gaussian_asset(
    std::shared_ptr<gaussian_splatting::raster_asset const> asset,
    glm::mat4 const& model,
    std::uint64_t stable_id) noexcept;
```

Rules:

- `gaussian_instances` is the source of truth.
- `gaussian_active()` becomes `!gaussian_instances.empty()`.
- The old `set_gaussian_asset(asset, model)` remains as
  `clear_gaussian_assets()` followed by `add_gaussian_asset(asset, model, 0)`
  for compatibility with the existing single-PLY scene.
- `make_gaussian_frame` and `make_gaussian_gpu_frame` keep passing the whole
  span to `prepare_raster_frame`; the global sort at `raster.gpu.cpp:143`
  already orders all clouds together.
- Each object gets a distinct `stable_id` so sorting and diagnostics can
  distinguish clouds.

App side (implemented in Phase B):

- `scene_asset_entry` has an optional `cloud` plus `cloud_placement`. A `.ply`
  object loads through `deren::gaussian::load_ply`; the cloud is uploaded with
  `runtime.create_gaussian_asset` and stored in its own asset slot, not in a
  glTF scene.
- World bounds come from `raster_asset::source().bounds_min/bounds_max` and the
  object transform; they drive camera fit, shadow exclusion, and AABB picking.
- Multiple clouds share one Gaussian pass and one snapshot. `main.cpp` calls
  `clear_gaussian_assets()` once and then `add_gaussian_asset()` for every
  visible cloud, using the object transform, the asset's grounding shift and
  the cloud's -Y alignment. The global sort orders all clouds together.
- TAA is disabled while any cloud is staged, regardless of the user toggle.
- A `.ply` object is legal only with `pipeline = "gaussian"`; a `.gltf/.glb`
  object cannot use that pipeline. For a cloud the object panel offers only
  `gaussian`, so it cannot be switched to a mesh material.

## 6. Configuration and UI

### 6.1 Config

Object `pipeline` values:

```text
default | pbr | unlit | game_toon | gaussian
```

Validation rules:

- `pbr`, `unlit` and `default` stay on the deferred path. `default` means the
  pass default (ordinary PBR), not the character stage.
- `game_toon` requires a glTF/GLB asset with a usable toon sidecar. A `game_toon`
  object whose asset has no sidecar is a startup error, so it cannot silently
  shade with another asset's material lanes.
- `gaussian` requires a `.ply` cloud; any other pipeline requires a
  `.gltf/.glb` mesh. The mismatch is a startup error.
- The existing `[render] game_toon` switch still selects the legacy GameToon
  chain. A scene with any `game_toon` object enables the character stage even
  when that global switch is off.

### 6.2 Object panel

- The selected-object header shows the resolved stage: deferred lit, deferred
  unlit, game_toon with its head slot, or gaussian cloud with the splat stage.
- The pipeline combo is filtered by the selected object's asset: a mesh offers
  `default`, `pbr` and `unlit`, plus `game_toon` only when that asset has a
  sidecar; a cloud offers only `gaussian`. Switching applies between frames and
  updates the object's shading path.
- Gaussian objects are selectable by their cloud bounds and expose transform,
  visibility, cast-shadow and pipeline controls. The mesh-only material override
  and environment controls do not apply to a cloud.
- Keep the existing per-object transform, visibility, cast-shadow, animation,
  and environment controls.

Animation remains asset-scoped: each object's controller is initialized from
its own scenes and exposes that asset's clip list.

## 7. Gesture and picking

- Mesh objects continue to use their world AABB for picking.
- Gaussian objects use the transformed cloud bounds from the loaded asset.
- An object with `pipeline = gaussian` is selectable by its bounds even though
  it has no mesh leaf.
- Visibility and cast-shadow rules remain object-level; Gaussian objects do not
  cast mesh shadows in v1.

## 8. Phases

### Phase A: routing and character objects (implemented)

- [x] Add `shading_path`.
- [x] Filter `frame_character` and `frame_outline` on
  `shading_path::character`.
- [x] Add `game_toon` to the object pipeline enum, with validation.
- [x] Raise `max_game_toon_characters` to 16 and assign every object a head slot.
- [x] Load sidecars per asset and route material lane lookups through the state
  of the asset being imported.
- [x] Verify PBR + unlit + two GameToon characters in one capture.
- [x] Verify that a GameToon object works while the global `[render] game_toon`
  switch is off.
- [x] Complete the CI-equivalent gate before commit (passed; merged as PR #28).

### Phase B: Gaussian objects (implemented)

- [x] Add the multi-instance runtime API (`clear_gaussian_assets`,
  `add_gaussian_asset`; `set_gaussian_asset` remains a compatibility wrapper).
- [x] Add the Gaussian asset variant to the asset pool.
- [x] Add bounds, picking, and object panel support.
- [x] Verify two clouds in one capture and a mesh plus a cloud in another,
  including the shared splat stage, depth ordering, and automatic TAA disable.

### Phase C: mixed scene and hardening (not started)

- Verify PBR + unlit + GameToon + Gaussian in one frame.
- Add config tests for mismatches and the sixteen-character cap.
- Add a capture with outline and shadow checks across multiple characters.
- Update `config.example.toml` and user-facing docs.
- Run the complete local gate.

## 9. Validation

Phase A captures already taken:

1. A clean-HEAD build of the same asset produced a byte-identical
   single-character frame, so the routing change does not alter the existing
   GameToon picture.
2. Two GameToon scene objects rendered with distinct head slots, with and
   without the global `[render] game_toon` switch.
3. One GameToon object plus one PBR object rendered in the same frame; the
   helmet did not panic for a missing sidecar.

Phase B captures already taken:

1. Two independent Gaussian objects at distinct positions rendered through one
   splat stage (`tmp/cap-gauss-two/screenshot_20261102_220247.png`).
2. A PBR mesh and a Gaussian cloud rendered in one frame
   (`tmp/cap-gauss-mixed/screenshot_20261102_220313.png`); TAA was requested on
   and the app disabled it while the cloud was staged.
3. A valid single-cloud run also succeeded after the multi-cloud change
   (`tmp/cap-gauss-two/screenshot_20261102_220349.png`).
4. Two distinct cloud assets, rather than two instances of one asset, rendered
   together (`tmp/cap-gauss-distinct/screenshot_20261102_220935.png`).
5. A mesh in front of a cloud confirmed depth ordering
   (`tmp/cap-gauss-occlusion/screenshot_20261102_220908.png`).
6. A scene with no top-level `model`, where every object names its own cloud,
   rendered without loading a default model
   (`tmp/cap-gauss-no-top/screenshot_20261102_221137.png`).

Still required before a commit:

1. PBR + unlit regression; existing pixels should be unchanged.
2. A capture with outline and shadow checks across multiple characters.
3. Phase C captures once that phase exists.

Required automated checks:

- `test_app_config` for parsing, inheritance, mismatches, and the character cap.
- `test_shader_sources` if new shader entry points or pipeline names are added.
- Full `ctest` for the configured build.
- The complete local CI-equivalent gate before any commit or push.

## 10. Risks and open questions

- **Sixteen-character cap.** `max_game_toon_characters` is 16, matching
  `max_scene_objects`. Each character adds 64 bytes to the per-frame head array;
  the shader index is 8 bits wide. A larger scene limit would require growing
  both the cap and the character-index width together.
- **Double shading cost.** Character pixels are shaded by deferred and then
  overwritten. This is accepted for v1; it is measurable and removable later.
- **Sidecar ownership.** The lookup is now per asset. A `game_toon` object whose
  asset has no sidecar is a startup error; there is no fallback to another
  asset's material table.
- **Gaussian ordering.** The global sort handles multiple clouds, but splats do
  not interleave correctly with transparent meshes; v1 keeps the existing
  limitation.
- **Gaussian shadows.** Splats cast no mesh shadows in v1. Do not add a
  per-object shadow path without a separate design.
- **Asset type in the pool.** `scene_asset_entry` now has an optional cloud and
  no longer assumes every asset is a glTF scene. Consumers must check
  `is_gaussian()` before touching `scenes`, `sidecar`, or materials; the type
  remains a pair of optional members rather than a discriminated union.
- **Deferred writes for character pixels.** If a future optimization skips
  deferred shading for character pixels, the character pass must guarantee full
  coverage; holes would expose undefined G-buffer color.

## 11. File impact

Phase A and Phase B modified:

- `source/engine/primitive/primitive.cppm`
- `source/engine/runtime/runtime.declarations.cppm`
- `source/engine/runtime/runtime.frames.cppm`
- `source/engine/runtime/runtime.cpp`
- `source/app/main.cpp`
- `source/app/app_config.cpp`
- `source/app/app_config.cppm`
- `source/app/app_ui.cpp`
- `source/configuration/configuration.cppm`
- `source/configuration/configuration.cpp`
- `source/tests/test_app_config.cpp`
- `source/tests/test_subtilis_frame.cpp`
- `source/tests/fixtures/config_scene_objects.toml`
- `config.example.toml`
- `Doxyfile`
- `docs/multi_pipeline_objects.md`

Unchanged:

- `source/engine/pass/character_forward.cpp`, because the pass-level pipeline
  selection is still one toon pipeline per character group.
- `source/engine/gaussian_splatting/raster.gpu.cpp`, because global multi-instance
  sorting already existed; Phase B only feeds it multiple instances.

## 12. Approval decisions

The three pre-implementation gates were answered:

1. **Character cap.** Raise it to 16, matching `max_scene_objects`.
2. **Scope.** Land Phase A first; Gaussian objects stay a separate round.
3. **Base.** Keep the per-object asset work and overlay Phase A on it.
4. **Phase B.** Approved by the user after Phase A; it added Gaussian objects
   and was merged with Phase A in PR #28.

Phase A and Phase B are complete; Phase C remains a separate design round.
