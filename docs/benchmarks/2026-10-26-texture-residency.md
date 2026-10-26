# Same-frame texture residency smoke acceptance

Date2026-10-26. Same executable and shader build; textured verified Bistro core-MR asset,
1280x720,64 capture frames per case, visibility enabled, DAG/software/HZB disabled. Other settings
match measure_render_optimizations.py's no_optimizations profile: shadows/TAA/SSAO/bloom/GUI off.
One run per arm is smoke evidence, not a statistically stable throughput benchmark.

| Camera | Full texture payload | Physically ready lazy payload | Reduction | Decoded final RGB |
|---|---:|---:|---:|---|
| far |2338695096 B|889541720 B|61.96%|identical|
| near |2338695096 B|668292248 B|71.42%|identical|
| sweep, final sampled state |2338695096 B|808801372 B|65.42%|identical|

Pinned texture payload is141907236 B in all arms. Sweep rotates2 degrees per frame from the near
camera; sampled ready payload varies494228688..931178912 B, proving demand changes/reclamation during
motion. All sampled unload-deferred counters are zero in this matrix. These are actual ready lazy
objects' source payload sums, not VMA block sizes or process VRAM usage. CPU source memory is retained.
No claim that the driver returns the entire payload difference to the OS is made.

The feature trades frame time for residency. First demanded upload/update took371.30 ms far and
319.38 ms near. Subsequent stationary publication/update samples were0.14..0.18 ms, excluding the
checkpoint wait. Sweep upload/update samples after the first varied0.37..124.14 ms. Session GPU
timestamp intervals were24.44→33.28 ms far,23.47→30.69 ms near,22.39→35.63 ms sweep. The selective
intervals include host-wait/upload GPU idle and the first-load bubble amortized over63 timed frames;
they are not isolated GPU rendering cost or steady-state FPS. Do not extrapolate these single runs.

Reproduce:

```powershell
C:/msys64/clang64/bin/python.exe scripts/measure/measure_texture_residency.py --exe build-release-dyn-clang64/deren.exe --asset D:/deren-assets/bistro/textured-verified/bistro-core-mr.gltf --out tmp/residency-bistro-final --frames 64
python scripts/measure/analyze_texture_residency.py tmp/residency-bistro-final
```

The analyzer requires Pillow, checks a complete six-arm matrix and compares decoded RGB pixels,
not PNG file hashes. The generated results.json and analysis.json plus individual configs/logs/PNGs
are retained under tmp/residency-bistro-final. A preliminary run caught a mip-range bug: newly
published views defaulted to mip0; full mip-chain publication restored exact RGB matches in all three
cases. The preliminary results under tmp/residency-bistro are superseded.

Validation-layer correctness is established separately by the GPU feedback/checkpoint probe and the
small scene capture; this performance matrix disables validation. No process VRAM measurement, repeated
performance rounds, resize/recovery fault injection or reverse-camera stress claim is made.
