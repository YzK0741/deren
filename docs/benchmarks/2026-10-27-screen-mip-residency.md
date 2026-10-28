# Screen-driven mip residency — 2026-10-27

The second increment adds intermediate physical mip images and GPU screen-demand feedback, behind
`DEREN_PROGRESSIVE_MIPS=1` plus `DEREN_PROGRESSIVE_TEXTURES=1`. The original synchronous GPU mode and
the preceding two-tier progressive mode remain available. Neither progressive mode is enabled by default.

## Results

Same RTX 4060 Laptop GPU, Release, Bistro MR, 1280x720, fixed animation time, shadows/TAA/SSAO/bloom/FXAA
off. Performance captures present 360 frames and discard the first 60 intervals. Two moving rounds
reverse mode order; no builds, tests or memory samplers run concurrently with these timing captures.

| Scenario | Two-tier mean / P95 ms | Screen mip mean / P95 ms |
|---|---:|---:|
| Fixed | 20.712 / 22.196 | 20.609 / 22.264 |
| Sweep 2 degrees/frame, round 1 | 20.528 / 22.823 | 20.614 / 23.326 |
| Sweep, round 2 (reversed order) | 20.544 / 23.142 | 20.536 / 23.172 |

Frame time is essentially unchanged in these runs. The improvement is lower memory/upload volume
and closer image quality while moving, rather than a further frame-rate increase.

## Actual dedicated GPU memory

Two **separate**, fixed-camera 900-frame runs enable the Windows GPU-process memory sampler. These
runs are excluded from the performance table. Ignore the first 180 frame timestamps; sample the
remaining rendering interval, excluding application teardown. Sum the target PID's adapter instances.

| Mode | WDDM dedicated steady median | Steady samples | Shared steady median |
|---|---:|---:|---:|
| Two-tier | 2,067.125 MiB = 2.019 GiB | 49 | 30.652 MiB |
| Screen mip | 1,810.625 MiB = 1.768 GiB | 50 | 30.652 MiB |

This fixed-view pair reduces the process's reported dedicated GPU memory by **256.5 MiB (12.4%)**.
Both modes' fixed final images are pixel-exact to the fully preloaded reference. These are WDDM
process counters, not whole-device usage or a claim about physical residency on every adapter/driver.
The counters include more than textures, and allocator/driver retention makes the saving smaller
than the texture-payload reduction. This is one pair on one laptop, not a universal memory ratio.

The initial restricted CIM queries failed with access errors and produced zero memory samples.
The repeated permitted runs have sampler exit zero and 145/162 total samples. Failed sampler runs
are excluded from memory conclusions; failure logs are preserved in the evidence archive.

## Texture payload and quality

| Metric | Two-tier | Screen mip |
|---|---:|---:|
| Fixed referenced texture payload, final | 666.333 MiB | 317.332 MiB |
| Fixed optional resident payload, final | 502.000 MiB | 143.333 MiB |
| Compulsory texture floor | 164.333 MiB | 173.999 MiB |
| Fixed upload payload, entire run | 502.000 MiB | 183.166 MiB |
| Sweep upload payload, round 1 | 1,712.000 MiB | 243.166 MiB |
| Sweep peak referenced payload, round 1 | 489.667 MiB | 331.332 MiB |
| Sweep terminal RGB MAE, 0–255 | 0.422091 | 0.008750 |
| Sweep terminal pixels differing from preload | 11.3685% | 1.5737% |
| Sweep terminal pixels with max channel error > 8 | 1.7291% | 0.0033% |

Payload accounting includes current and retired frame-bank texture owners but excludes targets,
geometry, IBL, allocation alignment and VMA cached blocks. It is distinct from the WDDM table above.
The mip floor is slightly larger because it includes each coarse image's available lower mip chain.

Fixed output is pixel-exact to preload after convergence. Of 98 requested textures, 25 are adequately
served by their coarse floor, 51 remain at an intermediate mip and 22 need mip zero. All 98 reach
their screen target; stopping at intermediate resolution does not change this fixed reference image.

Both screen-mip sweep terminal images match each other exactly. They are **not** pixel-exact to the
moving preload reference: maximum channel error is 83, with 30 pixels above error 8. These are terminal
frame comparisons, not validation of every moving frame. Raw comparisons are in image-comparison.json.

Across steady sweep updates, about 78.0% of requested entries meet the last completed GPU feedback's target,
versus about 35.8% reaching the two-tier mode's full-resolution target. Targets differ between modes;
this count is unweighted by pixel coverage and should not replace image comparisons.

An optional 64 MiB budget keeps intermediate detail instead of loading only a few full images:
final optional payload 63.333 MiB, 77/98 screen targets met, RGB MAE 0.03574 against fixed preload.
The compulsory floor is additional to that budget. Pending uploads remain at most two jobs and
128 MiB; observed peak pending payload is 10.667 MiB.

## Mechanism and remaining limits

The visibility resolve already reconstructs UV-per-pixel scale. It now optionally atomically records
the maximum desired extent per material, using `2 / footprint` (one mip of safety), capped at 16384.
The request is independent of the actual resident image size. A separate 64 KiB feedback buffer per
frame slot avoids conflicts with material visibility feedback. The graph declares its storage access,
the command stream publishes shader writes to host reads, and the CPU consumes only a completed slot.

Existing source mip suffixes become smaller GPU images. Single/partial-chain sources generate missing
tiers on a worker. A texture advances one mip at a time while its preceding valid image remains usable.
Visible floor images get promotion priority over already refined images. Budget selection distributes
intermediate tiers before allocating further detail. Small images whose entire original chain fits
in the floor reuse that complete image; pinned alpha/transparent/toon/overlay data stays full quality.

The existing software-raster flag remains in push word 3; optional mip feedback has its own word 4
(byte offset 12). Descriptor/image lifetime uses the preceding increment's completed-slot banks.
Restore completion requires mip zero and no pending replacement, not merely any intermediate image.

There is a cost: fixed convergence takes **135 updates / 134 uploads**, versus 102 updates / 98 uploads
for two-tier. The first frame includes substantial initialization, so these counts are not translated
into wall-clock loading latency here. More smaller uploads reduce byte volume but add fixed per-image
cost. The backend still performs two graphics-queue submissions/waits per image; it is a clear next
optimization candidate, not removed by this increment. Invisible tiers still use a 30-update retention
window, and footprint estimation is per triangle/material rather than per texture page or anisotropic
per-pixel derivative. No sparse residency, virtual texture pages or CPU HZB was added.

## Verification and reproduction

- Full build and **73/73 CTest** pass, including four measurement-evidence failure tests. Initial parallel tests plus asset load caused the existing
  0.4-second symbolizer-process startup test to time out once; a quiet lower-concurrency complete run
  passes without changing that test. Both logs are retained.
- 32 pure policy/filter/layout assertions pass; graph tests exercise optional feedback access, and
  the CPU shader mirror checks all four push fields.
- Vulkan validation passes for fixed, moving, zero budget, active software raster + subtilis,
  missing/partial NPOT mip chains, and full restoration from intermediate tiers. No reported VUID or
  backend error and no failed upgrade in accepted captures.
- NPOT 513x257 sources with one mip use 128x64 floor → generated 256x128 → full 513x257 images;
  two-mip sources reuse their supplied 256x128 suffix. These fixtures take the conservative non-visibility
  path; Bistro separately exercises geometric GPU demand. Both fixture cases reach full quality.
- Disable at frame 80: all 456 optional entries needing restoration reach mip zero by update 313,
  with no jobs left, and the capture continues through frame 420. Small complete-chain floor images
  do not require a redundant restore upload. Re-enable control regression is also green.
- Independent review found and fixed the software-flag overlap and intermediate-restore completion
  issues. A further review fixed memory captures incorrectly succeeding when the sampler failed;
  sampler failure, insufficient evidence and short captures now fail explicitly. Final review has
  no remaining actionable implementation findings.

```powershell
python scripts/measure/measure_progressive_textures.py --exe build-release-dyn-clang64/deren.exe --out tmp/mip-rerun --mips --sweep 2 --frames 360
python scripts/measure/make_mip_residency_fixture.py tmp/mip-fixture
```

`--memory --frames 900` performs a separate WDDM run; Windows CIM access must be available.
`--source-levels 1` or `2` exercises partial source chains, and `--software` also enables subtilis for
the active software lane. Runtime use requires visibility texture streaming configured, plus both
progressive environment flags. Existing optional/upload budgets remain configurable in MiB.

Tracked CSV and compressed evidence accompany this report. Raw captures: tmp/screen-mips-final/;
accepted validation cases: fixed-validation, zero-validation, software-active-v2, npot-single-v2,
npot-partial-v2 and restore-v2 in tmp/mip-validation/. Restricted memory attempts and the earlier
short restoration run are excluded from accepted results. No commit, merge or push was performed.
