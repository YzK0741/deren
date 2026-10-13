# CPU skin history: remove upload-memory reads

The Gaussian CPU hotspot was unrelated to sorting. `begin_recording()` copied
all 2048 skin matrices out of a write-oriented mapped upload allocation every
frame, including the flower scene. Keeping history from CPU upload sources
removes that read and limits publication to the uploaded extent.

## Implementation and ordering

`set_skin_matrices()` retains a CPU copy per frame slot and computes the upload
fingerprint from the input span. The mapped allocation is only written. Prefix
uploads preserve their previously uploaded tail; empty uploads preserve the
block, and uploads beyond capacity remain clipped to 2048 matrices.

`advance_motion_deformations()` publishes the preceding rendered frame's CPU
history into the current, paced slot before refreshing that history from the
current slot's CPU copy. It does not use the last rendered pose of the same slot
as history. A first-frame upload gets identity history; subsequent missing
matrices are zero, matching initially zero-filled current allocations.

Publication covers the larger extent of the preceding and current frame. This
preserves motion when slots have different upload lengths. A shorter prefix is
not a declaration that the remaining joints disappeared: the existing API leaves
that tail intact, so the CPU copy must do the same. Matrices outside both extents
are untouched. With no skin upload in either frame there is no buffer copy.
An identity-only setup upload needs just four matrices, rather than 2048.

GPU buffers, descriptors, shader code and recording order are unchanged. The
additional CPU current-pose storage grows with uploads, up to 128 KiB per slot;
the existing global history also grows only to the used extent.

## Why no async copy in this change

An asynchronous copy could overlap independent scene collection if completed
before its first GPU consumer. Moving the original mapped read to a worker would
retain its memory cost and add task, ownership and synchronization requirements.
After removing that read and reducing the extent, the entire flower CPU begin
phase is approximately 0.01–0.02 ms. There is no measured remaining copy hotspot
to justify a future here. A large animated-rig workload can be measured separately
before introducing overlap through the existing task pool.

## Interleaved Release comparison

Baseline is the frozen `f57c54b228356c2e43caee9e8bcdea7f175f2ae4` runtime.
Candidate is the current Release build with this CPU history change. Both use
the same flower model, shaders, camera, Bloom and 1280×720 settings from the
recorded baseline. GUI, VSync, frame limit, AA and validation are disabled for
timing. GPU sorting is forced and confirmed by logs.

Each scenario has three repetitions per variant. Order is baseline/candidate,
candidate/baseline, baseline/candidate. Each run presents 364 frames, produces
363 timing rows, discards the first 60 rows and selects rows 61–360. There are
900 selected frame samples per scenario and variant. GPU runs were serial,
without concurrent builds or tests; desktop activity and hardware clocks were
not controlled.

| Scenario | Frame median, old → new (ms) | Frame P95, old → new (ms) | Median ratio | GPU interval sum median, old → new (ms) |
|---|---:|---:|---:|---:|
| Static | 6.471 → 2.823 | 7.417 → 3.136 | 2.29× | 2.232 → 2.161 |
| Rotation | 6.521 → 2.909 | 6.972 → 3.212 | 2.24× | 2.225 → 2.197 |
| Near | 6.978 → 3.474 | 7.484 → 3.851 | 2.01× | 2.753 → 2.804 |

These are completed-slot wall frame times, not isolated CPU durations. The GPU
interval sum is reported separately; it is not subtracted from frame time to
derive a CPU cost.

| Scenario | CPU begin, old → new (ms) | Present, old → new (ms) |
|---|---:|---:|
| Static | 3.66 → 0.01 | 2.72 → 2.62 |
| Rotation | 3.63 → 0.01 | 2.75 → 2.69 |
| Near | 3.64 → 0.02 | 3.27 → 3.27 |

CPU phase values are medians of 15 rounded 60-frame window means per variant,
after discarding the initial window. They are not per-frame medians or separate
copy timings. The begin-phase reduction and relatively stable GPU durations
support attributing the improvement primarily to the removed CPU work.

## Correctness and verification

The runtime injection regression ran against the real runtime and a CPU-backed
RHI probe. Before the fix it failed five checks, including unwanted history
writes without uploads and history read from deliberately corrupted mapped
memory. After the fix it passes. Coverage includes two-slot rendered ordering,
distinct setup-time slot poses, first-frame identity, growth/shrink between slot
extents, retained prefix tails, empty uploads, invalid slots, and an untouched
sentinel beyond the published extent. Test-only heap inspection remains in the
probe, not in the runtime API.

Release build succeeded and all 24 CTest tests passed. The complete suite was
rerun after the additional first-frame/empty-upload checks.

All 15 baseline/candidate capture pairs have identical final pixels: nine flower
pairs and six animated-plane pairs. The plane advances animation by 0.02 seconds
per presented frame, with captures at frame 1, 40 and 41 in motion-channel and
TAA modes. This exercises both frame slots and deformation, rather than a pinned
pose. All nine flower linear HDR captures also match byte for byte.

The six animation pairs enable synchronization validation. Motion-channel
captures expose existing `vkCmdPipelineBarrier2` WRITE_AFTER_WRITE reports in the
frozen baseline: one error at frame 1, and ten logged error lines at frames 40/41
(including duplicate-limit suppression). The frame-1 TAA capture exposes an
existing GENERAL-versus-UNDEFINED image-layout report; frame-40/41 TAA captures
are clean. Candidate error messages and counts match baseline after normalizing
handle addresses. This is evidence of no new validation report, not a claim that
all animated rendering is validation-clean. These image-barrier issues remain
separate follow-up work.

The frame-40 motion capture has 128,827 foreground pixels and 407 distinct
red/green motion-channel values, including the still value (206,206) and moving
values. The matching images are not simply two empty or uniformly still frames.
An additional 12-frame near-flower CPU/GPU check is synchronization-validation
clean in both paths. Its maximum channel RMSE is 0.0108 in 8-bit units and its
linear HDR RMSE is 0.0000235, within the existing 1/255 and 0.005 gates. This
CPU/GPU comparison has the small expected sort-path differences; it is separate
from the exact old/new GPU comparisons above.

## Retained evidence

`docs/benchmarks/2026-10-12-skin-history/` contains per-run and pooled results,
all 6534 timing rows, the renderer logs, representative losslessly compressed
captures, measurement/analysis scripts, executable/source/shader fingerprints
and SHA256SUMS. Full local capture outputs remain under
`build-release-dyn-clang64/skin-history-check/`.

The previously recorded flame graphs remain the pre-fix evidence; no new ETW
capture or post-fix flame graph was substituted. No commit or push was performed.
