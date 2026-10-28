# Parallel visibility recording, 2026-10-28

Status: **optimization paused after recording this state.** The parallel path is opt-in
(`DEREN_PARALLEL_VISIBILITY=1`); the serial path remains the default.

## Change

The per-pass record trace showed that `scene visibility raster` dominated the CPU graph-record
callback while every other pass was below 0.1 ms. The visibility raster therefore records disjoint
item ranges into up to eight per-frame-slot `mergeable_command_buffer` segments on the task pool,
then executes those segments in item order inside its own rendering instance. Each segment rebinds
the pass pipeline, viewport, scissor, cull and depth-write state; each item still writes its own
host table row, pushes its own block and issues its own mesh-task indirect dispatch. The serial
recorder is unchanged and remains the default.

The optional segments are created lazily on the first parallel visibility frame, after the scene
material-import peak, because creating them beside the existing per-slot pools during import could
push a device-memory allocation over budget.

## Diagnostic attribution

`DEREN_RECORD_TRACE=1` produced these 24-frame serial means for the visibility recorder:

| Phase | Mean |
|---|---:|
| candidate scan | 1.728 ms |
| per-item planning | 1.272 ms |
| publication rule | 0.287 ms |
| RHI record | 1.525 ms |
| total | 4.811 ms |

The trace itself is diagnostic and is not enabled in the comparison captures. Combining the trace
with the parallel path occasionally hit a startup device-memory allocation failure; the measured
comparison below uses the parallel path without tracing.

## Method

- Same binary, shaders and asset for both arms; `DEREN_PARALLEL_VISIBILITY` is the only renderer
  difference. Both arms use textured-DAG Bistro, DAG + HZB, eager texture residency, 1280x720,
  camera `35,8,42,0,-88,0`, animation pinned at zero.
- 140 requested frames per capture, two reversed-order captures per arm. Frame mean/P95 use the
  harness's post-warmup intervals; CPU phases are the 60-frame windows after the first window.
- Timing captures have no validation layers, operation profiling, record tracing or memory
  sampling. A separate 16-frame validation capture ran with the parallel switch and Vulkan
  validation enabled.

## Results

Each cell is one clean capture; the average is the two reversed-order runs.

| Arm | Capture | Mean ms | P95 ms | CPU post ms | CPU total ms | GPU session ms |
|---|---:|---:|---:|---:|---:|---:|
| serial | r1 | 11.770 | 13.415 | 10.10 | 11.63 | 4.38 |
| serial | r2 | 11.651 | 13.184 | 10.07 | 11.52 | 5.90 |
| serial | **average** | **11.710** | **13.299** | **10.085** | **11.575** | 5.14 |
| parallel | r1 | 9.956 | 10.941 | 8.34 | 9.82 | 4.75 |
| parallel | r2 | 9.486 | 10.264 | 7.92 | 9.22 | 5.78 |
| parallel | **average** | **9.721** | **10.602** | **8.130** | **9.520** | 5.265 |

Observed reduction:

```text
mean frame   -1.989 ms  (-17.0%)
P95          -2.697 ms
CPU post     -1.955 ms  (-19.4%)
CPU total    -2.055 ms
GPU session  +0.125 ms  (single-session noise, not a claim)
```

The GPU values are full-session timestamp means; the two runs differ by more than the serial/
parallel delta, so this report makes no GPU-equivalence claim. CPU and GPU overlap, and the frame
reduction is not an additive CPU/GPU decomposition.

## Images and validation

The two serial captures are pixel-identical, and **parallel r2 is byte-identical to both serial
captures**. Parallel r1 differs by the already-known 14,823-pixel capture-order pattern
(RGB MAE 0.008548/255, max 30); parallel r1 vs parallel r2 has the same pattern. This is the same
renderer nondeterminism recorded in earlier captures, not evidence of an item-order change.

The separate 16-frame `--parallel-visibility --validation` capture exits zero with one screenshot,
no reported VUID/backend error and every requested activation gate present.

## Limits

- One GPU/machine and two runs per arm; the serial post values are higher than the earlier
  single-round sample, so the percentage is local to this capture window.
- Parallelism is limited to eight segments; candidate scan, per-item planning and publication
  remain serial. The recovery raster remains serial and is not included in this change.
- The trace/parallel combination is not a formal measurement path.
- No commit, push, renderer default change or image-equality claim is implied.

## Evidence

- [Per-run comparison CSV](2026-10-28-parallel-visibility.csv)
- [Compressed manifests, frame summaries, CPU phases, source hashes, image metrics and commands](2026-10-28-parallel-visibility-evidence.json.gz)
- Raw captures: `tmp/visibility-state-serial/`, `tmp/visibility-state-parallel/`,
  `tmp/visibility-state-serial-r2/`, `tmp/visibility-state-parallel-r2/`.
- Diagnostic trace: `tmp/visibility-phase-serial/`; validation: `tmp/parallel-validation-final/`.
- Implementation commits: `81cd858d` (trace), `b3840fae` (parallel path), `9edb2f45` (lazy segments).

## Next work (deferred)

1. Parallelize the candidate scan and per-item planning, which the diagnostic trace shows are the
   next two costs.
2. Apply the same segment mechanism to `visibility recovery raster`.
3. Decide whether the eight-segment fan-out should be tuned or made adaptive after those changes.
