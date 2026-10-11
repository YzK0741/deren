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
