# Gaussian performance baseline after the Bloom repair

Measured on 2026-10-12 against commit `f57c54b2`. This is the current baseline,
including the repaired Bloom chain and cached Gaussian GPU path. It supersedes
the earlier pre-Bloom whole-frame numbers for future optimization comparisons;
it does not replace the earlier CPU/GPU correctness and cache acceptance reports.

## Frozen workload

- Release build; Ryzen 7 7745HX (8 cores / 16 threads), RTX 4060 Laptop GPU
  (8188 MiB), NVIDIA driver 617.42, Windows 10.0.26300.
- Vulkan SDK 1.4.357.0; Slang 2026.13.1-1-g84792eb15. Program and all 45 current
  compiled shader-stage hashes are in the archived metadata.
- Existing `flowers_1.ply`: 139,619,083 bytes, 562,974 particles, SH degree 3.
  SHA256: `3641192d70d598894f4d6ad92e0890b4a7336b43aa6af47bd35842c3e2ee8765`.
  The model is not redistributed with this report.
- 1280x720 window and render extent; render scale 1.0; GUI, VSync, frame cap,
  TAA and FXAA off. Shadow enabled. GPU timestamp collection enabled.
- The real post chain is retained: Bloom enabled, intensity 0.8, threshold 0.35;
  composite enabled. These are the application's defaults at this commit.
- Sun direction uses the current configuration default `[0.3, 1.0, 0.5]`, including
  the bouquet rotation toward that direction. IBL sizes: environment 256 with
  5 mips, irradiance 32, LUT 256. This is a different fixed lighting direction
  from the earlier orientation illustrations using `[0, 0.25, 1]`.
- Target `[0.1190, 0.3818, 0.3433]`, pitch 0, starting yaw 0. Static and rotation
  use distance 4.6685. Near uses distance 3.0 and keeps the bouquet in frame.
  Rotation adds 0.5 degrees of yaw per presented frame.
- Three repetitions per scenario, each CPU then forced GPU. Runs were interleaved
  static / rotation / near in each repetition. No builds, tests or additional
  renderer workloads ran during sampling; ordinary desktop activity was not
  controlled. GPU clocks were not locked.

## Sampling and interpretation

Each run presents 364 frames and exports 363 completed-slot timing rows. Discard
the first 60 rows and take rows 61..360: 300 samples/run, 900 samples per mode
and scenario, 5,400 selected samples overall. The saved CSV also retains warmup
and trailing rows. Performance runs disable validation; separate validation runs
followed after measurement.

Medians and P95 below are computed over each scenario's 900 selected GPU samples.
Gaussian total is the sum of prepare/sort/finalize/raster in each sample before
computing statistics. GPU total is the per-sample sum of all 16 timestamp
intervals. Post total sums TAA/debug/Bloom/composite/FXAA/tail. Neither total is
the sum of separately calculated medians.

`frame_ms` is the recorded completed-slot wall-frame duration, not GPU execution
time or display latency. `cpu_prepare_ms` measures the CPU Gaussian preparation
call only, not the full CPU frame. CPU and GPU can overlap; subtracting GPU total
from frame time does not yield a measured CPU cost. Final PNG/HDR capture occurs
after the selected rows and is not part of these timing statistics.

## Current GPU baseline

All values are milliseconds, median / P95.

| Scenario | Wall frame | GPU total | Gaussian total | Post total |
| --- | ---: | ---: | ---: | ---: |
| Static | 6.438 / 6.960 | 2.196 / 2.548 | 1.809 / 2.097 | 0.244 / 0.422 |
| Rotation | 5.571 / 7.072 | 2.195 / 2.601 | 1.784 / 2.071 | 0.232 / 0.488 |
| Near | 6.979 / 7.479 | 2.747 / 3.119 | 2.376 / 2.683 | 0.227 / 0.466 |

| Scenario | Prepare | Sort | Finalize | Raster | Bloom | Composite |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Static | 0.717 / 1.018 | 0.185 / 0.194 | 0.013 / 0.014 | 0.891 / 0.905 | 0.053 / 0.234 | 0.189 / 0.194 |
| Rotation | 0.722 / 0.997 | 0.176 / 0.194 | 0.013 / 0.014 | 0.865 / 0.927 | 0.053 / 0.308 | 0.179 / 0.197 |
| Near | 0.725 / 1.026 | 0.177 / 0.182 | 0.013 / 0.013 | 1.460 / 1.467 | 0.048 / 0.286 | 0.179 / 0.180 |

GPU-path CPU preparation medians are 0.00220 / 0.00235 / 0.00220 ms respectively.
Other intervals and all individual-run distributions are in `performance.json`.

## Repetition spread

Each cell is the median from one 300-sample GPU run, in repetition order.

| Scenario | Wall frame R1 / R2 / R3 | GPU total R1 / R2 / R3 | Gaussian total R1 / R2 / R3 |
| --- | ---: | ---: | ---: |
| Static | 6.539 / 6.515 / 5.287 | 2.204 / 2.208 / 2.123 | 1.817 / 1.816 / 1.747 |
| Rotation | 6.547 / 5.454 / 5.438 | 2.203 / 2.189 / 2.197 | 1.784 / 1.781 / 1.785 |
| Near | 5.952 / 7.081 / 7.060 | 2.745 / 2.749 / 2.747 | 2.375 / 2.375 / 2.378 |

Whole-frame results vary much more than the main GPU intervals. The rotation
result is therefore not evidence that rotating improves throughput. A future
claim should use interleaved baseline/candidate runs on this same machine and
compare stage cost, repeated frame distributions and matching images, rather
than compare one new FPS value to this pooled wall-frame median. Three short
repetitions are not a confidence interval or an automated CI regression limit.

The CPU reference was measured too: pooled frame medians/P95 are static
152.788/168.586, rotation 122.291/170.705 and near 126.700/171.326 ms. Its CPU
Gaussian preparation medians are 94.225, 94.820 and 94.664 ms. These reference
numbers retain the same scheduling/clock limitations; no new speedup promise is
derived from them.

## Image and validation checks

- All nine CPU/GPU comparisons pass the existing final RGB RMSE <= 1/255 and
  pre-transparent HDR RMSE <= 0.005 thresholds.
- Static: maximum final channel difference 5, 44 differing pixels, maximum
  channel RMSE 0.00920 in 0..255 units; HDR RMSE 0.0000252383, maximum HDR
  RGB difference 0.0217285.
- Rotation final yaw 182: maximum final channel difference 1, 39 differing pixels;
  HDR RMSE 0.00000943351, maximum HDR RGB difference 0.00463867.
- Near: maximum final channel difference 3, 132 differing pixels; HDR RMSE
  0.0000234969, maximum HDR RGB difference 0.009765625.
- GPU screenshots are pixel-exact across all three repetitions of each scenario.
  Their RGB hashes and each individual comparison are archived. Near framing was
  inspected visually; it does not clip the bouquet against the screen edges.
- Separate 12-frame CPU/GPU runs of all three scenarios enable synchronization
  validation and pass with no VUID, validation error, synchronization hazard or
  renderer failure. The short rotating check ends at yaw 6, so it adds a pose
  rather than replacing the 364-frame final-yaw comparison. Bloom is present in
  every selected timing row; all forced GPU preparation/sort/finalize/raster
  intervals are nonzero. This task changes no rendering code.

## Evidence and reproduction

Portable evidence is in
[`docs/benchmarks/2026-10-12-gaussian-f57c54b2`](../benchmarks/2026-10-12-gaussian-f57c54b2/):
`metadata.json`, `performance.json`, `timings.csv.gz`, a viewer configuration
template, three losslessly gzipped reference PNGs, and `SHA256SUMS`. Full local
configs, logs, PNGs, HDR captures and the summarizer remain under the ignored
`build-release-dyn-clang64/gaussian-baseline-f57c54b2` directory.
Its `runtime/` snapshot also preserves the measured executable, DLLs and SPIR-V
files for paired comparisons after the normal build directory is overwritten;
all runtime-file hashes are in metadata. Point a copied viewer configuration's
shader path at that snapshot and run its executable with the same external asset.
The snapshot includes libc++ and passes a 12-frame forced-GPU smoke run whose
static screenshot is pixel-exact with the archived baseline.
The model, program package and HDR captures are kept locally, not added to Git.

Use the existing flower asset and a Release build of the recorded commit. In a
fresh output directory, run the same existing checker (which regenerates the
viewer configuration and compares CPU/GPU images):

```powershell
$baselineDir = 'build-release-dyn-clang64/gaussian-baseline-repeat'
foreach ($rep in 1..3) {
    foreach ($scenario in @('static', 'rotation', 'near')) {
        $sweep = if ($scenario -eq 'rotation') { 0.5 } else { 0.0 }
        $camera = if ($scenario -eq 'near') {
            '0,0,3.0,0.1190,0.3818,0.3433'
        } else {
            '0,0,4.6685,0.1190,0.3818,0.3433'
        }
        & source/scripts/windows/check_gaussian_gpu.ps1 `
            -BuildDir build-release-dyn-clang64 `
            -WorkDir "$baselineDir/$scenario-$rep" `
            -Frames 364 -Sweep $sweep -Camera $camera `
            -Validation $false -Timings $true
    }
}
```

Before comparing a candidate, verify the model/configuration fingerprints and
restore baseline shader binaries by their recorded hashes. Read the archived
CSV with `gzip.open(..., 'rt')` and `csv.DictReader`; group by scenario/mode/run,
select sample IDs 61..360, and use NumPy median/percentile(95). PNG gzip archives
decompress to the original PNG bytes, without changing image pixels.

The present cost ranking supports investigating Gaussian raster first, especially
in the near view; preparation is the second substantial Gaussian interval. Sorting
is about 0.18 ms here, so further sort optimization offers a smaller budget.
These are priorities inferred from measured intervals, not yet new optimizations.
