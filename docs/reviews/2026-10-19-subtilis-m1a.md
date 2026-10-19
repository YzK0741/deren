# Subtilis M1a — LOD0 visible-run bridge

## Scope

Implements the user-approved M1a route in `docs/superpowers/specs/2026-10-18-subtilis-design.md` §11. The preceding ownership/GUI/RT fixes remain in the working tree. No commit or dependency import in this batch; RHI ABI remains 46 and GUI ABI 5.

- `[render] subtilis = false` defaults off. Enabling it also requires `visibility_buffer = true` and an admitted frame.
- `geometry/subtilis.cppm` is a pure CPU module with no RHI dependency. It tests object-space bounding spheres against clip planes, preserves source order, and publishes complete LOD0 records before enabling their metadata. Nonfinite bounds/transforms fail open; plane equations support nonuniform scale and shear.
- The result supplies one base/count/source tuple to the raster push block, resolve draw-item table and indirect count. Empty success dispatches zero work; failed publication uses the full shared LOD0 run.
- Reservations check per-item source count, global table bounds, the 14-bit item meshlet budget, and earlier published intervals. A shared geometry alias may fall back individually without overwriting a prior item's selected records.
- While the admitted Subtilis frame owns the culled table, other camera host-cull writers are disabled. Toon exact-depth re-shading still draws its unchanged LOD0 geometry; it cannot overwrite the visibility records. Shadows and BLAS retain their existing policy.
- Both graph adapters declare optional culled-table reads: mesh_shader for raster, compute_shader for resolve. Runtime imports/retains the existing resource generation; no fake GPU select node is added.

## Test evidence

- CPU test first failed because the module was absent, then passed 25 checks. Config/runtime/metadata assertions first failed on missing APIs; the final CPU test includes 29 checks.
- Capacity tests cover the global end, exactly 16384 records, ID overflow, source mismatch, publication failure, overlap and exact adjacency. Zero-result and invalid-number cases are separate from errors.
- `test_render_graph_visibility`: missing optional input fields produced RED; corrected declarations passed 74 checks, including both OFF and ON resource sets and actual write-to-mesh/compute-read barriers.
- The real interactive config generator was run with default answers in an ignored build directory. Its generated TOML explicitly has both switches false; fixtures test true parsing and default/example behavior.
- Full Release build uses `-j12`. Initial full tests exposed a missing CI list entry and sandbox hardlink restrictions; the list is corrected and the complete test run is performed with normal Windows test permissions.

- Final Release CTest after graph declaration repair: **51/51 passed**. Relevant ASan checks: six initial targets passed, then the changed CPU/runtime and new graph adapter tests rebuilt and passed (three tests); seven distinct relevant tests in total.
- Final graph input repair was repeated through the admitted visibility reference and all four device A/B pairs; results remained identical.

- Final default-OFF full gate: **15/15 identical** to `docs/reviews/2026-10-17-render-graph-gate/legacy-hashes.txt`, each scenario deterministic. No references updated.

## Device evidence

The admitted deferred Subtilis scenario matches the checked-in visibility reference `AEE07350B796AC07` byte for byte, with two deterministic captures. Four additional pairs use the same model/camera/40-frame sequence in the existing visibility path and M1a:

| Scenario | First-frame input → selected records | Result |
| --- | --- | --- |
| Toon face | 715 → 368 | Identical PNG hash |
| Toon body with TAA | 1115 → 1061 | Identical PNG hash |
| Sponza, including MASK | 2431 → 1619 | Identical PNG hash |
| Animated skin plane | Whole-frame geometry fallback, as before | Identical PNG hash |

These counts prove that records were filtered; they do not establish a total-frame speedup. Pair captures enable synchronization validation and reject VUIDs, warning/error messages and recording refusals. A fresh screenshot is required for each run.

Local scripts/logs/artifacts: `build-release-dyn-clang64/subtilis-*`, `build-asan/subtilis-*`; the local pair driver is `subtilis-pairs.ps1`, with per-arm configurations and captures under `subtilis-pairs/`. No screenshot references are updated.

## Review

A fresh read-only review found no further issue beyond the culled-table graph declaration omission already identified during implementation. The declarations were added with RED/GREEN tests and then directionally re-reviewed; shared_refs[9] and both consumer stages were confirmed.

## Next boundary

M1b still needs real hierarchy construction, boundary-preserving simplification, geometric error/transform policy and actual mixed-LOD cuts. M1a does not freeze a disk asset or GPU group layout. The next step is to freeze the minimal M1b asset/build plan; software raster and paging remain later milestones.

Final usage checkpoint: 5-hour (300-minute) window used 86%, remaining **14%**. M1a completed before the remaining-5% pause threshold. On continuation, keep polling and pause/write a checkpoint at remaining <=5%; do not automatically redeem rate-limit reset credits.
