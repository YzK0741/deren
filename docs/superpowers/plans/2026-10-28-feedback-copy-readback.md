# Feedback copy readback implementation plan

User approved GPU-local feedback writes followed by asynchronous copies to CPU-readable storage.
Use writing-plans, executing-plans, TDD and verification-before-completion. Preserve all existing
uncommitted consuming-import, recorder and documentation work. No commit or push.

## Approved design

Progressive/two-tier feedback gets GPU storage and cached coherent readback buffers per feedback
kind and existing frame slot. An immutable zero-filled staging source resets GPU storage by copy.
After shader writes, barriers publish a copy into cached host storage, followed by host-read
visibility. CPU reads after wait_and_acquire completes that slot, using the existing pending bit.
No added submission/wait, feedback latency, ABI, shader or budget change. Original same-frame GPU
streaming retains its buffer/checkpoint path. Allocation preferences do not guarantee memory type.

- [x] Add internal copy/reset recording helpers and real-device regression for 64 KiB sentinel data,
  zero reset, independent slot snapshots and slot reuse; observe failure before implementation.
- [x] Implement helpers, runtime GPU/readback allocation, reset and publication barriers.
- [x] Build, run CPU regression and real-device validation; check moving progressive, two-tier,
  original GPU streaming and disable-streaming restoration.
- [x] Freeze before/after bundles; capture clean reversed-order near/far/sweep timings, separate
  memory samples and images. Exclude diagnostic runs from performance comparisons.
- [x] Keep only if full-frame benefit holds. Record limits and review synchronization/evidence.

Review focus: first use, copy flags/barriers, completed-slot host reads, pending uploads/restoration,
failure cleanup, original GPU path, resize, source lifetime and final-image-only validation limits.

Ruling: validation exposed a pre-existing consuming-import regression: rectangular mip generation
appended one unadvertised level, so strict lazy image creation failed before rendering on both old
and new binaries. Add a literal-size CPU regression and stop at the declared final level. Apply this
fix to both timing arms, preserving the consuming ownership optimization. Original failed captures
remain in tmp/feedback-copy; final matrix restarts under tmp/feedback-copy-final. The control retains
unused new member slots but executes the original direct-feedback code, isolating transfer behavior.

Verification so far: helper regression RED 101 checks / 11 failures, GREEN 101 / 0 on real GPU;
loader tail regression RED 286 / 12, GREEN 286 / 0. Final full restored-source CTest 73/73 passes
at concurrency 2. An earlier concurrency-6 run hit the unrelated 0.4-second child-start timeout in
test_stacktrace_offline, which passed in isolation and in both subsequent full runs. Six final
scene validation captures (baseline, progressive sweep, restore, two-tier, original GPU, software)
exit zero with zero reported validation errors. Restore reaches full= wanted=456 and jobs=0.
Reviewer confirmed initialization readiness publishes only after success and clear explicitly makes
zero visible to shader reads as well as writes. No confirmed source findings remain.

Completed 2026-10-28: [report](../../benchmarks/2026-10-28-feedback-copy-readback.md).
Final 21 captures: seven validation, two memory, twelve timing. Two reversed-order rounds show mean
frames near 11.919->9.959 ms, far 12.050->10.112 ms, sweep 12.862->10.645 ms (16.1-17.2% lower).
Moving GPU mean increases 0.465 ms but full frame improves 2.217 ms, so the implementation is retained.
Device-local steady allocation +64 KiB, total live resource allocation +196 KiB, VMA host reservation
+32 MiB; no claim that reserved bytes are live VRAM. Fixed final images and restore/eager match exactly.
Both moving arms produce the same two final images in opposite rounds; underlying small renderer
nondeterminism remains unresolved. Final read-only reviewer independently verified CSV/gzip/hash/image
and test claims with no material findings. Final targeted checks 5/5 and real-device checks 101/0 pass.
