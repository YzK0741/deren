# Optional asynchronous presentation experiment

The renderer can now overlap a pending host presentation call with next-frame
preparation, through `[render] async_present = true` in `config.toml`. The default remains synchronous.
The measurements below used frozen binaries with the earlier `DEREN_ASYNC_PRESENT` switch.
The current flower workload shows only a small improvement; it does not justify
enabling this by default or claiming that all CPU work now overlaps GPU execution.

## Frame and image ownership

The actual device reports **three swapchain images and two frame slots**. These
are separate counts. The constructor already requests `minImageCount + 1`, within
the surface maximum, so switching the frame ring to three slots is not necessary
to obtain a third presentation image. Three in-flight frames would allow more
queued work and can add latency and memory use; it was not changed here.

The worker owns an immutable snapshot of the submitted frame's swapchain, image
index, present queue and present-ready semaphore. It never reads the subsequent
acquire's image index and never rebuilds the renderer. The next frame writes a
different acquired image, with slot reuse still guarded by the existing timeline.
The queued presentation does not authorize overwriting its image.

The initial experiment incorrectly allowed acquire and present to access the
same swapchain concurrently. Synchronization validation caught the threading
error. Those measurements are invalid and remain separate local scratch data in
`build-release-dyn-clang64/async-present-invalid-wsi`; they are not the results
reported below.

The corrected backend attempts a **zero-timeout acquire of the next image before
starting the worker**, only when the next slot's timeline is already complete.
That image and its slot are cached privately and published by the next frame
open. If no image is immediately available, the normal acquire waits for the
presentation call to finish. This respects exclusive host access to the
swapchain. [Vulkan acquire synchronization requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkAcquireNextImageKHR.html)

The caller holds a shared future and collects its result before the next frame's
queue submission. The backend retains the same completion for teardown. A
persistent worker is created lazily, rather than starting a thread per frame.
The core and VMA uploads share a queue-access mutex. GPU completion remains the
timeline/semaphore's responsibility: completing the future only means the host
present call returned, not that the display finished scanning the image.
[Vulkan presentation semantics](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html)

Recreation and teardown first drain presentation. A prefetched but unused image's
binary acquire signal is consumed by an empty queue submission before device-idle
and reuse. Deferred `out_of_date` is handled on the caller after the current
acquire has been consumed by submission; fatal presentation errors stop before
another submission. A result for a replaced generation cannot trigger another
unnecessary rebuild. Shutdown also checks outstanding presentation errors.

The RHI adds an optional `async_presentation` extension at a new bit. Existing
interface vtables and ABI 32 are unchanged. Backends without the extension keep
the synchronous path. The API core must remain alive until its returned future
has been collected and released, as with its other borrowed capabilities.

## Controlled comparison

Both modes use the same Release executable and backend DLL, with the CPU skin
history fix already present. Only `DEREN_ASYNC_PRESENT=0/1` differs. Flower model,
1280×720, cameras, shaders, Bloom and forced GPU sorting match the established
baseline. GUI, VSync, frame limit, AA and validation are off for timing.

Three repetitions per mode and scenario run serially, with order sync/async,
async/sync, sync/async. Each presents/queues 364 frame requests and records 363
timing rows. Rows 61–360 are selected after warmup: 900 samples per mode and
scenario. Frame times measure frame-request cadence, not monitor scanout rate.
Hardware clocks and desktop activity remain uncontrolled.

| Scenario | Frame median, sync → async (ms) | Frame P95, sync → async (ms) | Median reduction |
|---|---:|---:|---:|
| Static | 2.840 → 2.771 | 3.466 → 3.069 | 2.43% |
| Rotation | 2.911 → 2.850 | 3.237 → 3.123 | 2.09% |
| Near | 3.396 → 3.366 | 3.661 → 3.659 | 0.89% |

| Scenario | CPU begin, sync → async (ms) | Host present/wait, sync → async (ms) | GPU interval-sum median, sync → async (ms) |
|---|---:|---:|---:|
| Static | 0.02 → 2.55 | 2.63 → 0.01 | 2.229 → 2.161 |
| Rotation | 0.02 → 2.61 | 2.69 → 0.01 | 2.200 → 2.198 |
| Near | 0.01 → 3.18 | 3.22 → 0.01 | 2.749 → 2.747 |

CPU phase numbers are medians of 15 rounded 60-frame window means per mode,
not per-frame medians. In async mode the present phase measures caller launch
and future collection, not the worker's full duration. Much of the waiting
appears in begin instead. The static GPU median also changes by about 3%, almost
the entire static frame-time difference. These data do not establish a stable
large throughput gain from async presentation. The specific blocking operation
inside begin has not yet been isolated; attributing it to a particular driver
lock or Vulkan command would be speculation.

## Validation

- All 24 CTest tests pass. The runtime injection test prepares and records the
  next frame while an injected presentation completion remains pending, then
  checks deferred out-of-date and fatal errors. Its 22 checks pass. Forcing the
  synchronous path makes the deferred-fatal assertion fail, so it catches a
  missing future-collection path.
- The real Vulkan backend test passes 1126 checks, including capability identity
  and refusal before a frame is submitted. It also exposed a stale ABI-31 test
  pin, which was updated to the actual existing ABI 32; this change does not bump
  the ABI.
- All 15 sync/async image pairs are pixel-identical. Nine flower HDR pairs also
  match byte for byte. Animated motion-channel and TAA pairs use a 0.02-second
  frame-indexed animation step and frame 1/40/41 captures.
- Animated validation messages and counts match the synchronous baseline.
  Existing motion-channel WRITE_AFTER_WRITE warnings and the frame-1 TAA layout
  warning remain; there are no new swapchain threading reports. Frame-40/41 TAA
  captures are validation-clean.
- A separate 12-frame near-flower CPU/GPU check with async enabled is sync-
  validation clean in both paths. Maximum final channel RMSE is 0.0108 in 8-bit
  units; HDR RMSE is 0.0000235, within the existing comparison gates.
- Maximizing to 2560×1494 and restoring to 1280×720 passes synchronization
  validation and the captured image has no unfilled edge band. Twenty rapid
  resizes also pass, ending at 1050×654.

## Evidence and next step

`docs/benchmarks/2026-10-12-async-present/` retains per-run/pooled results, all
6534 timing rows, logs, representative lossless captures, measurement scripts,
measured executable/backend/source fingerprints and exact measured source text.
The measured runtime is also frozen locally under
`build-release-dyn-clang64/async-present-check/runtime/`. The later addition of
the extension's typed-query identifier and comment cleanup does not alter the
frame algorithm; the final build and tests are checked separately.

Keep the feature opt-in. The next useful investigation is to locate the begin
wait and separate CPU scene preparation from command-buffer/WSI work, then
measure a scene with substantial CPU preparation. Adding another in-flight frame
is not a substitute for that dependency split. No commit or push was performed.
