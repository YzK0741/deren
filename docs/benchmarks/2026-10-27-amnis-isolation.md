# Memory acceptance and amnis CPU isolation — 2026-10-27

This report completes the memory/residency acceptance for the textured-DAG Bistro asset and isolates the
remaining CPU cost of progressive mip streaming against an otherwise identical eager-residency run.
It changes no production policy: all comparisons use one frozen binary/DLL/shader bundle and the same
geometry, camera, resolution and render settings. Software raster is off.

The operation-stream CPU optimization is measured separately in
[the operation-stream report](2026-10-27-operation-stream-cpu.md); the captures below use the optimized
recorder.

## Setup

- Asset: `D:/deren-assets/2026-10-23-software-raster/subtilis-textured/subtilis-textured.gltf`
  (1,873 adopted DAG instances, 1,540 shared, 531 LOD0 fallback).
- 1280x720, fixed animation time, offscreen capture; shadows/TAA/SSAO/bloom/FXAA/GUI/vsync off;
  visibility buffer, subtilis DAG and HZB cull/recovery on.
- Modes: `eager` (`visibility_texture_streaming = false`) versus `progressive`
  (`visibility_texture_streaming = true`, progressive mip streaming).
- Timing: 360 requested frames per run, first 60 presentation intervals discarded, two reversed-order
  rounds per scene (near, far, 2°/frame sweep), 602 pooled intervals per cell.
- CPU post is the mean of the reported 60-frame phase windows after the first window. It is a
  wall-clock recording-phase measurement, not utilization.
- Memory captures are separate runs with resource sampling; timing captures have resource sampling,
  validation and operation profiling disabled.
- All runs share one executable/DLL/shader identity; activation is logged as `adopted 1,873`,
  `cluster_dag_active true`, `hzb_cull_active true`, `hzb_recovery_active true`. The near view
  culls about 91.05% of HZB candidates; far and moving views have different cull rates.

## Timing and CPU attribution

Unit: ms. `mean / p95` over the pooled intervals; CPU post is the phase mean.

| scene | mode | mean / p95 | CPU post | GPU session | FPS from mean |
|---|---|---:|---:|---:|---:|
| near | eager | 9.504 / 10.234 | 7.113 | 4.29 | 105.2 |
| near | progressive | 11.465 / 12.246 | 9.061 | 4.68 | 87.2 |
| far | eager | 9.992 / 11.073 | 7.546 | 8.285 | 100.1 |
| far | progressive | 11.711 / 12.670 | 9.346 | 8.635 | 85.4 |
| sweep | eager | 9.688 / 10.864 | 6.968 | 5.85 | 103.2 |
| sweep | progressive | 12.116 / 13.352 | 9.309 | 5.815 | 82.5 |

Progressive mip streaming costs **1.7–2.4 ms of mean wall time** in these views; the CPU post phase
accounts for about 1.80–2.34 ms of that. GPU session time rises by 0.39 ms near and 0.35 ms far;
the sweep difference is -0.035 ms. Two rounds are insufficient to establish general GPU equivalence.
This is the remaining amnis CPU cost after the operation-stream fix, not an HZB or subtilis cost.
The operation profile measures prepare/validation/native replay/state saving inside the recorder;
it does not include all feedback processing and cannot attribute the complete mode delta.
The [feedback-readback follow-up](2026-10-28-feedback-readback-probe.md) isolates a roughly 1.9 ms
CPU mip-feedback copy and records why directly moving GPU feedback writes to cached host memory
was rejected.

## Memory

Separate memory captures. Values are project-allocator/resource counters, not WDDM and not
whole-process VRAM; GPU values are renderer `device_local` resource allocation.

| scene | mode | GPU local steady | CPU allocator steady | process working median | process private median |
|---|---|---:|---:|---:|---:|
| near | eager | 2.965 GiB | 0.323 GiB | 0.648 GiB | 4.240 GiB |
| near | progressive | **1.098 GiB** | 2.570 GiB | 2.848 GiB | 4.636 GiB |
| sweep | eager | 2.966 GiB | 0.324 GiB | 0.655 GiB | 4.260 GiB |
| sweep | progressive | **1.154 GiB** | 2.570 GiB | 2.815 GiB | 4.632 GiB |

Progressive streaming lowers GPU allocation by about **1.8–1.9 GiB** versus eager on this asset, while
retaining roughly **2.25 GiB** of CPU source data for future mip uploads. Eager residency releases more
CPU source because it never needs reload; its steady GPU allocation is higher. These memory captures
predate the consuming-import change. Later single-run peak captures are not matched steady-state
comparisons; the later streaming capture uses two-tier streaming rather than progressive mips.
Initialization peak is a separate lifetime counter: after consuming import it is about
**3.09 GiB** instead of about 5.3 GiB in the measured textured-DAG capture; see
[the texture-source memory report](2026-10-27-texture-source-memory.md).

The texture-source ownership work is documented in that report; it removes import-only decoded
pixels, moves RGBA8 pixels into mip0, and shares immutable source owners without changing mip policy.

## Images

Final-frame comparison against the same round's eager reference:

| scene | round | changed pixels / 921,600 | RGB MAE (0–255) | max channel |
|---|---:|---:|---:|---:|
| near | 1 | 0 | 0 | 0 |
| near | 2 | 0 | 0 | 0 |
| far | 1 | 0 | 0 | 0 |
| far | 2 | 0 | 0 | 0 |
| sweep | 1 | 28 | 0.000127 | 43 |
| sweep | 2 | 14,470 | 0.008655 | 43 |

The moving pair has a reproducible within-binary round difference: progressive round 1 versus round 2
differs by 14,442 pixels (MAE 0.008528, max 35), while the eager two rounds are identical. The fixed
near/far pairs are exact. Restoration at the end of the near progressive run is pixel-identical to the
eager near reference. These are final-frame observations, not a full-trajectory equality claim.

## Restoration and validation

- The near-region validation capture switches streaming off and finishes with all 456 wanted non-pinned
  textures at full resolution, zero pending jobs and zero upload failures; validation is clean.
- The 20 formal captures (timing, memory, diagnostics, restoration) all exit zero, produce one
  screenshot and pass activity checks.
- A separate 64-frame composed + experimental software validation capture exits zero with no reported
  VUID/backend error and all activation gates present.

## Limits

- One GPU, one asset, one resolution. Small timing deltas are local observations, not a fixed guarantee.
- CPU allocator counters include live capacity and allocation rounding, excluding freed allocator
  caches/reserved pages; they are not OS-resident or
  driver memory. GPU numbers are resource accounting, not WDDM residency.
- Progressive captures include asynchronous mip upgrades after the first 60 frames; they are not a
  fully converged-only quality or latency test.
- Only final screenshots were compared. The moving-round difference is recorded, not explained.
- No production default changed and no commit/push was performed.

## Evidence

- [CPU isolation CSV](2026-10-27-amnis-isolation-cpu.csv)
- [Memory CSV](2026-10-27-amnis-isolation-memory.csv)
- [Compressed manifests, traces, image hashes/differences and diagnostics](2026-10-27-amnis-isolation-evidence.json.gz)
- Raw captures: `tmp/amnis-isolation/`; frozen bundle: `tmp/amnis-isolation-bin/`.
