# Gaussian viewer integration plan (2026-10-11)

> For agentic workers: execute this plan on master, task by task, with RED/GREEN tests. The raster batch that
> precedes it is complete (a5ff51b0..b4760); this batch is the one its review named as next.

Goal: a Gaussian PLY asset reaches the SCREEN through the normal frame chain - the engine's own camera drives
it, instance transforms work, TAA policy respects it, failure paths leave the previous scene intact, and the
numbers the design spec asks for are reported. No offscreen probe, no test-only path.

Architecture (fixed by the design spec, sections 6-7 and 9): the pass records between the opaque/lighting
stage and the transparent mesh pass, LOADs the existing HDR target and the opaque depth, disables culling and
depth writes, keeps the depth test on, and returns depth to `shader_read`. Per-frame uploads are immutable
snapshots retained until their frame slot completes. While Gaussians render, TAA is off and its history is
cleared; leaving the feature re-initializes that history.

Verified integration points (read from the tree at 2e9b6004 + the two commits after it):

- The chain is assembled by `deren.engine.render_start_demo`: passes are emplaced in
  source/engine/render_start_demo/render_start_demo.cpp (create order), looked up by declaration name, and the
  chain is handed to the runtime with `set_pass_chain` (line 115). The per-stage switch in `prepare` is where a
  pass is fed, and `feature_active` is the run gate.
- The pass exists and is callable: `pass::gaussian_splatting_pass` with `set_frame(shared_ptr<raster_frame const>)`,
  `record(resolved_io)`, `last_error()`, and the declaration name "gaussian"
  (source/engine/pass/gaussian_splatting.{cppm,cpp}).
- Asset and frame builders: `make_raster_asset(rhi::api_core&, deren::gaussian::asset, foundation_limits const&)`
  and `prepare_raster_frame(api_core&, std::span<raster_instance const>, raster_camera const&, limits)`
  (source/engine/gaussian_splatting/raster.gpu.cpp), with `raster_camera{view_projection, position, extent}`.
- The loader: `deren::gaussian::load_ply(path, load_options)` -> `expected<asset, diagnostic>`
  (source/gaussian_loader/gaussian_loader.cppm).
- The runtime owns the camera (`make_orbit_camera_ubo(yaw, pitch, distance, target, scene_extent_radius, aspect)`,
  runtime.frames.cppm), the frame ring (`frame_ring().position()`), the render extent, and the completion points
  the retention rule needs.
- The runtime's frame loop is where the stage sequence lives (runtime.frames.cppm around the
  `record_transparent_pass()` call), so putting the pass in the right PLACE is a runtime change, not a demo one.

## Global constraints

- No backend types, no native escape, no Gaussian-specific RHI types, no ABI change.
- The push block stays 112 bytes with the three heap index lanes the runtime appends (see the raster batch).
- Targets: HDR LOAD/STORE, depth LOAD/STORE and returned to shader_read; culling and depth writes off, depth
  test on; unpremultiplied alpha.
- Zero visible references skips the pass entirely. A failed load, sort or upload keeps the previous frame and
  logs why; no half-frame is published.
- Snapshots are retained by completion slot, never by swapchain image index; no `wait_idle` on the normal path.
- The 14 frozen capture scenarios must stay byte-identical when no Gaussian asset is configured.

## Task 0: a deterministic viewer asset, and a place to put one

Files: a small generator under source/scripts/ (or the existing probe helper), plus a documented default path.

- [ ] Produce a small, deterministic PLY (a few thousand splats in a recognisable shape - a colour-graded
      cluster the eye can confirm is ours) so the viewer path can be verified without a download. Record the
      expected particle count and byte size.
- [ ] Decide the config spelling (`[render] gaussian = "<path>"`, empty = off) and the auto-locate behaviour,
      next to `model`/`background_glb`.

## Task 8: recycle the snapshot's buffers per frame slot (NVIDIA's consume pattern)

WHY: probe B put the resize corruption in the splat STAGE's own runtime work, and NVIDIA's own sorter does the
opposite of this viewer in three places (docs/gaussian_splatting.md records the comparison): it re-sorts only when the
viewpoint changed, it hands its result over by SWAP into a caller-owned vector with the buffers as members, and it has
an explicit wait-for-in-flight contract. This task is the first of those to act on.

WHERE THE ALLOCATION IS: `raster.gpu.cpp:215-237`, inside `prepare_raster_frame` - two `core.create_buffer` calls per
frame (`storage_coherent`, `storage | device_address`, `initial_bytes` from the freshly built vectors) and then the
push block's two device addresses. At the acceptance cloud that is 4.5 MB created, uploaded and released per frame,
which is both the performance cost measured earlier and the per-frame creation a swapchain rebuild may trip over.

THE CHANGE:

1. `raster_frame` keeps its two buffers (it already does) plus their CAPACITIES, and gains a member
   `std::expected<void, foundation_error> refill(std::span<raster_instance const>, raster_camera const&, foundation_limits const&)`
   - a member, so it needs no new friend and can touch its own privates.
2. The body of `prepare_raster_frame` from the camera check through `result->push.visible_count` (lines 39-194) is
   EXTRACTED into a shared helper that fills a frame's contents (instances, sorted references, counters, push, and a
   failure result), so `prepare_raster_frame` and `refill` cannot drift.
3. `refill` then: computes the required bytes; if they fit the existing buffers, WRITES INTO THE MAPPED MEMORY and
   keeps the addresses unchanged; only if they exceed the capacity does it re-create that one buffer (and recompute
   that address). The addresses therefore stay valid across frames, which is the whole point.
4. The runtime already keeps one snapshot per frame slot (`gaussian_frames`); `make_gaussian_frame` calls `refill` on
   that slot's frame instead of `prepare_raster_frame` building a new one, and only creates on the first use of the
   slot (or when a growth forces it).

TO ANSWER FIRST, because the recipe stops here without it: how a `storage_coherent` buffer is WRITTEN after creation -
the contract's buffer interface may expose a mapped pointer, a `write`, or nothing but the upload-at-create path. Grep
`source/promise/rhi` for the buffer verbs before starting; if the contract has no write verb, the honest alternative is
a per-slot buffer created once with `initial_bytes` sized to the frame's own budget (`limits.max_packed_bytes`) and
never grown - the sizes are then stable by construction.

ANSWERED: the contract has exactly the verb needed. `rhi.api_core.cppm:822` declares
`[[nodiscard]] virtual std::span<std::byte> mapped() noexcept = 0;`, whose own header comment says a host-visible
buffer's bytes are handed to the caller that way ("buffer::mapped() hands the caller the bytes of a host-visible
buffer", and line 1570: upload at creation and a later write are two moments, the second being `mapped()` once the
frame lands). `buffer_usage::storage_coherent` is defined as "a storage buffer the host also writes (e.g. a table the
GPU reads)" - which is what these two buffers already are - so `refill` writes into `mapped()` and the
create-once-at-max-bytes fallback is not needed. The one rule to respect is the one the same comment states: read or
overwrite only after the frame that wrote it has completed, which is what the per-slot ring guarantees.

ACCEPTANCE, both measurable:
- [ ] the per-frame `create_buffer` lines disappear from a normal run (they are 2 per frame today);
- [ ] the visual protocol passes: three scripted resizes, every capture LOOKED at, no garbage;
- [ ] the cost line still reports the same visible counts, and the gates stay green.

## Task 7: the placeholder goes through import_scene (fixes the resize corruption)

WHY: measured in docs/gaussian_splatting.md's known issue. A leaf created by `runtime::make_primitive` loses its
scene data when the swapchain is rebuilt (first resize: the frame comes out as uninitialised memory - 4 of 4 runs),
while `import_scene`'s leaves survive it (the helmet controls are clean). So the placeholder triangle is wrapped as a
hand-built glTF scene and imported, which is the route the controls prove is generation-correct.

EVERYTHING IT NEEDS, read off the loader so it does not have to be rediscovered:

* `deren::gltf::vertex_portion { std::vector<uint8_t> data; component_type component; }` (gltf_loader.cppm:238).
* `deren::gltf::component_type` (gltf_loader.cppm:46): `float_t = 5126`, `unsigned_short_t = 5123`.
* `deren::gltf::primitive` (346): `std::map<std::string, vertex_portion> vertex;` (the ATTRIBUTE names the loader
  assembles into the engine's 64-byte interleaved vertex), `std::vector<uint8_t> index;`,
  `component_type index_component_type;`, `uint32_t material_index`.
* `deren::gltf::mesh` (363): `std::vector<primitive> primitives;`.
* `deren::gltf::node` (602): `name`, `local_transform`, `std::vector<mesh> meshes`, `transform_matrix`,
  `source_index`, and the TRS base pose (`translation`/`rotation`/`scale`, identity by default).
* `deren::gltf::scene` (632): `std::string name; std::vector<node> nodes; std::vector<std::size_t> root_indices;`.
* `deren::gltf::scenes`: `std::vector<material> materials;` plus `std::vector<scene> scene;`, and the iterators
  `nodes_begin()` / `nodes_end()`.
* `deren::gltf::resolve_materials(scenes)` (1105) produces the `std::vector<resolved_material>` that
  `drawable_iterator(scenes, materials)` (1124) takes; `get_vertex()`/`get_index()` call the iterator's
  `ensure_built()`, which is where the portions become the engine layout - so the portions are what to fill, not the
  interleaved bytes.
* The call is the one the app already makes for a model:
  `runtime.import_scene(scenes.nodes_begin(), scenes.nodes_end(), drawable_iterator(scenes, materials), drawable_iterator{}, glm::vec3(0.0f))`.

STEPS: (1) build the three attribute portions (POSITION 12 B, NORMAL 12 B, TEXCOORD_0 8 B per vertex) and the
uint16 index block for the existing triangle's vertices; (2) assemble scenes -> scene -> node -> mesh -> primitive
with `material_index = 0`; (3) one `deren::gltf::material{}` (the default, white opaque: the leaf is seven pixels
BEHIND the bouquet at the screen centre, so its colour is not observable); (4) `resolve_materials`, then
`import_scene`; (5) add `import deren.gltf_loader;` to chores.cpp after its module declaration and make the chores
target depend on gltf_loader in CMakeLists if it does not already; (6) re-run the scripted resize (the sky-region
difference must fall from 64+ to 0-2) and re-run the gates.

- [ ] Portions + primitive + node + scene built in `deren::chores::add_placeholder_triangle`
- [ ] `chores` imports/links gltf_loader
- [ ] The scripted resize measures 0-2 in the sky region (was 64.5)
- [ ] Gates: build, CTest, format, backend boundary

## Task 1: the model path decides the loader, and the asset lives

Files: source/application_configuration/*, source/app/chores.cpp (model selection + the file dialog's
filter), source/app/main.cpp (the load site), and headless tests.

NO NEW CONFIGURATION KEY: `model` (and argv[1]) already names a file, so the file's own extension chooses the
loader and there is no second key that can drift out of sync with the path.

- [x] Classify the path in `deren::app_config` (pure CPU, tested by test_app_config): `.gltf`/`.glb` -> the
      scene importer, `.ply` -> the Gaussian cloud loader, anything else -> `unsupported` rather than a silent
      default. Case-insensitive, extension only, nothing opened. Lands in
      `model_format_of`/`model_format` with ten cases in test_app_config.
- [ ] The app's two sites follow it: the file dialog's filter (`*.glb;*.gltf` today) gains `*.ply` and its
      title stops promising glTF only; `main.cpp`'s `gltf::load_model_async(model_path)` becomes a dispatch on
      `model_format_of`, and `unsupported` panics naming the two formats this build reads.
- [ ] The `.ply` branch loads the cloud once (`deren::gaussian::load_ply`), builds the `raster_asset`
      (`make_raster_asset`), and logs the totals the design asks for (particles, GPU bytes, bounds) - the
      spec requires the UI to report the real total, and 562,974 splats is not a number to guess at.
- [ ] A load failure keeps the previous scene and says why; nothing is published half-built. The glTF branch
      is untouched: a `.gltf`/`.glb` path must behave exactly as before.
- [ ] RED: a loader test that a malformed PLY yields a diagnostic rather than a partial asset (the dialect
      cases already exist; this adds the failure shape), plus a test that an unsupported extension is refused
      by name.

## Task 2: the frame-chain stage

Files: source/engine/runtime/runtime.frames.cppm (+ declarations if the stage list is declared there),
source/engine/render_start_demo/*.

- [ ] Add a "gaussian" stage between the lighting stage (`deferred`) and the transparent pass, gated by
      `feature_active("gaussian")`, so the frame loop records it in the order the spec fixes.
- [ ] The demo passes the stage its frame: build the `raster_camera` from the runtime's camera
      (view-projection, world position, RENDER extent - not the presentation extent) and call
      `prepare_raster_frame`, then `set_frame`.
- [ ] RED: a headless test that the stage order is what the spec says (the gaussian pass must not record before
      the lighting stage or after the transparent pass) and that a frame with no configured asset records
      nothing at all.
- [ ] The zero-visible-references case skips the pass entirely (no draw, no barrier).

## Task 3: retention across frame slots

- [ ] Keep the submitted snapshot alive until the frame slot that recorded it completes, reusing whatever
      completion mechanism the runtime already exposes (not the swapchain image index).
- [ ] RED: unloading the asset while a snapshot is in flight keeps the buffers alive; the frame after completion
      may reuse them.

## Task 4: TAA policy

- [ ] While the feature is active, force TAA off and clear `history_valid`; on leaving the feature, re-initialize
      the history rather than resuming from a stale one.
- [ ] RED: a frame with Gaussians must not run the TAA resolve, and the first frame after it stops must not
      blend against pre-Gaussian history.

## Task 5: the numbers

- [ ] Report total/visible/near-omitted particles, resolution, CPU sort time, upload bytes and GPU draw
      (extend the existing frame stats/overlay rather than inventing a second channel).

## Task 6: verification, review, delivery

- [ ] Run the generated asset on the real device: a screenshot that shows the splats, plus a second frame with a
      moved camera showing they track it. Record particle counts and frame cost.
- [ ] The 14 frozen scenarios stay matched with no asset configured.
- [ ] Full gates (build, ctest, format, backend boundary, strict clang-tidy - remembering that this machine's
      clang-tidy is LLVM 22 while CI is LLVM 23, so the CI job is the oracle for the check set).
- [ ] Independent review of this batch; address findings with RED/GREEN; push and confirm both CI jobs; record
      scope, limits and what remains (GPU sorting, unified transparency, real-asset acceptance with
      Bouquet of Flowers).

## Task 9: put the frame's upload INSIDE the frame (NVIDIA's tryConsumeAndUploadCpuSortingResult)

WHY, from the comparison: NVIDIA keeps ONE long-lived device buffer per semantic role and delivers each frame's
content through an upload RECORDED IN THE FRAME (splat_set_manager_vk.h: m_splatSortingIndicesDevice plus
tryConsumeAndUploadCpuSortingResult(cmd, ...), with updateSortingBuffers() creating only on a mode/size change). This
viewer writes the device-visible buffers from the CPU - create_buffer(initial_bytes) before Task 8, mapped() since -
at a moment with NO ordering relationship to any frame's execution. Every measurement collected here fits that shape:
the corruption needs the stage's own runtime work, survives removing the draw and the ensure calls, is intermittent,
and was unmoved by per-frame creation, per-slot retention, in-place refill or a generation reset - none of which
changes WHERE the write sits.

THE VERB EXISTS, and it is on the recording face:
  rhi.api_core.cppm:1691  [[nodiscard]] virtual error copy_buffer(buffer& destination, buffer const& source,
                                                                  std::uint64_t size, std::uint64_t source_offset ...) = 0;
declared with copy_image in the command_buffer verb list, so it records into the frame's own command buffer.

THE ONE BLOCKER, named rather than discovered halfway: WHICH INTENT A COPY DESTINATION NEEDS. The contract's own note
says storage_gpu_only is allocate-only - "its Vk usage carries no TRANSFER_DST and create_buffer refuses initial
contents" - so no intent in the enum today gives a device-local buffer that a frame copy may write; using
storage_coherent (host-visible) as the destination would make the copy pointless. So this task starts with that
question: either a new buffer intent (a contract change, which is the honest home for it) or an existing route this
analysis has not found.

STEPS, once the intent question is answered:
1. per slot: a host staging buffer (CPU-written, no device address) plus the long-lived device buffers the pass binds;
2. prepare_raster_frame / refill writes the STAGING buffer and returns the frame describing the copy;
3. the splat pass records copy_buffer(device, staging, bytes) in its own record(), next to the barriers it already
   records - which is the frame-ordered upload;
4. the acceptance is unchanged and measurable: the visual protocol (three scripted resizes, every capture LOOKED at)
   plus the cost line and the gates.
