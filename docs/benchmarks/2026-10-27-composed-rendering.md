# Composed rendering benchmark — 2026-10-27

**CPU recorder follow-up:** this report preserves the measurements before the common operation-stream
CPU fix. A later controlled [before/after study](2026-10-27-operation-stream-cpu.md) reduces composed
frame time from 25.625 to 14.381 ms in its own paired matrix. The old composition delta must not be
interpreted as an unavoidable GPU HZB cost; stream copying and repeated push-range validation amplified
CPU recording work. The later study retains its own build identities, image limits and controls.

**Scope:** measure subtilis DAG + HZB + amnis progressive mip streaming, with the experimental triangle
software-raster comparison. This is a measurement report, not a policy switch: no renderer default was
changed and the experiment switches remain opt-in (`DEREN_PROGRESSIVE_TEXTURES`,
`DEREN_PROGRESSIVE_MIPS`, `DEREN_HZB_*`, `DEREN_SOFTWARE_RASTER_*`).

Base: local `codex/progressive-texture-residency`, Release Clang, RTX 4060 Laptop GPU / NVIDIA 617.42,
AMD64 Family 25 Model 97 Stepping 2. Bistro textured-DAG asset
`D:/deren-assets/2026-10-23-software-raster/subtilis-textured/subtilis-textured.gltf` (464 converted
payload-bearing primitives; the runtime log reports 1,873 adopted instances, 1,540 shared, 531
LOD0-only). Resolution 1280x720, visibility buffer on, animations pinned at 0, shadows/TAA/SSAO/bloom/
FXAA/GUI/vsync off, progressive mip streaming on. The three profiles are:

| profile | geometry | texture | software |
|---|---|---|---|
| `amnis` | ordinary visibility lane | progressive mips | off |
| `dag_hzb` | subtilis DAG + HZB build/cull/recovery | progressive mips | off |
| `all` | same as `dag_hzb` | progressive mips | triangle software raster + GPU survivor routing |

## Method

Each timing run requests 360 frames through the normal capture path; the first 60 frame intervals are
discarded. Two rounds run the profiles in opposite order; every value below is the median of the two
round-level summaries. Cameras are the fixed near pose, fixed far pose, and the near pose rotating
2 degrees per presented frame. Timing is wall-clock presentation interval, not isolated GPU time.
Validation smokes run before the matrix, and no build, test or other GPU workload runs concurrently with
a timing capture.

Separate resource captures use `--resource-memory` (same binary/shaders, no timing comparison). Each of
the nine profile/scenario cells samples GPU buffer/image `device_local` allocation and the project CPU
allocator from frame 0 every 10 frames; the steady figures use frames 180–360. These counters exclude
borrowed swapchain images, driver-private allocations and VMM block retention, so they are not WDDM or
whole-device VRAM. The timing table and the resource table are deliberately separate.

## Activation

The measurement CLI requires logged execution, not just configuration:

| cell | DAG adopted | HZB cull/recovery | software raster | triangle route | GPU routing |
|---|---:|---|---|---|---|
| `amnis` | - | - | - | - | - |
| `dag_hzb` | 1,873 | on | - | - | - |
| `all` | 1,873 | on | on | on | on |

Across the HZB captures the cull session reports **91.05% culled** (1,026,162 of 1,127,036 candidates).
The `all` near log records 2,591 software clusters / 2,587 work entries in the 80x45 tile grid. All
eighteen timing runs exit zero, produce one screenshot each and report no VUID, validation error or
backend failure; `feature_errors` is empty for every cell.

## Timing

Unit: ms. Each cell is `mean / p95`, median over two round-level values.

| profile | near | far | sweep |
|---|---:|---:|---:|
| `amnis` | 22.506 / 26.833 | 24.592 / 28.404 | 22.303 / 26.353 |
| `dag_hzb` | 29.276 / 36.061 | 29.529 / 35.731 | 25.834 / 31.287 |
| `all` | 28.488 / 35.283 | 29.557 / 34.215 | 26.623 / 32.407 |

Relative to `amnis`, `dag_hzb` adds 3.5–6.8 ms and `all` adds 4.0–7.0 ms in these captured views. The
software profile is not consistently separated from `dag_hzb` (near is 0.8 ms cheaper, far is equal,
sweep is 0.8 ms more expensive); this matrix does not establish a software-raster win. The experiment
also uses different geometry work than the ordinary lane, so the delta is not an isolated attribution
to one pass.

## subtilis-only follow-up

This follow-up disables HZB and software raster and leaves only the subtilis DAG enabled, with the same
progressive mip texture path and the same asset/binary/settings. Six timing runs (two rounds) and three
separate resource captures were added. Activation is logged as `adopted 1,873`, `cluster_dag_active
true`, `hzb_cull_active false`, `software_raster_active false`; all runs exit zero with no validation
errors.

### Timing

Unit: ms, `mean / p95`, median over two rounds; FPS is `1000 / mean`.

| profile | near | far | sweep |
|---|---:|---:|---:|
| `amnis` | 22.506 / 26.833 (44.43 FPS) | 24.592 / 28.404 (40.66 FPS) | 22.303 / 26.353 (44.84 FPS) |
| `subtilis` | **21.784 / 24.957 (45.90 FPS)** | **22.604 / 26.145 (44.24 FPS)** | **21.607 / 25.422 (46.28 FPS)** |
| delta | -0.72 ms / +3.3% FPS | -1.99 ms / +8.8% FPS | -0.70 ms / +3.2% FPS |

subtilis alone is faster than `amnis` in these three views. This is a different result from
`dag_hzb`: adding HZB build/cull/recovery to the same DAG reverses the frame-time delta in the matrix
above. The DAG cut changes geometry work, so this is a whole-frame comparison, not a pass attribution.

### GPU resource allocation

Unit: MiB steady median / peak; CPU is the project allocator steady median.

| profile | near | far | sweep | peak | CPU current |
|---|---:|---:|---:|---:|---:|
| `amnis` | 1,180.0 | 1,079.2 | 1,176.0 | 1,414.5 | 7,682.1 |
| `subtilis` | 1,107.8 | 1,031.1 | 1,140.6 | 1,342.4 | 7,821.6 |

subtilis-only lowers the steady GPU local allocation by 35–72 MiB (about 3–6%) and the peak by about
72 MiB, while adding about 140 MiB of project CPU allocator usage. These are local resource counters,
not WDDM.

### Image comparison

Final frame versus the same round's `amnis` frame:

| round | camera | changed pixels / 921,600 | RGB MAE |
|---:|---|---:|---:|
| 1 | near | 18,156 | 0.0180 |
| 1 | far | 16,416 | 0.0663 |
| 1 | sweep | 3,552 | 0.0095 |
| 2 | near | 18,271 | 0.0181 |
| 2 | far | 16,416 | 0.0663 |
| 2 | sweep | 3,538 | 0.0094 |

These are final-frame geometry-route differences; the comparison does not establish per-frame equality.

## GPU resource allocation

Unit: MiB. `median / historical peak` of the local resource counters. CPU is the project allocator
steady median.

| profile | cell | GPU buffer+image local | peak local | CPU current |
|---|---|---:|---:|---:|
| `amnis` | near | 1,180.0 | 1,414.5 | 7,682.1 |
| `amnis` | far | 1,079.2 | 1,414.5 | 7,682.1 |
| `amnis` | sweep | 1,176.0 | 1,414.5 | 7,682.1 |
| `dag_hzb` | near | 1,123.9 | 1,342.4 | 7,822.9 |
| `dag_hzb` | far | 1,047.1 | 1,342.4 | 7,822.9 |
| `dag_hzb` | sweep | 1,181.3 | 1,342.4 | 7,822.9 |
| `all` | near | 1,169.5 | 1,373.5 | 7,823.6 |
| `all` | far | 1,092.8 | 1,373.5 | 7,823.6 |
| `all` | sweep | 1,226.9 | 1,373.5 | 7,823.6 |

The composed profiles do not reduce the steady GPU allocation relative to `amnis` in this capture:
subtilis retains geometry generations and HZB tables while amnis already keeps the progressive texture
floor. The `dag_hzb` peak is about 72 MiB lower and its CPU current is about 141 MiB higher; the
`all` profile adds the software route's buffers. These differences are not a memory-policy conclusion.

## Image comparison

Final screenshots were compared to the same round's `amnis` screenshot (1280x720, 921,600 pixels).
Only the final frame is compared; this is not a per-frame quality score.

| round | profile | camera | changed pixels | RGB MAE (0–255) |
|---:|---|---|---:|---:|
| 1 | `dag_hzb` | near | 3,602 | 0.0096 |
| 1 | `all` | near | 18,156 | 0.0180 |
| 1 | `dag_hzb` | far | 16,416 | 0.0663 |
| 1 | `all` | far | 16,630 | 0.0672 |
| 1 | `dag_hzb` | sweep | 3,553 | 0.0095 |
| 1 | `all` | sweep | 3,553 | 0.0095 |
| 2 | `dag_hzb` | near | 3,600 | 0.0096 |
| 2 | `all` | near | 18,271 | 0.0181 |
| 2 | `dag_hzb` | far | 16,416 | 0.0663 |
| 2 | `all` | far | 16,630 | 0.0672 |
| 2 | `dag_hzb` | sweep | 17,860 | 0.0179 |
| 2 | `all` | sweep | 3,552 | 0.0095 |

The changed pixels are edge/geometry-route differences at the final pose. The second-round sweep
`amnis`/`dag_hzb` pair differs more than the first, which is why this table does not claim frame-exact
equality.

## Validation

Three 32-frame validation smokes cover the composed paths: `amnis` only, `dag_hzb`, and `all`
(subtilis DAG + HZB + software triangle routing). All exit zero, produce one screenshot, report zero
validation errors and pass the independent activity gates. The final matrix uses the same executable
and shader bundle for all eighteen timing runs (one distinct binary-hash set, one shader-hash set).

## Limits

- One GPU, one laptop, one asset, one resolution. Wall-clock capture intervals are not isolated GPU
  timestamps; resource counters are not WDDM or whole-device VRAM.
- The profiles do different geometry work. The timing differences are whole-frame, not pass-level
  attribution, and small deltas are not stable evidence.
- `--resource-memory` runs are separate from timing runs; their frame times must not be read as
  performance.
- Only final-frame images were compared. No continuous-motion quality score, no skinning, no
  transparency/shadow composition and no interactive latency measurement is claimed.
- The experiment switches remain off by default; this report does not change production residency.

## Reproduction

```powershell
python scripts/measure/measure_progressive_textures.py --exe build-release-dyn-clang64/deren.exe `
  --out tmp/composed-run --frames 360 --camera 35,8,42,0,-88,0 `
  --model D:/deren-assets/2026-10-23-software-raster/subtilis-textured/subtilis-textured.gltf `
  --mode progressive --mips --subtilis --hzb --software --gpu-software
```

Use `--resource-memory` for a separate resource capture, and omit `--software --gpu-software` for the
`dag_hzb` profile; omit all geometry flags for `amnis`. The two-round matrix, per-run summaries,
manifests, activation logs, frame/residency traces and screenshots are archived under
`tmp/composed-final/`; resource captures are under `tmp/composed-resources/`.

Tracked evidence: [timing CSV](2026-10-27-composed-rendering.csv),
[resource CSV](2026-10-27-composed-resources.csv) and
[compressed JSON evidence](2026-10-27-composed-rendering-evidence.json.gz).
