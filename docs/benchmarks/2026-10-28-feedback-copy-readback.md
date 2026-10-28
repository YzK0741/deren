# Asynchronous feedback copy readback

2026-10-28. Follow-up to the [readback diagnostic](2026-10-28-feedback-readback-probe.md).
The goal is to remove CPU reads from GPU-oriented mapped memory while avoiding the rejected
variant's GPU atomic writes directly into host-cached memory.

## Implementation

Progressive mip and two-tier streaming now keep GPU feedback in storage_gpu_only buffers, with
transfer source/destination capability. Each existing frame slot has independent material/mip GPU
and coherent CPU-readable buffers. The CPU-readable allocation uses uniform_cached with explicit
transfer_destination capability; it is never published to the feedback shader. HOST_CACHED is a
preference, while host-visible coherence is required by the existing allocation contract.

An immutable zero-filled staging buffer resets the GPU feedback by copy. A transfer-write to
shader-read/write dependency makes zero available to the atomics. After the shader, a shader-write
to transfer-read dependency precedes the readback copy, followed by transfer-write to host-read
visibility. The existing wait_and_acquire completes a frame slot before its CPU snapshot is read.
There is no extra submission, same-frame wait, shader change or feedback frame delay.

The original same-frame visibility-streaming/checkpoint path is preserved. Disabling progressive
streaming keeps the existing asynchronous full-resolution restoration. Pipeline readiness is
published only after all feedback allocations and descriptor writes succeed, allowing safe retry
after partial initialization failure.

## Verification and measurements

### Controlled comparison

Textured-DAG Bistro at 1280 x 720, visibility + subtilis DAG + HZB cull/recovery + progressive mips.
Software raster, shadows, TAA, SSAO, bloom, FXAA, GUI, vsync and frame cap are off.
Animation time is pinned at zero; the animation controller still samples the pose each frame.
Near pose: `35,8,42,0,-88,0`; far: `35,18,140,0,-83.07,0`; sweep: 2 degrees per presented frame.
Both arms have the consuming-import ownership work and the rectangular-mip correction below.
The control executes the original direct-feedback methods with unused new member slots; source
patches in the evidence identify the difference. DLL and shader hashes are identical between arms,
and each arm has one executable identity across all its captures.

Each cell pools two reversed-order 360-frame captures, discarding the first 60 presentation intervals
per run (602 intervals per cell). CPU phases average the 60-frame windows after the first window.
GPU values are full-session timestamp means, rounded by the existing logger, not the same warm
interval window. Resource/process sampling, Vulkan validation and operation profiling are absent
from timing captures. Frame and residency CSV traces are enabled equally in both arms.

Milliseconds, before / after:

| View | Mean frame | P95 frame | CPU post | GPU session | Mean frame reduction |
|---|---:|---:|---:|---:|---:|
| Near | 11.919 / 9.959 | 13.166 / 11.316 | 9.370 / 7.429 | 4.940 / 4.345 | 16.4% |
| Far | 12.050 / 10.112 | 13.355 / 11.508 | 9.532 / 7.647 | 8.355 / 8.140 | 16.1% |
| Sweep | 12.862 / 10.645 | 14.894 / 11.938 | 9.805 / 7.685 | 5.480 / 5.945 | 17.2% |

All six individual scene/round pairs improve in mean frame time. CPU post drops approximately
1.89-2.12 ms, consistent with removing the previously measured 1.9 ms mapped-feedback read.
The internal residency-policy timer remains near 0.075-0.082 ms in the fixed views and 0.14-0.16 ms
in the sweep; it excludes feedback preparation, so it never represented the full streaming CPU cost.

The moving GPU session mean increases by 0.465 ms; this is a measured tradeoff, not GPU equivalence.
Its full frame still improves by 2.217 ms. Fixed-view GPU means decrease, but two rounds and transient
uploads cannot separate DMA cost, clocks and scheduling. There are no isolated per-copy GPU timestamps
or memcpy-only CPU timers in these formal captures. The complete-frame benefit, rather than a
single microbenchmark, is the acceptance basis. The implementation is retained for progressive
and two-tier streaming; their existing opt-in configuration remains unchanged.

### Memory

Separate moving memory captures, one per arm. GPU/device-local resource steady allocation rises
from 1,238,643,792 to 1,238,709,328 bytes: **64 KiB**. Total resource allocation including non-local
buffers rises by 200,832 bytes (about **196 KiB**). Device-local resource peak is 1.310932 GiB in
both observed runs; asynchronous texture allocation peaks can vary and are not proof of zero overhead.

VMA's total reserved block capacity increases by **32 MiB**, while its device-local reserved capacity
is unchanged. This is allocator block reservation, distinct from the small live readback allocations.
Process private/working medians also rise by roughly 29/33 MiB in these single captures; these OS
counters include allocations outside vessel and are not exact feature accounting.

Project allocator steady usage is 2.570254 / 2.570256 GiB and lifetime peak is 2.970523 GiB in both
arms to the shown precision. These figures include the common corrected importer and must not be
attributed as additional CPU source-memory savings from feedback copies.

### Images and restoration

Near and far final frames are pixel-identical before/after in both rounds, including within-arm
repeats. Each sweep before/after pair differs by 14,442 of 921,600 pixels, RGB MAE 0.008528 on the
0..255 scale, maximum channel difference 35, with 24 pixels exceeding 8.

Both arms also exhibit exactly that difference between their own rounds. Before round 1 matches
after round 2 exactly, and before round 2 matches after round 1 exactly: both arms produce the
same two moving final images. This rules out treating the pair difference alone as a copy-path
regression, but its underlying renderer/capture cause remains unresolved. These are final-frame
checks, not full-trajectory image equality. A frame-based sweep also runs faster in real time in
the optimized arm, so asynchronous upload readiness can differ along the route.

The restoration probe disables streaming at frame 80 and finishes with wanted=full=456, zero jobs,
zero failures. Its final image is pixel-identical to a separate current-build eager reference.

### Correctness checks

- Real GPU transfer regression: unsupported stub produces 11 failures; implementation passes all
  101 checks with validation enabled. It checks full 64 KiB payloads, two independent host snapshots,
  reset-to-zero and repeated slot reuse. It waits for the device in the test oracle; it does not
  independently prove concurrent shader atomics. Full runtime captures exercise the actual atomics.
- Seven validation captures (control, progressive sweep, restoration, two-tier sweep, original GPU
  streaming sweep, experimental software composition and eager reference) exit zero with no reported
  validation errors. Requested DAG/HZB/software activation gates pass where applicable.
- Full restored-source build and 73/73 CTest pass. An earlier concurrency-6 run hit the unrelated
  stacktrace test's 0.4-second child-start timeout; it passed alone and in subsequent full runs at
  concurrency 2. Loader suite passes 286 checks.
- All 21 final captures pass exit/screenshot/feature gates. Fixed timing views also finish with
  nonzero screen demand and target_ready=wanted; moving views are not fully converged-only tests.
- Independent read-only review found no remaining confirmed synchronization/lifetime issues after
  initialization readiness and atomic read/write barrier corrections. Allocation-failure retry is
  protected by transactional readiness publication; OOM was not exhaustively injected in the renderer.

## Import regression discovered by validation

Before reaching feedback recording, both the original and modified executables failed strict lazy
image creation on the same rectangular texture. The consuming mip generator moved mip0 into the
result and appended one more level after the final advertised mip when a chain ended before 1x1.
The GPU eager/progressive paths accepted or sliced the prefix, hiding the invalid trailing bytes;
the original whole-image lazy path correctly rejected their size.

The generator now stops at its declared final mip, preserving the existing min-dimension mip-count
policy. Literal fixtures check 8x4=168 bytes, 1x8=32, 9x5=220 and 4x4=84, in both copying and consuming
resolution paths. The regression first failed 12 checks and then passed. This correction is common
to both final timing arms, so its memory/capacity changes are excluded from the feedback comparison.
Earlier failed captures are preserved under `tmp/feedback-copy/`; they are excluded from the final
matrix under `tmp/feedback-copy-final/`.

## Limits

One machine, one asset/resolution, two local timing rounds and single memory captures per arm.
Reported frame improvements are descriptive observations, not universal guarantees. Hardware cached
memory availability and transfer scheduling can change the tradeoff. Resource counters exclude driver
and borrowed allocations; vessel counters exclude unrelated standard allocations and freed allocator
caches. The existing tiny moving-image nondeterminism and remaining common CPU recording cost are
not resolved by this change. Shader code, mip selection, budgets and feedback delay are unchanged.

## Evidence

- [Frame/CPU CSV](2026-10-28-feedback-copy-cpu.csv)
- [Memory CSV](2026-10-28-feedback-copy-memory.csv)
- [Compressed manifests, traces, source patches and image comparisons](2026-10-28-feedback-copy-evidence.json.gz)
- Final raw captures: `tmp/feedback-copy-final/`; frozen bundles: `tmp/feedback-copy-control-bin/`,
  `tmp/feedback-copy-final-bin/`. Earlier failed validation bundles/captures remain separately saved.
- Runner/analyzer: `tmp/measure_feedback_copy.py`, `tmp/analyze_feedback_copy.py`.
- Final build/regression logs: `tmp/feedback-copy-restored-build.log`, `tmp/feedback-copy-restored-ctest.log`.
- Device test: `tmp/feedback-copy-device-final.log`; red test: `tmp/feedback-copy-red.log`.
