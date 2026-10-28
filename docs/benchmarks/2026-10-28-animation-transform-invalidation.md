# Animation transform invalidation, 2026-10-28

## Change

`controller::sample_source` now compares each composed target-local matrix with its current value
before assigning it and reporting a scene change. The previous implementation returned true whenever
the sampled pose had a transform channel, including a paused clip or a constant channel. That made
`scene_changed()` invalidate the entire culling BVH every frame. It also cleared Subtilis history.

This is a common renderer/animation correction following
[CPU attribution](2026-10-28-common-cpu-attribution.md); amnis policy is unchanged. It does not skip
animation updates by time or camera identity. Sampling, morph history and per-slot skin publication
still run; external node edits and multiple targets remain observable. The comparison is exact,
without an epsilon that could suppress small real motion. Import shift is included before comparison.

Actual animation changes still invalidate bounds and rebuild the full BVH. This is not incremental
BVH refitting and does not establish a speedup for a scene whose geometry changes every frame.

## Capture protocol

Frozen before: `tmp/common-cpu-baseline-v2-bin/`; after: `tmp/animation-invalidation-final-bin/`.
Near/far/sweep, eager/progressive mips, before/after, two reversed rounds: 24 clean timing captures,
360 frames each. Before uses the same restored, current GPU feedback-copy implementation as the
attribution controls. No temporary diagnostic instrumentation remains in production sources.

Both use textured Bistro, DAG+HZB, 1280x720, 1 px error, hysteresis off, animation time pinned at
zero, no shadows/software raster/post effects/GUI/vsync/frame cap. No operation profiling, validation
layers or memory counters in timing runs; no builds or tests overlap them. A separate eight-frame
after progressive validation capture precedes timings. Frame mean/P95 use 301 intervals after
dropping 60; CPU phases use post-warmup 60-frame log windows; GPU means cover the session. CPU and
GPU overlap. The eventual reported reduction cannot be attributed by subtracting GPU from frame time.

Evidence: [per-run CSV](2026-10-28-animation-transform-invalidation.csv),
[compressed manifests, frames, images metrics, sources and reproduction scripts](2026-10-28-animation-transform-invalidation-evidence.json.gz).
Raw captures remain in `tmp/animation-invalidation/`.

## Results

Means of two clean runs per arm; frame times are ms. P95 is the mean of the two per-run P95 values,
not a pooled percentile. Each of the twelve paired runs improves, including the moving view.

| View/mode | Before frame | After frame | Reduction | Before P95 | After P95 |
|---|---:|---:|---:|---:|---:|
| Near eager | 9.731 | 7.869 | 19.1% | 10.547 | 8.727 |
| Near progressive mips | 10.031 | 8.212 | 18.1% | 10.948 | 8.972 |
| Far eager | 9.647 | 8.109 | 15.9% | 10.295 | 8.776 |
| Far progressive mips | 9.958 | 8.239 | 17.3% | 10.826 | 8.901 |
| Sweep eager | 9.690 | 8.554 | 11.7% | 10.639 | 9.860 |
| Sweep progressive mips | 10.499 | 9.089 | 13.4% | 11.791 | 10.493 |

| Progressive CPU wall phase | Near before/after | Far before/after | Sweep before/after |
|---|---:|---:|---:|
| begin | 2.093 / 0.393 | 1.962 / 0.362 | 2.166 / 0.879 |
| post | 7.479 / 7.381 | 7.551 / 7.393 | 7.587 / 7.430 |

The dominant measured change is begin, consistent with removing redundant BVH reconstruction and,
for fixed cameras, its dependent queries. Moving-camera queries still execute, explaining the
larger remaining begin cost in sweep. This comparison uses clean captures, not the diagnostic
address-counting build. It establishes a benefit for fixed object poses with both fixed and moving
cameras, not a bound for fully animated geometry. Only two rounds were run.

GPU session means for progressive near/far/sweep are 4.810/8.300/5.965 ms before and
4.800/8.405/5.165 ms after. These session averages include startup/uploads and different scheduling;
there is no GPU shader change. Far GPU mean increases by 0.105 ms despite lower frame time. Do not
claim uniform GPU improvement or derive CPU cost from frame minus GPU duration.

All 25 captures exit successfully with zero reported errors and one screenshot; the separate
progressive validation capture reports clean validation. Hashes are stable within each arm and
shaders are identical across arms. Both sweep modes are pixel-exact across all before/after pairs
and repeats. Near round two is exact in both modes; round one differs in 14823 pixels, RGB MAE
0.008548/255, maximum 30, 12 pixels above 8. The same near difference also occurs within an unchanged
before or after arm, and matches the attribution report's existing image variation. Far round two
is exact; round one differs in 678 pixels, MAE 0.000291/255, maximum 4. These fixed-view variations
remain unresolved; universal pixel equivalence is not claimed.

## Validation scope

The regression test first failed on the old behavior (261 checks, 6 failures). It covers repeated
pinned sampling, advancing and scrubbing time, clip switches, constant channels, multiple targets,
root import shift and repairing an external target edit. Existing tests exercise morph history across
frame slots and shared-skin matrices. Additional assertions cover representable tiny movement and
repairing NaN external data. Real VMD-file validation requires `VR_MMD_MOTION` and is skipped when
that variable is absent; synthetic MMD tests remain active.

Final `test_animation`: 266 checks, zero failures. Five targeted CTest cases pass:
`test_animation`, `test_docs`, `test_progressive_measure`, `test_progressive_texture`,
`test_runtime_injection`. Independent source review found no blocking issue; regression assertions
for tiny motion and NaN repair were added before final verification. No commit or push was made.

The remaining common CPU work is mostly post (about 7.4 ms in progressive captures). A subsequent
investigation should isolate visibility candidate preparation/material lookup and command recording,
using a lighter probe than the address-counting hash set. Do not reopen amnis or start a broad graph
rewrite based on this correction alone.
