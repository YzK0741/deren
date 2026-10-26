# CPU-GPU overlap / async texture upload: Bistro benchmark (2026-10-26)

Same Release build, Bistro textured-verified `bistro-core-mr.gltf`, 1280x720 capture, visibility
buffer on, DAG/software raster/HZB/shadows/TAA/SSAO/bloom/FXAA/GUI/vsync off. Capture is 32 frames
with 10 warm-up frames; each cell is the median of 2 rounds. `visibility_texture_streaming` is the only
changed switch. The executable is the local ABI-60 build that adds lazy `prepare()`, the background
upload workers and the speculative next-frame candidates; no CI-published artifact is involved.

## Results

### Fully culled view (scene off-screen)

Camera `0,0,5000,100000,0,0`; no scene geometry is visible.

| mode | mean | p95 | FPS | resident payload | pinned |
|---|---:|---:|---:|---:|---:|
| eager | 13.61 ms | 15.36 ms | 92.2 | all textures (2,338,695,096 B, uploaded at startup) | n/a |
| streaming | **10.61 ms** | **12.68 ms** | **94.5** | **141,907,236 B** | 141,907,236 B |
| delta | **-22%** | **-17%** | **+2.5%** | **-94%** | |

The streaming run reports `prefetched 0, waited 0` because nothing is demanded; the remaining 141.9 MB is
the pinned set.

### Visible views

`far` and `default` see essentially the whole Bistro scene; `near` sees most of it. The required texture
working set therefore stays near the full payload.

| camera | mode | mean | p95 | resident payload | prefetched | waited | update |
|---|---|---:|---:|---:|---:|---:|---:|
| far | eager | 27.01 ms | 29.53 ms | all (2,338,695,096 B) | - | - | - |
| far | streaming | 27.02 ms | 29.90 ms | 2,338,695,096 B | 485 | 0 | 0.18 ms |
| near | eager | 24.08 ms | 26.88 ms | all | - | - | - |
| near | streaming | 27.69 ms | 31.68 ms | 2,293,955,868 B | 483 | 0 | 0.29 ms |
| default | eager | 24.45 ms | 27.30 ms | all | - | - | - |
| default | streaming | 27.80 ms | 29.96 ms | 2,338,695,096 B | 485 | 0 | 0.23 ms |

`prefetched N` is cumulative over the run; the 485 entries are the demanded textures that the upload
workers prepared before `update_texture_residency()` needed them. `waited 0` on every visible run means
the async path never fell back to a synchronous upload during these captures.

## Interpretation

- The memory win is real but only shows up when the view actually excludes most of the scene: 2.34 GB
  -> 142 MB, with a lower mean frame time because the GPU stops sampling 2.2 GB of texture data.
- In the Bistro views tested here almost every texture is still demanded, so streaming cannot reduce the
  payload. The extra CPU prep + worker scheduling makes those captures slightly slower, not faster.
- The async upload mechanism itself is healthy: every visible run finished with `waited 0`, and the
  streaming smoke reports `prefetched 5, waited 0, update 0.02 ms` versus the old synchronous fallback's
  5 waits / 46 ms on the same scene.
- This is not an FPS optimization for small or fully-visible scenes. It is a residency/bandwidth measure
  for views with a smaller working set, and its frame-time benefit is bounded by that working-set change.
- The 141.9 MB pinned set is unchanged by streaming.

## Reproduction

The local harness is derived from `scripts/measure/measure_render_optimizations.py`; it writes
`DEREN_BENCH_FRAME_TRACE` and parses the `texture residency:` line (which now carries the
`prefetched` and `waited` fields). Results are under `build-release-dyn-clang64/bistro-cpu-gpu-bench/`
and `build-release-dyn-clang64/bistro-away-bench/`; the resident bytes are the renderer's payload
accounting, not allocator or process VRAM measurements.
