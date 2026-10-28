# Private Morton Int128 integration, 2026-10-28

## Production boundary

Boost.Int128 is now a private BVH implementation dependency, pinned at
`16f0b3374470e3821c10277775de18cf320cda45`. The global module fragment includes a private helper
that returns `std::array<uint8_t, 12>`. The utility module exports no Boost type, general integer
alias or new encoder function. Morton storage, node layout, quantization scale 2^21 (including 1.0),
little-endian bytes, lexicographic comparison and internal-node code computations are unchanged.

The independent vendor subtree contains 33 upstream headers and their license, copied from exact
Git blobs, plus a SHA-256 manifest and provenance instructions. It adds about 436 KiB of upstream
source and no new runtime DLL. CMake supplies its SYSTEM include root privately to static_utility
and the encoder test. Existing Boost.Graph headers are unchanged; builds do not download either
dependency. See [vendor provenance](../../source/third_party/boost_int128/README.md).

The [earlier isolated benchmark](2026-10-28-morton-int128.md) motivated the change. Its 24x encoding
speedup is not a renderer FPS claim. This report validates production integration separately.

## Checks

- Private helper missing-header RED observed before implementation. Permanent `test_morton`
  checks an independent bit-by-bit oracle against 100000 deterministic random inputs plus all axis
  bits, saturated values, byte boundaries and quantization endpoint 2^21. Compile-time assertions
  preserve constexpr use, and sorted bytes explicitly preserve the old ordering.
- Release encoder test: 100161 checks, zero failures. ASan/UBSan encoder test passes. The real BVH
  utility test additionally compares 1024 generated AABBs with direct frustum membership from
  three views, checking duplicate/missing results.
- Full Release build and 75/75 CTest pass. The first suite exposed a missing CI target entry;
  `test_morton` was added to the existing workflow, then the complete suite passed.
- Offline vendor verification passes; deliberately corrupting a copied header is rejected.
- A plain utility-module consumer compiles. Separate attempts to name `boost::int128::uint128`,
  `deren::utility::int128_t`, or the private helper through module import are correctly rejected.
  Test code reaches the helper only through its explicit private header/include dependency.
- Independent review found an LP64 narrowing issue in the random-input test and a misplaced vendor
  verification command. Explicit integer casts and the two correct dependency commands resolve them.
  Linux/MSVC execution is not claimed by these local Windows Clang64 checks.

## Renderer capture protocol

Before: frozen `tmp/animation-invalidation-final-bin/`, including the accepted paused-pose
invalidation fix and GPU feedback-copy path. After: `tmp/morton-production-final-bin/`.
Both use textured Bistro, progressive mip residency, DAG+HZB, 1280x720, 1 px error, hysteresis off,
no software raster/shadows/post effects/GUI/vsync/frame cap. Real `subset_0.174Action` animation
plays with `animation_time=-1` and `--capture-animation-sweep 1/60`: deterministic animation time
advances every presented frame, rather than following wall-clock execution speed. Near/far/sweep
are tested in two reversed rounds; camera sweep is 2 degrees/frame. Four additional near controls
pin the pose at zero, two per arm. Two separate 130-frame animated validation runs precede timings.

This totals eighteen captures: twelve animated timing runs, four pinned controls, two validation
runs. Timing runs have no validation layers, temporary counters, operation profiler or memory
sampling. No builds/tests overlap captures. Frame mean/P95 use the harness's 301 intervals after
dropping 60 from a 360-frame capture; CPU phases use post-warmup 60-frame windows; GPU means cover
the entire session. These overlapping measurements are not an additive CPU/GPU decomposition.
P95 group values are means of per-run P95s, not pooled percentiles. Only two runs per arm/view.

The temporary measurement harness is derived from the existing progressive-texture harness, with
an animation-step argument and frame-indexed clock. Its initial relocated copy missed the parser
module search path and produced no summary; `tmp/morton-production/` is excluded. That Python
import was corrected before all formal `tmp/morton-production-v2/` captures.

## Results

Two-run means. `begin` is the CPU scene/build phase, the phase expected to contain any real BVH
construction change; the pinned control holds the pose at zero and should therefore see no
construction benefit.

| View | Frame before / after | Delta | Reduction | Begin before / after | GPU session before / after |
|---|---:|---:|---:|---:|---:|
| near, animated | 11.299 / 9.591 ms | -1.708 ms | 15.1% | 2.450 / 1.475 ms | 4.28 / 4.49 ms |
| far, animated | 10.813 / 10.097 ms | -0.716 ms | 6.6% | 2.203 / 1.520 ms | 8.395 / 8.295 ms |
| sweep, animated | 10.308 / 9.429 ms | -0.879 ms | 8.5% | 2.122 / 1.348 ms | 6.00 / 5.755 ms |
| near, pinned pose | 8.977 / 9.119 ms | +0.142 ms | -1.6% | 0.524 / 0.528 ms | 4.14 / 4.23 ms |

The pinned control shows no begin-phase benefit and a small frame-time regression within this
two-run sample, which is the expected control result: the encoder is not on the no-rebuild path.
The animated views reduce begin by 0.68-0.98 ms; their frame deltas are larger and include unrelated
post/scheduling variation, so the frame reduction is an end-to-end observation, not the encoder's
isolated cost. No isolated encoder timer was collected in these production captures. GPU session
time is effectively unchanged within the single-capture noise; this is not a GPU optimization.

Final images: far and pinned pairs are pixel-identical. Near has one forward round pair at 14,823
pixels (MAE 0.008548/255, max 30, 12 pixels above 8) while the other forward pair is exact. Sweep
shows 14,442 pixels (MAE 0.008528, max 35, 24 above 8) for both forward pairings, while its
same-arm cross-round pairings are exact. This remains the known
capture-order/renderer nondeterminism, not evidence of an int128 encoding mismatch; the byte-level
oracle test is the correctness gate.

## Evidence

- [Per-run capture CSV](2026-10-28-private-morton-int128.csv)
- [Compressed settings, manifests, frame data, source hashes, checks and reproduction scripts](2026-10-28-private-morton-int128-evidence.json.gz)
- Local capture driver/analyzer: `tmp/measure_morton_production.py`, `tmp/analyze_morton_production.py`.
- Dependency extraction/verifier: `scripts/tools/vendor_boost_int128.py`.

No commit or push is part of this task. Amnis policy remains unchanged. Comparison-order changes,
64-bit keys, generic utility integers, BVH refitting and node-allocation changes remain separate work.
