# Texture residency performance measurement plan

**Goal:** Execute the user-approved performance comparison of current-frame texture residency on the current working tree.

**Method:** Reuse the existing Release executable and measurement harness. Same build, Bistro core-MR, 1280x720, vbuffer enabled, other optimizations/post effects disabled, validation disabled during timing. No engine modifications or rebuild during the experiment.

**Approved scope:** User approved the proposed baseline matrix, abrupt camera turnaround stress, and long looping motion. Execute directly in this chat.

- [x] Record executable identity, GPU/driver, settings, and measurement limitations.
- [x] Run 3 interleaved rounds of eager/selective x far/near/sweep, 600 frames, 120 warm-up frames.
- [x] Verify complete frame traces, finite intervals, memory samples, active residency logs, and decoded final RGB comparison. Strict RGB gate failed; do not claim image equivalence.
- [x] Run abrupt turnaround stress using the existing deterministic 180-degree/frame sweep (alternating opposite views); compare both modes and multiple endpoint parities. Three rounds, 360 frames, 60 warm-up; separate validation captures at requested frame counts 1 and 2.
- [x] Run long continuous rotation in both modes, inspect per-revolution process memory and frame-time trends. Report observational trends, not a proof of no leaks. One pair, 3600 frames, 120 warm-up, five revolutions.
- [x] Report per-run medians/ranges, FPS, p95/p99, 1% low, maximum interval, first-loop intervals, actual WDDM process memory, and residency upload/deferred-unload evidence.
- [x] Preserve raw evidence and a concise final report under docs/benchmarks; identify unsupported conclusions explicitly.

Frame intervals are host-observed completed-present-loop intervals, not isolated GPU pass time or display scanout timestamps. Initialization before the render loop is separate from first-loop timing. Memory sampling is coarse and may miss short peaks. Final RGB comparisons do not establish every intermediate frame. Camera motion is frame-indexed so geometry is paired despite unequal elapsed times.

## Measurement correction

The first matrix reused the existing harness, which left Bistro's authored animation on the wall clock. Near/sweep final captures differed, including between repetitions of the same mode. The executable exposes `[render] animation_time`; pin it to `0.0` for the controlled rerun and all stress cases. Preserve the initial matrix under `tmp/residency-review-20261026` as diagnostic evidence, not the accepted comparison. Scratch wrappers apply this configuration correction without changing product code or existing harness files. The accepted matrix is `tmp/residency-review-pinned-20261026`.

The pinned first near pair still differs (mean RGB absolute error approximately 0.0085/255; maximum 30). Animation pinning removes a confound but does not establish the cause of the original difference. Record a separate strict RGB gate failure rather than discarding or concealing the observation. Continue requested performance/stress measurements; product root-cause repair is outside this measurement task.

## Final evidence

Report: `docs/benchmarks/2026-10-26-residency-performance.md`. Measurement integrity checked independently for 18 baseline and 8 stress runs. All 71 build artifact hashes unchanged. Four validation-enabled short captures exited cleanly; both first-reveal/return pairs match. Overall final-capture exact RGB comparison: 11/15 identical, strict gate failed; the mode-off repeated captures also exhibit non-determinism. Per-frame traces/configs/memory samples archived in the report's `measurement-traces.zip`; complete raw logs and screenshots retained under the two pinned tmp directories. No engine/shader changes, commits, or pushes for this measurement.
