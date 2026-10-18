# Documentation audit — 2026-10-18

**Scope:** the whole project's hand-written documentation (not generated output), checked against the CURRENT
source code and against the doxygen/LaTeX reference where that reference is still valid. This is a
correctness/adaptation audit, not a rewrite: every superseded design decision keeps its original text and gains
a dated `**2026-10-18 现状核对：** … (证据：file:line)` note.

**Method:** five independent read-only-plus-assigned-file auditors worked in parallel with disjoint write
scopes; the Lead verified the invariants and committed. Ground truth = current source (`file:line` evidence);
unverifiable claims (historical measurements, external UE references, commit hashes) were left untouched and are
listed as follow-ups/deferred below.

## Why `docs/latex` could not be used as the reference

`docs/latex/` is a **local, git-ignored doxygen snapshot from 2026-10-09…10-12** (`.gitignore:13`,
`refman.log` dated 2026-10-12; it still contains pre-rename entries such as `group__vulkan__core__filters` and
root-level `source/shaders/shading.slang`). It therefore cannot corroborate anything after 2026-10-12 — render
graph adoption, RT/AS stages, the visibility buffer, RHI ABI 33, heap slots 719/755, vessel/stacktrace pages are
all absent or stale in it. The audit used it only to confirm the generated label pattern; every substantive
correction cites the source instead. **Refreshing the manual is an owner-level follow-up and the build entry
points exist**: `scripts/windows/build_docs.ps1` and `scripts/posix/build_docs.sh` (README documents the raw
`doxygen Doxyfile` plus pdflatex steps). Nothing in this audit is visible in the 2026-10-12 snapshot until
that build is run again.

## Coverage

| Area | Documents audited | Edited | Scope |
| --- | ---: | ---: | --- |
| Core/foundation (Doxyfile-INPUT) | 19 | 16 | `mainpage`, `conventions`, `runtime_split`, `pass_io_design`, `render_graph*`, `render_resource_inventory`, `managed_resources`, `resource_clusters`, `vessel`, `stacktrace`, `migration_tradeoffs`, `unified_image_layouts`, `descriptor_heap_*`, `compiler_tolerance`, `host_image_copy`, `README` |
| Features (Doxyfile-INPUT + reference/research) | 11 | 9 | `shaders`, `mesh_shaders`, `deformation_motion_vectors`, `megalights`, `gltf_loader_usage`, `gaussian_loader`, `gaussian_splatting`, `slang_migration`, `reference/*`, `research/*`, `screenshots/README` |
| Design specs (`docs/superpowers/specs`) | 10 | 9 | the 2026-10-09…10-16 design docs |
| Plans (`docs/superpowers/plans`) | 25 | 15 | all plans, including cluster-geometry and visibility-buffer |
| Subtilis design | 1 | 1 (new) | `docs/superpowers/specs/2026-10-18-subtilis-design.md` |

Result: **48 existing documents modified, 1 new design document** (49 paths), ~660 insertions / ~230 deletions
in tracked files. `docs/reviews/**` (historical evidence) and `docs/latex/**` (generated) were NOT edited, and no
source, CMake, script, config or test file was touched.

## Corrections applied (grouped highlights)

**State/status that had become false**
- Render-graph specs and several plans claimed "the scene chain has not migrated / legacy path remains the
  default / opt-in post graph". The graph is now the only recorder (`runtime.frames.cppm:876-878,3147-3150`,
  `runtime.post_graph.cpp:30-47`), the `main_render_graph`/`post_render_graph` config keys are gone, and all
  21 lanes are in-graph.
- Gaussian plans/specs said "not started"; they are implemented (`source/engine/gaussian_splatting/`,
  `render_graph/gaussian.cppm`, lane 17). Status banners added instead of silently ticking checkboxes.
- Megalights was removed 2026-10-17; live-reference lists, shader paths and the RHI gap list corrected.
- Stage 1 (scene snapshot) is genuinely still unimplemented — verified and left so, with ABI/anchors updated.
- `2026-10-17-visibility-buffer.md`: the ON reference hash updated to the current `AEE07350B796AC07` (the old
  `104E00AFDF3630EA` predates the clip-position root-cause fix); slot/config/test paths corrected.

**Wrong identifiers/paths that would mislead an implementer**
- `heap_slots::vis_buffer` → `heap_slots::visibility` (base+719); shader mirror is
  `common/heap_slots.slang` + `heap_slot_constants.slang`.
- `visibility_resolve.comp` → `.slang` (the emitted `.comp.spv` name is historical); `taa.frag` →
  `post/taa.slang`; `gbuffer.frag`/`gbuffer_debug.frag` → their real `scene/`/`debug/` folders.
- `render_graph::compiler` does not exist (it is `builder::compile`); `add_pass(...record_callback)` is
  `add_pass(name, Parameters, pass_options)` + `bind_pass`; `export_state` is `export_resource`; `post_resources`
  is the runtime's `bloom_images`; `shader_stages` → `shader_stage_bits`; `texture_indices[16]` → `indices[16]`.
- Backend/engine namespaces: `deren::vulkan::*` → `deren::engine::*` for camera/light/animation/pan; backend
  `render_resource` → `source/engine/render_resource`; `make_post_pipeline` → `pipelines::build_post`;
  `vulkan.pass.*` → `deren.engine.pass.*`; `source/shaders/reference/official/` → `source/reference/shaders/official/`.
- RHI ABI 32 → **33** (GUI ABI 3) everywhere it was quoted as current; `image_range` DOES carry `aspect`
  (it was `image_copy_region` that lacked it); sampler set is **six**, not five/seven.
- `mesh_geometry.slang`'s "exactly 28 bytes" meshlet comment and `visibility.cppm`'s "lightweight-Nanite
  roadmap" comment are stale SOURCE comments — intentionally left for the Subtilis M1 code commit (see §5 of the
  Subtilis design).

**Counts / capacities / gates**
- Scenario scope: sixteen scenarios (5 core, 1 visibility tier kept out of `-Full`, ten extra) and `-Full` = 15
  scenarios / 30 renders (was "fourteen / 28 / nine").
- `mesh_command_capacity` is THREE runs (one per command class), not two; test count is 43 CMake test targets /
  49 executed on Windows (was 13/14/29/…); module counts (110 modules, vstd 38 files), module versions (README
  table now `deren.utility.shared 1.2.0`, `deren.application_configuration 1.0.0`, runtime 1.0.0, …), toon
  sidecar lanes (16 lanes = 4 record + 12 block lanes), runtime phase line counts, push block 144 B, and the
  BLAS/descriptor-heap acceptance facts (`703 of 1024 slots named` → "reserves 1024, highest used base+795";
  "zero mapping uses" → one survives for RT AS bindings).
- `static_draw_create_info` has no factory or call site today (recorded as fact; re-wire vs drop = owner call).
- Documented pass order is now spelled out (cluster → shadow → opaque → rt shadow → lighting → gaussian →
  transparent → character → rim(s) → taa → post → fxaa/upscale) and the visibility-buffer lane appears in
  `shaders.md`.

## Invariants verified after the edits

- `test_docs` **passes** (`ctest -R '^test_docs$'`): every INPUT document is still byte-pure ASCII, every
  `heap_slots_<name>` mentioned resolves in `render_layout.cppm`, links/anchors and `@code` fences balanced.
- `git status`: 49 paths, all under `docs/` plus the root `README.md`; **no `source/`, `CMakeLists.txt`,
  `scripts/`, config, test or shader change**; `docs/reviews/**` and `docs/latex/**` untouched.
- No build was needed for this audit; the CTest suite is unaffected (documentation is only a build input of the
  manual, which `test_docs` guards). The last verified full-suite state remains 49/49.

## Owner decisions (reported, deliberately not changed)

1. **Regenerate the manual**: `docs/latex` is a stale 2026-10-12 snapshot; rebuild it with
   `scripts/windows/build_docs.ps1` (or `scripts/posix/build_docs.sh`) or retire it. This audit's corrections
   are not visible in it until then.
2. **`docs/megalights.md`**: whole feature removed 2026-10-17 but the doc's design/progress body still names
   removed shaders/keys under a REMOVED banner — archive vs rewrite.
3. **`mainpage.md`**: does not mention the visibility-buffer opaque lane that `render_graph.md` documents as a
   production lane; adding a highlight is editorial.
4. **`render_resource_inventory.md`**: two MegaLights rows remain in the pass table, marked historical only by
   the top note — delete vs keep as record.
5. **`runtime_split.md` §3-§8**: still names removed internals (`gi_*` families, `set_ssgi`, `image_set_family`,
   `sponza_gi`); the status note covers only C1/C8.
6. **README static batching**: `static_draw_create_info` exists but nothing builds one — re-wire or drop.
7. **Cluster-geometry plan 2.6**: cannot be ticked (registry/passchain/runner still present) but "尚未开始" is
   no longer accurate; the remaining scope is likely just deleting the old runner.
8. **Gaussian-splatting plan (2026-10-09)**: phases 0-3 are actually complete but unchecked; archive or tick
   with owner approval.
9. **Gaussian viewer-integration plan**: Task 4/5 have no code evidence found; Task 7 (placeholder via
   `import_scene`) addresses code that no longer exists — retire or redefine.
10. **Historical measurements kept verbatim**: `descriptor_heap_migration` §Acceptance (13/13, gate 14/14, and
    its own 15-vs-21 push-site mismatch), `host_image_copy` "THIRTEEN capturing scenarios",
    `unified_image_layouts` §2's `megalights_temporal.cpp` mention — they describe past runs and must not be
    "corrected" without a new measurement.
11. **Production source comment is mangled**: `source/application_configuration/application_configuration.cpp:619-622`
    has a sentence cut mid-line ("…clamped to the shader's own compile-time bound (see" then an orphan clause).
    Out of documentation scope; needs a source fix.
12. **Citation style**: specs cite `path/file.cppm:133` as plain text, not resolvable Markdown links; changing
    that convention is a project-wide editorial decision.
13. **Stale source comments to fix with the Subtilis M1 code commit**: `mesh_geometry.slang:119` ("28 bytes";
    actual 48 B via std430) and namespace `deren::vulkan::meshlet` (actual `deren::engine::meshlet`);
    `visibility.cppm:20-21` "lightweight-Nanite roadmap" → "Subtilis roadmap".

## Deferred (not code-checkable, untouched)

Performance/latency/disk-size figures, GPU-driver strings, external UE 5.8.2 file:line references, commit
hashes, and acceptance measurements in historical records. The Subtilis design review (fifth auditor) fixed its
own document, including the one load-bearing error it found: the M1 plan cannot be "table write only" — the
visibility raster shader needs a host-culled-table branch plus four host wirings, or the frame silently loses
geometry.

## Follow-up applied in the same session (2026-10-18, later)

The owner answered three of the open decisions: remove MegaLights from the documentation, add descriptions for
the new feature, and complete the damaged source comments.

- **MegaLights documentation removed.** `docs/megalights.md` and
  `docs/reference/megalights_stochastic_lighting.md` are deleted. `Doxyfile`'s `INPUT` entry for the page was
  replaced by the new visibility-buffer page, the mainpage's feature bullet and chapter-table row were replaced,
  the two README links were removed, dead links in `slang_migration.md` and `unified_image_layouts.md` now point
  at the history/review record, and `render_resource_inventory.md`'s two MegaLights rows were removed from the
  production pass table (its 2026-10-17 note now says so explicitly). Mentions in migration/plan/spec documents
  are kept: they are dated records of a chain that existed, not claims that it exists.
- **New feature documented.** `docs/visibility_buffer.md` was added (manual INPUT, ASCII) and linked from the
  mainpage (highlight bullet plus chapter row 7) and README: pipeline, id layout, admission/fallback, MASK
  handling, near-plane clipping, the gate and its frozen reference, and the measured limits/cost. The Subtilis
  hierarchical-geometry design is linked from the mainpage and README as a design-phase item.
- **Source comments completed.** The orphaned MegaLights comment fragments left in
  `source/application_configuration/application_configuration.cpp` (a sentence cut mid-line after the feature
  removal) were deleted. `source/shaders/geometry/mesh_geometry.slang`'s record comment was corrected from
  "28 bytes `deren::vulkan::meshlet`" to the real 48-byte `deren::engine::meshlet` layout with its std430 offsets.
  `source/engine/visibility/visibility.cppm`'s roadmap comment now says Subtilis and points at the design instead
  of the old "lightweight-Nanite" wording.
- **Owner decisions 2, 3, 4 and the source-comment half of 11 are therefore resolved**; the remaining decisions
  above (manual rebuild, megalights history rows elsewhere, runtime_split hazard list, static batching, plan
  checkboxes, citation style) still stand as recorded.
