# subtilis buffer-transfer consumer checkpoint

Date: 2026-10-25. RHI ABI remains 54. This checkpoint migrates the immutable subtilis vertex/index
pair, including runtime calls using the default upload mode; it does not change per-frame rendering.

## Behavior

`geometry_upload_mode::automatic` uses one independent graphics transfer batch on an owning core with
submission and scoped synchronization. Missing capabilities select the existing synchronous creation
path. Explicit `synchronous` and `batched` modes support diagnosis/comparison; forced batching without
capabilities is refused before resource creation. Upload/submit/wait errors propagate without retry.

The resource chain creates two allocation-only generations. Owning aliases retain the generations and
core through transfer completion. Both payloads are copied in one submission; the upload function waits
for that token before publishing the result, preserving ready-on-return behavior. Available vertex-input
and shader readers receive visibility. The Vulkan AS build verb retains its transfer-write pre-barrier.

The batched path bypasses native initial-content deduplication between separate live uploads. Share the
returned uploaded_geometry owner when sharing an asset. Existing resource-chain destruction may still
wait for device idle; this change does not promise lifecycle-wide idle removal or asynchronous streaming.
Staging is still allocated per operation. Frame screenshots and frame completion are unchanged.

## Local upload latency

Release dynamic Clang64, NVIDIA GeForce RTX 4060 Laptop GPU, driver 617.42. Three sequential process
runs, each with four warmups and twelve measured calls per mode and size. Mode order alternates.
Validation is disabled for the timed core. CPU hierarchy construction and output destruction are outside
the timer; each result is released before the next upload, avoiding simultaneous identical-content cache
hits. The timer includes allocation, staging, command recording, submission and wait to ready return.

| Combined vertex/index bytes | Synchronous us | Batched us | Reduction |
| --- | ---: | ---: | ---: |
| 18,464 | 116.90 | 87.25 | 25.4% |
| 1,085,304 | 238.50 | 216.65 | 9.2% |
| 4,145,300 | 558.85 | 531.40 | 4.9% |

Entries are the median of the three per-process medians, not a pooled sample median. Paired reductions
across processes range approximately 14-26%, 5-14%, and 4.9-5.6%, respectively. These are warm grid-asset
upload measurements on one machine, not cold-start, large-scene, cache-sharing or FPS results. The
benchmark does not request AS-input usage, while the correctness probe does when ray-query is available.
Raw logs: tmp/subtilis-transfer-benchmark.log, -2.log and -3.log.

## Verification

- Forced-batch capability test observed failing before implementation; CPU upload probe now 72/0.
- Complete Release dynamic Clang64 build with 14 workers passed: tmp/subtilis-transfer-full-build.log.
- Full CTest: 69/69, tmp/subtilis-transfer-ctest.log.
- Optional device lane: 7/7, counts 221/95/75/12/65/65/77; JSON in
  tmp/subtilis-transfer-device-probes.json. New probe validates synchronous/batched/automatic byte-exact
  vertex and index readback and upload submission counts. On this GPU all three modes also create/build
  a BLAS after readback; this checks AS-input compatibility, not ray-traced hit results.
- Archived device debug log: tmp/subtilis-transfer-final-device-debug.log, zero VUID/validation errors.
- Documentation checks: 439/0; changed upload/probe sources pass clang-format and git diff --check passes.
- Independent read-only implementation review found no blocking issue. Nonblocking gap: no consumer-level
  injected upload/submit/wait failure test; no injected device loss, exhaustion or concurrency coverage.

Next consider another measured consumer or staging reuse only if allocation/submission profiles justify it.
The generic registry and frame completion contracts do not need expansion for this consumer.
