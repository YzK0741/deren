# Measurement instruments

The instruments a rendered frame is measured with. They are here rather than beside the captures because a
capture is 4 MB of scratch that a `-Clean` may take with it, while these are the reason a recorded number can
be re-derived at all: the feature docs quote means, 4x4 tile tables and per-material buckets, and every one of
them came out of one of these.

The captures they read come from a normal `deren run <project> --capture-frames N` launch: the PNG lands in the
project's `[paths] screenshot_dir` and the log line prints its path. These instruments do not launch the
application themselves. The capture harness (`scripts/gates/capture.ps1`, `check_render.ps1`) and the
instruments that wrapped it drove `deren.exe --config`, a flag the application stopped reading when it moved to
the managed host; they were retired in #90. The numbers those runs recorded stay in the feature docs.

The render instruments need Python 3 with Pillow (`python -c "import PIL"` must work) and nothing else.
The subtilis converter acceptance instruments additionally need NumPy (the same requirement as
`prepare_bistro_geometry.py`); the Bistro asset and the official Khronos validator they use are
Task 8 acceptance inputs outside the repository, not engine build dependencies.

## The instruments

| Script | Question it answers |
| --- | --- |
| `mean.py <a.png> ...` | the per-channel means. Every number in the records is quoted in **mean green** - one channel, so that a colour change cannot hide a luminance change |
| `diff.py <a.png> <b.png>` | how two frames differ: signed mean per channel, how many pixels differ at all / by >1 / by >4, the worst pixel, and a 4x4 tile table of the mean difference. **The workhorse**, and the tile table is the part that matters - a single frame-level mean cannot tell a global bias from a change ordered by the scene |
| `tiletab.py <on.png> <off.png>` | the same 4x4 table as an absolute difference AND as a percentage of that tile's mean in the off-frame. This is the form that shows a change is *ordered by the scene* rather than flat |
| `corr.py <a_on> <a_off> <b_on> <b_off>` | the Pearson correlation between two per-pixel difference images: do two estimators agree about *where* they moved, not merely about how much? A frame-level coincidence of means without correlation is not agreement |
| `spec_material.py <on> <off> <rough> <metal> <normal>` | an A/B delta **bucketed by material** instead of by tile - the instrument for a MATERIAL response, which a tile table would only show as a map of where the spheres are. The three channel images come from `[render] gbuffer_debug` with `gbuffer_channel` 2 (roughness), 3 (metallic) and 1 (normal); the normal channel is the surface mask, because the debug view paints the cleared background black in every channel and a unit normal can never map to (0,0,0) through `normal*0.5+0.5`. **Caveat**: the debug view is tonemapped on its way to the screen, so the buckets are in display units - ordinal, not linear roughness |
| `shadow_pose.py <raster_t0> <raster_t1> <rt_t0> <rt_t1>` | does an animated skinned mesh's ray-traced shadow follow the pose? The object moves in all four captures, so the instrument is the difference OF the differences: what survives subtracting the traced motion from the raster motion is the pose dependence only the raster path has. A traced shadow stuck in the bind pose leaves it as large as the shadow's motion |
| `motion_channel.py <motion_on.png> <motion_static_ref.png>` | did the pixels that DEFORM report motion? It reads the "did not move" constant out of a static reference capture instead of assuming 128 - channel 8 renders through the post chain, so it lands at 206 on this machine and a hard-coded constant would break on the next driver - and reports the share of geometry pixels that report anything else. A rigid scene reads 100% still; a swept deforming mesh ALSO read 100% still (229,267 px, one distinct value) before deformation-aware motion vectors existed, which is the bug; and a PINNED pose must keep reading 100% still afterwards, which is the null test that says the new path can be zero |
| `mask_stats.py <cascade> <rt_nobake> <rt_baked>` | is the alphaMode MASK bake moving a ray-traced shadow **towards** the raster one (which discards per pixel and is therefore the reference)? Reports the error against it before and after, and the correlation between "what the bake changed" and "how wrong the unbaked frame was" - the part that says whether it moved in the right direction rather than merely by a lot. This is the instrument whose answer was negative |
| `mask_scan.py <ModelsDir> [asset ...]` | CPU reconnaissance over the sample assets: for every alphaMode MASK material, how many triangles would a per-triangle rule collapse at the three vertex UVs alone? The number the shader implementation is then checked against |
| `mask_rules.py <gltf> [materialIndex]` | the same question for one asset under three rules of increasing cost (3 vertex UVs / a 4x4 barycentric grid, which is what `source/shaders/mask_bake.comp` does / every texel of the UV triangle), plus the share the raster path effectively keeps - which is what the bake has to agree with |
| `make_subtilis_converter_fixtures.py --out-dir <dir>` | generates the deterministic small and synthetic large grid GLB fixtures for the standalone subtilis converter measurement matrix (self-contained, no downloads) |
| `measure_subtilis_converter.py ...` | runs the converter asset x `--threads` x rounds matrix, recording every report JSON, external wall, sampled peak working set, output size/hash and per-round values; writes `measurements.json` with medians from all rounds |
| `verify_subtilis_converter.py gate ...` | structural gate over `measurements.json` (complete stage rows/rounds, accumulated == sum of exclusive stages, real cross-group DAG, byte-identical payloads, medians from every round); `--self-test` demonstrates the negative controls |
| `verify_subtilis_converter.py check ...` | runs the pinned official Khronos validator (separate acceptance tool, hashed manifest) on normal outputs and export-cut previews plus `deren-subtilis validate --full`, and checks LOD0 identity / cut monotonicity; writes `acceptance.json` |
| `subtilis_converter_gltf.py` | shared minimal glTF/GLB reader used by the converter acceptance checks (stdlib + NumPy) |
