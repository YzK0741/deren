# Parallel visibility recording

Status: **paused after recording the current state (2026-10-28).** The optimization is opt-in; the
default serial path is unchanged.

The measured target was `scene visibility raster`, whose serial callback was 6.54 ms in the
per-pass trace while every other graph callback was below 0.1 ms. The visibility item ranges now
record into up to eight per-frame-slot mergeable segments, then execute in item order inside the
pass's rendering instance.

- [x] Add opt-in per-pass record tracing and identify the dominant callback.
- [x] Add the serial/parallel visibility path with a default-off switch.
- [x] Keep item order, pipeline/dynamic state, host table writes and recovery raster behavior.
- [x] Create optional segments lazily after the material-import peak.
- [x] Run full CTest, clang-format, Vulkan validation and reversed-order image checks.
- [x] Record serial/parallel performance and the internal phase breakdown.
- [x] Document the remaining costs and pause optimization.

Result: [parallel visibility report](../../benchmarks/2026-10-28-parallel-visibility.md).

Current state: serial/parallel capture averages are 11.710 / 9.721 ms mean frame and
10.085 / 8.130 ms CPU post; parallel validation is clean. The serial image pair is identical and one
parallel capture matches it byte-for-byte; the other shows the known 14,823-pixel capture-order
difference.

Deferred next steps: parallel candidate scan, parallel per-item planning, then the recovery raster.
