# Subtilis - hierarchical cluster LOD

> **Status (2026-10-22).** M1a/M1b and **M1c runtime import of offline cluster DAGs** are implemented.
> `DEREN_subtilis` assets now load through the normal glTF path, share immutable CPU/GPU generations,
> and feed the CPU cut into the existing visibility/HZB lanes. The explicit legacy builder remains usable.
> M2 (GPU select/finalize), M4 (software raster) and M5 (streaming) are not implemented.
> **Default OFF**: `[render] subtilis=true` opts into payload extraction/adoption at startup and requires
> `visibility_buffer=true` plus frame admission. Unsupported assets/items keep complete LOD0.
>
> The real converter-DAG Bistro matrix measures GPU -17.8%/-12.9% (far/near), parallel cut about 1.2ms,
> at the cost of about +1.25s import-ready and +139MiB process peak working set. These are local results,
> with inherited source-validator errors and slight near-image variation explicitly recorded in the
> [runtime acceptance report](reviews/2026-10-21-object-lod.md). The older M1b numbers below remain historical.
>
> Tool entry: [tools/subtilis/README.md](../tools/subtilis/README.md); independent converter evidence:
> [2026-10-21 converter report](reviews/2026-10-21-subtilis-converter.md). Dated plans/reports remain
> authoritative for their own steps and are indexed below.

## 1. What it is

Subtilis is the renderer's **static, opaque, fully-resident hierarchical cluster LOD** system, in the style
of virtualized geometry (the code never uses the "Nanite" trademark; documents say "Nanite-style" when the
comparison matters):

- the **offline converter** creates a shared-vertex **cluster DAG**, with unique producer/consumer links,
  fine/coarse reference lists and monotone error/bounds; coarse indices are appended after the complete
  original LOD0 prefix;
- group decisions are closed along dependencies before stable cluster IDs are scanned, preserving
  cross-group reuse instead of projecting it into a forest. The older explicit CPU builder still produces
  a forest (and a validated DAG wrapper) and retains its existing selector for compatibility;
- at run time a **deterministic CPU screen-error cut** picks, per visible item, either a coarse group run or
  the full LOD0 run, writes the selected cluster records into the existing per-frame host-culled meshlet
  table, and publishes the raster/resolve/indirect metadata from that one result;
- the **existing visibility-buffer lane** (`docs/visibility_buffer.md`) rasterizes and resolves the result
  unchanged - Subtilis replaces the *producer* of the visible meshlet set, not the resolve math;
- every unsupported case (no hierarchy, invalid generation, budget overflow, strategy restriction) falls
  back to the item's complete LOD0 run; an unadmitted visibility frame keeps the legacy geometry lane.

## 2. What runs in the frame today

| Step | What happens | Where |
| --- | --- | --- |
| Offline asset | shared-vertex cluster DAG, complete LOD0 prefix and appended coarse indices | `tools/subtilis`, `source/asset/subtilis/` |
| Loader/import | opt-in owned raw bytes; engine checks profile/source binding, structure and windows; CPU and GPU generations shared by source/payload identity | `gltf_loader`, `subtilis_dag_import.cppm`, `runtime::import_scene` |
| Validation | DAG factory checks producer/consumer reciprocity, references, coverage, bounds/errors and index windows; full geometry validation stays offline | `subtilis_dag_asset.cppm`; legacy forest: `subtilis_asset.cppm` |
| Upload | adopted payload or explicit `subtilis_geometry` becomes a shared generation: CPU hierarchy + two GPU buffers retained through the pass chain; normal draws/shadows/BLAS stay on LOD0 | `subtilis_upload.cppm`, `primitive_create_info::subtilis_geometry` |
| Per-frame cut | `subtilis::prepare_item` uses the current model, frozen view/jittered projection and render extent; picks a complete run, or LOD0 on any fallback | `runtime.cpp` (`record_visibility_items`), `subtilis_selection.cppm`, `subtilis_dag_lod.cppm` (legacy: `subtilis_lod.cppm`) |
| Publication | one run drives the visible record table, the resolve `draw_item` row, the raster push block and the indirect group count - no half-published metadata | `runtime.cpp`, `visibility.cppm` |
| Raster/resolve | the visibility lane consumes the run exactly as it consumed host-culled LOD0 records | `source/shaders/scene/visibility.slang`, `visibility_resolve.slang` |
| Occlusion (M3) | the HZB cull/recovery lanes treat the cut-selected records as their candidate set, so occlusion composes after LOD | `source/engine/pass/hzb_cull.*`, `hzb.slang`, `docs/reviews/2026-10-21-hzb-recovery.md` |

**Strategy guard-rails.** MASK/BLEND materials, non-standard attribute offsets, exact-depth redraws (toon
`character_forward`) and any unsupported view force the finest run; shadows and BLAS never consume the cut.
If an item's hierarchy is stale, its generation does not match the leaf, or publication fails, that item
reports a shared/LOD0 fallback - the frame never mixes two G-buffer producers.

## 3. Data model and hard limits

- **Meshlet record stays 48 bytes** (`first_index`, `index_count`, `base_vertex`, pad, center/radius,
  axis/cone) - Subtilis does not change the record or the raster ABI.
- **Packed visibility id is 32 bits, exactly filled**: `valid 1 / item 12 / meshlet 12 / triangle 7`
  (`visibility.cppm`), i.e. **4096 items per frame and 4096 selected meshlets per item**. The shared
  host-culled table keeps 65536 records; a cut must satisfy both its item's `[meshlet_base,
  meshlet_base + meshlet_count)` reservation and the 12-bit id.
- **Workgroup cap is 85 triangles per meshlet** (Vulkan mesh-shader 8-bit index / 256-vertex budget), so a
  group's coarse run is a run of ordinary meshlet records, not one big triangle list.
- **A legal DAG cut can exceed its leaf count**, despite reducing triangles. The asset remains valid;
  if the actual cut exceeds its LOD0 reservation, runtime uses the complete LOD0 run. The legacy forest
  factory keeps its stricter worst-cut precheck. Neither path truncates a selected run.
- **Errors are conservative heuristics**, not strict Hausdorff or per-pixel guarantees. Imported DAGs
  match the converter's `sqrt(norm1*normInfinity)` scale bound, projection and closest-sphere depth rule;
  unsupported/nonfinite views or near crossings prevent unsafe coarse selection. The legacy forest
  selector retains its older Frobenius bound.

## 4. Code map

| Concern | Files / modules |
| --- | --- |
| DAG asset/import + validation | `source/engine/geometry/subtilis_dag_asset.cppm`, `subtilis_dag_import.cppm` |
| Legacy forest validation | `subtilis_asset.cppm` |
| CPU cut reference | `subtilis_dag_lod.cppm`, legacy `subtilis_lod.cppm` (+ `subtilis.cppm` for the M1a LOD0 bridge) |
| Offline builder | `subtilis_builder.cppm`, `subtilis_builder.cpp`, `subtilis_builder_topology.cpp`, `subtilis_builder_simplify.cpp`, `subtilis_builder_internal.hpp` |
| Explicit upload / generations | `subtilis_upload.cppm`, `subtilis_upload.cpp` |
| Main-view selection + frame publication | `source/engine/runtime/runtime.cpp` (`record_visibility_items`), `subtilis_selection.cppm` |
| Consumer lane | `source/shaders/scene/visibility.slang`, `visibility_resolve.slang`, `source/engine/visibility/visibility.cppm` |
| Capacity / id contract | `source/engine/visibility/visibility.cppm`, `source/engine/render_layout/render_layout.cppm`, `source/shaders/common/heap_slot_constants.slang` |
| Tests | `test_subtilis`, `test_subtilis_asset`, `test_subtilis_dag_asset`, `test_subtilis_dag_lod`, `test_subtilis_import`, `test_subtilis_lod`, `test_subtilis_builder`, `test_subtilis_upload`, `test_subtilis_selection`, `test_subtilis_frame`; `test_visibility_buffer`, `test_render_graph_visibility`, `test_hzb` cover the consumer/Occlusion side |
| Builder dependency | meshoptimizer pinned **v1.3 / `9e1f07b159d3cb777f1c67ed31fc11fd117986f4`** (MIT), files SHA-256-verified, vendored privately |

## 5. Enabling and configuring

```toml
[render]
visibility_buffer = true   # the producer this LOD feeds
subtilis = true            # hierarchical cut for admitted static opaque items (default false)
subtilis_error_px = 1.0    # finite positive projected error; invalid values use 1.0
subtilis_hysteresis = false # imported DAGs: coarsen at 80%, refine above 100%
```

- Offline files use the `DEREN_subtilis` extension. Only static OPAQUE shared-vertex profiles are
  adopted; unsupported attributes (including TANGENT), malformed/source-mismatched data and capacity
  refusal retain ordinary LOD0. Full converter validation belongs in the trusted offline asset pipeline.
- The app uses one switch for loader extraction and runtime adoption. Custom callers can use
  `gltf::load_options{.extract_subtilis_payload=true}`. An OFF startup does not extract/decode payloads;
  toggling runtime ON later cannot create a hierarchy that was never imported.
- Runtime APIs: `set_subtilis(bool)`, `set_subtilis_error_px(float)`, `set_subtilis_hysteresis(bool)`.
  Explicit builder/upload via `primitive_create_info::subtilis_geometry` remains supported; no
  automatic build-at-load, disk cache or streaming is introduced.
- Any frame that fails visibility admission keeps the legacy opaque lane as a whole; Subtilis never
  creates a second G-buffer writer.
- With hysteresis disabled, the imported DAG cut matches the offline reference. Optional history is
  per primitive/generation; resize, a camera jump (the existing HZB motion limits), model changes and
  generation replacement invalidate it. It does not retain GPU generations. The per-item planning
  phase is parallel (thread pool), with an item-ordered
  overlap resolution so alias/shared items keep the serial semantics byte for byte.
- Off: nothing above runs and the frame is identical to the pre-Subtilis build.

## 6. Measured results (Bistro, RTX 4060 Laptop, 1280x720, Release)

**Full-scene geometry asset** (`bistro-geometry.gltf`: 6006 nodes / 2909 primitives / 4,208,625 draw
triangles; 4096-item build; validation off; 130-frame captures; same prebuilt hierarchy and upload for
OFF/ON):

| Metric (OFF -> ON) | far | near |
| --- | ---: | ---: |
| Input -> selected triangles | 4,208,625 -> 3,727,209 (**-11.4%**) | 3,626,626 -> 3,175,714 (**-12.4%**) |
| Selected meshlet records | 50,808 -> 45,493 (-10.5%) | 43,930 -> 38,864 (-11.5%) |
| GPU visibility raster (median) | 26.22 -> **23.89 ms (-8.9%)** | 23.00 -> **20.95 ms (-8.9%)** |
| GPU frame total (median) | 26.98 -> **24.63 ms (-8.7%, -2.35 ms)** | 24.03 -> **21.95 ms (-8.7%, -2.08 ms)** |
| End-to-end wall frame | 25.25 -> **23.18 ms (-8.2%)** | 22.67 -> **20.83 ms (-8.1%)** |
| CPU cut cost (serial build) | ~0.08 -> **+8.68 ms/frame** | ~0.07 -> **+8.69 ms/frame** |
| Selected coarse records / fallback items | 9,451 / 0 | 7,342 / 0 |
| Visible items (of 4,096) | 2,906 | 2,840 |
| Image vs OFF | 0.938% pixels changed, mean channel delta 0.020 | 0.098%, 0.0017 |

The cut cost was then **parallelized**: `post` phase 8.6 -> **about 2 ms** (far ON 13.93 -> 6.28/7.48 ms,
near 14.57 -> 6.28 ms) with byte-identical cuts and pixel-identical screenshots; the end-to-end frame does
not move because the frame is GPU-bound, but the CPU headroom is what M2/M3/streaming will spend.

**Capacity prerequisite** (not a Subtilis gain): the 1024-item build fell back to the legacy path at
178.5 ms GPU frame; the 4096-item build runs the meshlet lane at 26.7-27.0 ms (**about 6.6x**) and is what makes
Subtilis effective on a 2909-item scene at all (`visibility raster: recorded 2906 item(s)`).

**Occlusion composes after the cut (M3, 2026-10-21).** The exact HZB test rejects far 71.51% / near 92.83%
of the cut-selected candidates (100% agreement with the CPU reference), and the complete HZB lane measured
**-78.6% (near) / -61.6% (far)** GPU frame on moving views - that gain is occlusion, not LOD; Subtilis
supplies the candidate set and remains responsible for the LOD error, the HZB for conservative visibility.

**Textured acceptance boundary.** The 4096-item contract was verified on the original unmerged
hierarchy (2909 primitives -> 2906 visible items, no fallback) together with the 1024-slot texture array
(516 runtime slots) and the PBR path; see the textured report. **A Subtilis OFF/ON image comparison on the
textured scene is still open** (it needs the experiment-only build-at-import patch), and the original
geometry-scene image differences above are LOD edge differences, not pixel identity.

## 7. Tests and how to reproduce

- CPU/contract: `test_subtilis` (selection/run/fallback), `test_subtilis_asset` (tree/coverage/budget),
  `test_subtilis_lod` (cut reference, perspective/ortho/shear/near-plane), `test_subtilis_builder`
  (simplify/cluster/boundary), `test_subtilis_upload` (generation/bind), `test_subtilis_selection`
  (main-view prepare + frame run), `test_subtilis_frame` (published-run overlap/alias + frames);
  `test_visibility_buffer`, `test_render_graph_visibility` cover the consumer lane, `test_hzb` the
  occlusion composition. `ctest -R subtilis` covers the first group.
- Offline converter: `tools/subtilis` builds the `DEREN_subtilis` asset (build/inspect/validate/export-cut;
  `--threads N` deterministic, `validate --full`, per-stage report). Its own tests are built from
  `tools/subtilis/CMakeLists.txt` (`ctest --test-dir build-subtilis`), separately from the engine CTest set;
  measurement scripts live in `scripts/measure/` and the Task 8 acceptance record is
  `docs/reviews/2026-10-21-subtilis-converter.md`.
- Real-scene captures: `scripts/windows/capture.ps1 -Base <toml> -Camera ... -Frames ...` with
  `subtilis = true`; the full OFF/ON matrix and raw logs are in the Bistro performance report
  (`docs/reviews/2026-10-20-bistro-performance/`). The textured asset preparation/verification scripts are
  `scripts/measure/prepare_bistro_textures.py`, `convert_bistro_metalrough.py`, `verify_bistro_textured.py`.
- Assets stay outside the repository (`D:/deren-assets/bistro`); the review directories carry the captured
  evidence (webp/logs/json) that the numbers above come from.

## 8. Known limits and open items

1. **M2 (GPU cut selection/finalize) is not implemented** - selection is CPU-side; the cut is deterministic
   and parallel but still costs CPU and a visible table write per frame.
2. **Shared-vertex v1 only** - converter payload import is implemented; automatic build-at-load, disk
   caching and extra-attribute profiles are not. OFF startup cannot enable unavailable payloads later.
3. **Textured-scene Subtilis OFF/ON** is open; the textured acceptance covered capacity/PBR, not LOD.
4. **M4 software raster / M5 streaming** are not started; the new DAG probe finds 23.28% of far and
   5.37% of near selected clusters below 1px. This alone does not establish software-raster ROI.
5. The screen-error cut is a heuristic with a recorded conservative bound, not a strict per-pixel error
   guarantee; image differences and tiny near-view OFF variation are quantified in the runtime report.
6. Optional imported-DAG hysteresis defaults off; toon exact-depth redraw, unsupported strategies,
   shadows and BLAS keep LOD0. Real Bistro retains pre-existing core-validator errors; no source cleanup
   is silently performed by runtime import.

## 9. Document index

The dated records below stay in the repository as the authoritative evidence for each step and are **not part
of this manual** (the manual's reference chapters cover the API; this chapter is the feature overview). Paths
are repository-relative.

**Design (authoritative architecture):**
- `docs/superpowers/specs/2026-10-18-subtilis-design.md` - the main design; sections 12-17 are the actually
  implemented scope per step and supersede the earlier drafts where they conflict.
- `docs/superpowers/specs/2026-10-20-subtilis-m1b-builder-design.md` (implemented),
  `docs/superpowers/specs/2026-10-20-subtilis-m1b-upload-design.md` (implemented),
  `docs/superpowers/specs/2026-10-20-subtilis-m1b-selection-design.md` (implemented).

**Implementation plans:** `docs/superpowers/plans/2026-10-19-subtilis-m1a.md`,
`docs/superpowers/plans/2026-10-20-subtilis-m1b-foundation.md`,
`docs/superpowers/plans/2026-10-20-subtilis-m1b-builder.md`,
`docs/superpowers/plans/2026-10-20-subtilis-m1b-upload.md`,
`docs/superpowers/plans/2026-10-20-subtilis-m1b-selection.md`, plus the related
`docs/superpowers/plans/2026-10-19-review-fixes-and-subtilis.md`.

**Step acceptance records (chronological):**
- `docs/reviews/2026-10-19-subtilis-m1a.md` - accepted; CTest 51/51 at the time.
- `docs/reviews/2026-10-20-subtilis-m1b-foundation.md` - asset + cut reference; CTest 53/53.
- `docs/reviews/2026-10-20-subtilis-m1b-builder.md` - meshoptimizer build; CTest 54/54, 70 builder checks.
- `docs/reviews/2026-10-20-subtilis-m1b-upload.md` - generation/bind; CTest 55/55, 44 checks.
- `docs/reviews/2026-10-20-subtilis-m1b-selection.md` - CPU cut in the frame; CTest 57/57.
- `docs/reviews/2026-10-20-subtilis-selection-checkpoint.md` - historical pause note, superseded.

**Runtime DAG design / implementation:**
- [Runtime spec](superpowers/specs/2026-10-21-object-lod-runtime-design.md) and [plan](superpowers/plans/2026-10-21-object-lod-runtime.md).
- [Runtime acceptance](reviews/2026-10-21-object-lod.md) and [implementation checkpoint](reviews/2026-10-22-object-lod-checkpoint.md).

**Real-scene evidence:**
- `docs/reviews/2026-10-20-bistro/README.md` - historical: the 1024-item build forced a whole-frame fallback,
  so that round could not produce Subtilis evidence; it is the baseline that motivated the capacity work.
- `docs/reviews/2026-10-20-bistro-performance/README.md` - the OFF/ON numbers, the 6.6x capacity unlock, the
  probes, the CPU-selection parallelization and the tuning experiments (negative results included).
- `docs/reviews/2026-10-20-bistro-textured/README.md` - 2906 visible items, 516 texture slots, PBR channel
  captures; textured Subtilis OFF/ON remains open.
- `docs/reviews/2026-10-21-hzb-recovery.md` - the M3 lane that consumes the cut records; measured
  -78.6%/-61.6% GPU frame on moving Bistro views.
- `docs/reviews/2026-10-21-subtilis-converter.md` - the separate CPU offline glTF DAG converter
  (`tools/subtilis`): real-asset measurements, Khronos-validator and LOD0/cut evidence, independent CPU CI.

**Consumers / adjacent manual pages:** the visibility-buffer lane (\ref md_docs_2visibility__buffer "The visibility buffer lane") and the mesh-shader path (\ref md_docs_2mesh__shaders "Mesh shaders"); the project README's feature list is repository-level.

## 10. Milestone status

- M1a - LOD0 visible-run bridge over the visibility lane: **done** 2026-10-19.
- M1b - hierarchy asset, CPU builder, explicit upload, main-view CPU cut: **done** 2026-10-20.
- (prerequisite) packed id 4096 items / 4096 meshlets per item + 1024-slot texture array: **done** 2026-10-20.
- M1c - offline converter DAG import, shared generations, CPU selection and real-scene measurement: **implemented** 2026-10-22 (source-asset exceptions recorded).
- M2 - GPU-side cut selection + finalize, indirect parameters: **not implemented**.
- M3 - Hi-Z occlusion culling: **implemented as the HZB lane** 2026-10-21 (history prediction + current-frame recovery; default off, measured on Bistro).
- M4 - software raster for sub-pixel clusters: **not started** (probe: 22% of far clusters <1 px).
- M5 - streaming/paging: **not started**.
- asset pipeline - automatic import-time build + on-disk cache: **not implemented** (explicit legacy builder and offline payload import are available).
- offline glTF cluster-DAG converter (`tools/subtilis`, `DEREN_subtilis` extension): **implemented and
  accepted as an independent CPU tool** 2026-10-21; runtime consumption added 2026-10-22.
