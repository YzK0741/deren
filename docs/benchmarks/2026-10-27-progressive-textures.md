# Two-tier GPU-feedback texture residency — 2026-10-27

Subsequent intermediate-mip and screen-demand results, including actual WDDM memory sampling, are
recorded in [the next increment](2026-10-27-screen-mip-residency.md). This report preserves the two-tier baseline.

The first ranked increment is implemented on `codex/progressive-texture-residency`, default off.
Low-resolution textures remain resident while completed-frame GPU feedback schedules bounded
asynchronous full-resolution upgrades. Rendering continues with the current camera. No frozen-view
cache, CPU HZB, virtual texture pages or target-mip feedback was added.

## Results

RTX 4060 Laptop GPU, Release, Bistro core MR, 1280x720, shadows/TAA/SSAO/bloom/FXAA off,
animation time fixed at zero, offscreen capture. Each performance run presents 360 frames; omit the
first 60 frame intervals. Values below are measured wall-clock presentation intervals, not isolated
GPU timestamps. No compiler/test/GPU workload ran concurrently with these performance runs.

| Camera / path | Mean ms | P95 ms |
|---|---:|---:|
| Fixed, same-frame GPU feedback baseline | 32.369 | 33.892 |
| Fixed, progressive | 20.663 | 22.152 |
| Fixed, fully preloaded reference | 20.355 | 21.770 |
| 2 degrees/frame sweep, GPU baseline, round 1 | 51.518 | 137.731 |
| Sweep, progressive, round 1 | 20.394 | 22.559 |
| Sweep, progressive, round 2 | 20.383 | 22.538 |
| Sweep, GPU baseline, round 2 | 50.635 | 123.113 |

The two sweep rounds reverse mode order. These are preliminary measurements on one laptop, without
fixed clocks/thermal state. The speed comparison includes a quality tradeoff: the baseline waits for
current-view full textures, whereas progressive renders a real low-resolution texture when an upgrade
is unavailable. It is not an equal-quality performance win.

## Quality and residency

- Fixed view requests 98 optional full textures. All become ready by update 102; later frames start
  no additional uploads. Final output is pixel-exact against the fully preloaded reference.
- The same-frame GPU baseline differs from that fixed reference in 14,809/921,600 pixels, RGB MAE
  0.00854/255, maximum channel difference 30. This small baseline discrepancy is not explained here.
- Both moving progressive terminal frames differ from the moving preloaded reference in 10.051% of
  pixels, RGB MAE 0.41469/255; 1.726% of pixels have a maximum channel difference above 8. The two
  progressive terminal frames match each other exactly. They are not claimed pixel-exact to full quality.
- Across steady sweep updates, 66.47% of requested optional texture entries use their fallback.
  This is an unweighted texture-demand count, **not a pixel coverage or missing-texture percentage**.
  It shows that upgrading whole images with two jobs does not keep up with this camera path.
- With optional budget zero, all 98 requested optional textures remain low resolution, no upload job
  starts, and rendering/validation succeeds. The image remains textured but differs substantially from
  the full reference (RGB MAE 4.599/255).
- Default optional budget is 512 MiB; pending-upload budget is 128 MiB and there are at most two jobs.
  Normal measured runs respect those bounds. Actual peak pending payload is 10.667 MiB.
- The compulsory low + pinned-full texture floor is 164.333 MiB in this asset. Fixed progressive adds
  502 MiB optional payload, for 666.333 MiB of referenced texture payload, versus the baseline's logged
  637.333 MiB. Low fallback therefore adds memory at a fully converged fixed view.
- Sweep maximum referenced optional payload is 325.333 MiB, plus the 164.333 MiB floor. Retired images
  are retained by older frame banks; `retired_bytes` records this extra ownership separately.

Payload figures are logical texture-byte accounting. They exclude IBL, frame targets, allocation
alignment, VMA retained blocks and driver memory; they are **not measured WDDM VRAM**. Low images can
also be physically deduplicated by the backend, so logical accounting can overcount physical storage.

## Implementation and remaining cost

Generate an at-most 128x128 RGBA8 image at registration, preserving aspect ratio and source edges;
sRGB color filtering runs in linear space. Keep the original full mip chain as immutable source bytes.
Alpha-sensitive, transparent, toon and overlay textures retain the full-resolution pinning policy.

Each completed frame slot owns its feedback buffer and texture descriptor bank. Only that slot's
bank changes, and its image owners survive until the slot completes. Workers create full images;
the render thread only polls futures and publishes completed images. Failed upgrades retain their
low image. Workers are joined before the final GPU idle wait.

Selection prefers current demand, already resident data and recency, with a 30-update retention window
for invisible resident textures. This is a first budget policy, not UE's complete mip-priority system.
Disabling streaming continues asynchronous work until full restoration finishes, lifting the optional
budget intentionally. Re-enabling cancels restoration and returns to the configured budget.

The progressive path does not call the mid-frame feedback checkpoint. However, the backend uploader
still submits and waits for two per-image barriers on the graphics queue, from background workers.
Those queue costs still exist. Fixed-view convergence takes roughly 100 updates despite only 98
upgrades: improving upload publication and requesting intermediate mip levels remain useful next work.
Fallback generation also adds scene-load CPU work; scene-load latency was not benchmarked here.

## Verification

- Full build succeeded. All **72 CTest cases pass**. The backend-boundary test needs Windows symbolic
  link permissions; its first restricted run failed with WinError 5, and the final complete run passed
  with that permission. Log: `tmp/progressive-ctest-all-green.log`.
- New fallback/budget tests: 16 checks. Residency toggle regression: observed failure before the
  re-enable fix, then 232 visibility checks pass.
- Vulkan validation succeeds for moving 64-frame captures, zero-budget fallback, restoration while
  uploads are in flight, and non-visibility/unlit fallback. All final captures exit zero with one image,
  no reported VUID/backend error and no failed upgrade.
- Disable at frame 8 with initial optional budget 64 MiB: all 485 optional source textures restored by
  update 260, no pending jobs, and later frames keep rendering. Restoration intentionally exceeds the
  optional budget because streaming has been disabled.
- Independent source review found and fixed restore polling, re-enable budgeting, and unlit shader
  bank addressing. Final review has no remaining actionable source-level findings. SPIR-V confirms
  unlit frame/image/spare push fields at offsets 96/100/104.

## Reproduce and inspect

Enable with `DEREN_PROGRESSIVE_TEXTURES=1` and `[render] visibility_texture_streaming=true`.
`DEREN_PROGRESSIVE_BUDGET_MB` and `DEREN_PROGRESSIVE_UPLOAD_MB` override memory budgets (MiB).
`DEREN_PROGRESSIVE_TRACE` writes the residency CSV. Budgets do not include the compulsory floor.

```powershell
python scripts/measure/measure_progressive_textures.py --exe build-release-dyn-clang64/deren.exe --out tmp/progressive-rerun --mode progressive --sweep 2 --frames 360
```

Use `--mode gpu` for same-frame feedback, `--mode eager` for full preload, `--budget 0` for low-only,
`--validation` for correctness runs, and `--disable-streaming-frame 8` for restoration. The runner
defaults to the local Bistro asset path; override `--model` on another machine.

Tracked summary: `2026-10-27-progressive-textures.csv`. Raw final runs, logs, commands, binary hashes,
frame/residency traces and screenshots: `tmp/progressive-final/`. Initial development validation runs
are outside that directory and excluded from the performance table. `image-comparison.json` records
the full-frame comparisons. The final unlit correction changes only `unlit.frag.spv`; performance
captures used PBR/visibility shaders, whose binaries did not change with that correction.
`2026-10-27-progressive-textures-evidence.json.gz` preserves all 11 final runs' text evidence,
screenshot hashes, image comparisons, shader hashes and build/test logs in the repository.
