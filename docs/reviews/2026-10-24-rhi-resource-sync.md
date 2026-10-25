# RHI resource and synchronization expansion

Date: 2026-10-24. Builds on the uncommitted RHI conformance checkpoint; ABI remains 53.

## Contract and decision

Standard sections 4.1-4.3 now explain memory intent versus operation flags, exact initialization and input
lifetime, borrowed mapped spans, host updates/readback, submission acceptance versus completion, tokens
versus presentation, reuse conditions and binding/capability boundaries. Section 6.3 maps these obligations
to headless and device evidence, with explicit coverage limits. Public mapped/token comments agree.

The user approved cache-maintenance-free public buffer mappings. Vulkan explicitly requires HOST_VISIBLE
and HOST_COHERENT on all five host-mapped intents; uniform_cached additionally prefers HOST_CACHED.
VMA CPU_ONLY already required coherence for uniform_cached/readback_coherent, so the missing actual
coherence requirement was on staging_source. Internal upload staging retains its explicit flush path.
No new virtuals, value layouts, barrier guarantees, retention or allocation-error channel were added.

## Verification

- Production allocation-policy bodies are compiled by the existing generated headless fixture. Tests failed
  before the explicit requirements/preference changes (55 checks, 7 failed), then passed (55 checks, 0 failed).
- Device test_submission_completion now mutates storage_coherent, staging_source and uniform_coherent
  mappings after creation, copies to readback_coherent/uniform_cached, publishes host reads and waits for the
  submission token before checking bytes. It passes 75 checks with zero Vulkan validation errors.
- Existing deterministic queue gate still tests pending refusal, retryable timeout, no blocking on later
  submissions, old completed receipts, foreign values and core recreation.
- Full dynamic Release build with 14 workers passes. Full headless CTest: 69/69. Optional device lane: 5/5
  (resource_group 221, texture_group 95, submission_completion 75, render_graph_frame 12,
  gaussian_gpu_frame 65). Device minimum count updated for submission_completion.
- Documentation checks: 429/0. Changed C++ files pass clang-format; git diff --check passes.
- Independent read-only review found no actionable issues.

Local evidence: tmp/rhi-resource-sync-red-test.log, tmp/rhi-resource-sync-green-build.log,
tmp/rhi-resource-sync-full-build.log, tmp/rhi-resource-sync-ctest.log,
tmp/rhi-resource-sync-device-submission.log, tmp/rhi-resource-sync-device-submission-debug.log,
and tmp/rhi-resource-sync-device-probes.json.

## Limits

Allocation-policy checks prove memory-selection requirements; this GPU does not simulate every memory
topology. Coherence removes explicit cache maintenance, not CPU/GPU races or the need for visibility and
completion. Device lane success is not display-completion evidence. Existing factory allocation failure
handling remains unchanged, and unknown enum/flag rejection policy is not invented here.
