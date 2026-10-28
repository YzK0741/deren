# Common CPU attribution, 2026-10-28

The next focused change is animation transform invalidation. With animation time pinned at zero,
the controller still samples every frame and reports a scene change whenever a transform channel
exists, even if the resulting local matrix is identical. All twelve diagnostic captures observe
BVH dirty every frame. Rebuilding the full tree costs 1.28-1.42 ms per frame in this matrix.
This is a measured candidate cost, not a promised whole-frame saving.

This investigation is separate from the [closed amnis stage](../reviews/2026-10-28-amnis-phase-closeout.md).
No production behavior was changed for attribution. Four temporarily instrumented source files
were restored byte-for-byte and rebuilt, including a forced rebuild of texture residency.

## Method and evidence

Sixteen captures: twelve diagnostic captures (near/far/sweep, eager/progressive mips, two reversed
rounds), plus four clean near controls surrounding the diagnostic block. Each runs 360 capture
frames with DAG+HZB, 1280x720, Bistro textured asset, error 1 px, hysteresis off and animation time
pinned at zero. Shadows, software raster, GUI, post effects, vsync and frame cap are off.
Animation playback is paused; the controller is not disabled. No validation layers, memory counters,
concurrent builds or tests in these timing captures. An independent eight-frame progressive
validation smoke passed before captures.

- [Per-run CSV](2026-10-28-common-cpu-attribution.csv)
- [Compressed evidence, manifests, frame data, diagnostic patches and scripts](2026-10-28-common-cpu-attribution-evidence.json.gz)
- Local raw captures: `tmp/common-cpu-v2/`; correct frozen bundles:
  `tmp/common-cpu-baseline-v2-bin/` and `tmp/common-cpu-diagnostic-v2-bin/`.

Diagnostic component timers below select frames 61-360 (300 frames) and average both rounds.
All reported frame means use the harness's 301 post-warmup intervals; GPU means cover the session. These windows
are different. Timers/logging and an address-counting hash set perturb the diagnostic path.
Clean near means are 10.116 ms eager and 10.257 ms progressive; diagnostic means are 10.614 and
10.905 ms respectively. These differences include run-order noise and instrumentation overhead;
they are not feature regressions or a correction to subtract from far/sweep measurements.

## Attribution

All durations are CPU wall time in ms. Rows are nested and must not be added together.

| Diagnostic component | Near eager | Near progressive | Far eager | Far progressive | Sweep eager | Sweep progressive |
|---|---:|---:|---:|---:|---:|---:|
| BVH rebuild | 1.330 | 1.352 | 1.388 | 1.381 | 1.417 | 1.283 |
| Frustum query/caster gathering | 0.386 | 0.400 | 0.317 | 0.315 | 0.489 | 0.439 |
| Graph construction | 0.257 | 0.279 | 0.275 | 0.289 | 0.279 | 0.320 |
| Graph prepare | 0.067 | 0.075 | 0.074 | 0.079 | 0.078 | 0.077 |
| Graph record, including callbacks | 7.640 | 7.665 | 8.178 | 8.184 | 8.020 | 7.374 |
| Record residual outside timed lanes | 0.151 | 0.172 | 0.161 | 0.178 | 0.170 | 0.153 |
| Candidate setup/scan | 2.152 | 2.151 | 2.257 | 2.224 | 2.209 | 2.105 |
| Address queries, inside scan | 0.287 | 0.293 | 0.344 | 0.331 | 0.345 | 0.294 |
| Planning, including task group | 1.058 | 1.057 | 1.241 | 1.209 | 1.200 | 1.079 |
| Task group elapsed, inside planning | 0.761 | 0.755 | 0.916 | 0.877 | 0.883 | 0.789 |
| Visibility publication | 0.260 | 0.258 | 0.275 | 0.281 | 0.271 | 0.247 |
| Visibility serial recording/tail | 1.447 | 1.439 | 1.513 | 1.526 | 1.467 | 1.385 |
| Large-batch native command replay | 0.744 | 0.755 | 0.806 | 0.810 | 0.798 | 0.691 |

Graph record includes lane callbacks, visibility scan/planning/publication and deferred operations.
Residual includes graph access/barriers, GPU markers, custom feedback copies and diagnostic logging;
it is not a pure graph-framework timer. Since this residual is small, no further graph-barrier
instrumentation was justified. Task-group elapsed includes worker execution, scheduling and caller
waiting, not summed worker CPU time or pure idle time. Native replay measures CPU Vulkan command
encoding, not GPU execution. Large-batch phases cover only batches with more than 1000 selected
operations; the CSV also records prepare/validate/demand/save, not every backend call in the frame.

The previous coarse candidate-scan measurement must not be described as address-query time.
Near has 5680 address calls for 2638 unique buffer pointers, but direct query duration is only about
0.29 ms. Per-item clocks affect that duration, and the counting set adds further scan overhead.
Duplicate queries alone do not establish a cache's benefit or safe resource lifetime.

## Next change and limits

The concrete chain is `controller::update -> sample_source -> scene_changed -> bvh_dirty -> rebuild`.
`sample_source` returns true for any transform-channel pose after writing target locals, without
comparing their values. `scene_changed` also clears Subtilis history (hysteresis is off here).
The asset contains eight animation clips; a pinned pose is therefore not an inactive CPU controller.

Compare composed target-local matrices before assigning/reporting change. Keep sampling, morph
history and skin publication active, handle import shift and all runtime targets, and detect external
edits even when animation time is unchanged. Do not use a time-only shortcut or a tolerance that
can discard small real motion. This also applies while the camera moves: camera-dependent culling
still runs, but unchanged object transforms need not force a tree rebuild.

Actual animation can legitimately alter bounds every frame; this change does not eliminate that
cost. Incremental BVH refitting is separate work. Candidate preparation remains a substantial cost,
but the present scan probe cannot isolate its allocations, material lookup and shared-object access
well enough to choose a second optimization. Measure the focused invalidation change first.

All 16 runs exited successfully with zero reported errors and one screenshot. Three near
control/diagnostic pairs are pixel-exact; progressive round two differs in 14823 pixels, RGB MAE
0.008548/255, maximum 30. Do not claim universal image equivalence. Async residency timing and the
previously recorded small image nondeterminism remain relevant; no source policy changed here.

Initial `tmp/common-cpu/` captures are excluded. A timestamp-preserving source restore had left the
workspace executable equal to the old feedback control; recompilation mixed incompatible objects.
The accepted frozen amnis bundle was intact. Fresh writes/forced rebuilds and allocation-flag
validation established the v2 baseline before all evidence in this report.
