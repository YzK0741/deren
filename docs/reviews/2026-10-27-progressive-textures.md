# Progressive texture residency review

Independent read-only source review of the uncommitted implementation against
`docs/superpowers/specs/2026-10-27-progressive-textures-design.md`.

Findings fixed:

1. Continue polling and scheduling full restoration after streaming is disabled, rather than clearing
   the restore flag after starting the first two uploads. Device probe disables at frame 8 and confirms
   all 485 optional sources restored by update 260 with zero pending jobs.
2. Cancel restoration on re-enable so the normal configured budget is restored. The new control
   regression failed before the change and passes after it.
3. Migrate the active unlit fragment shader to the current frame's texture bank. Its push fields match
   host offsets 96/100/104, checked in compiled SPIR-V; fallback/unlit Vulkan validation succeeds.

Final reviewer verdict: no remaining actionable source-level findings. Frame-bank ownership,
completed-slot feedback, bounded uploads, shutdown joining and the default GPU baseline were reviewed.
Source review did not claim device correctness or performance; those are covered by the separate
measurements and validation report. Full CTest: 72/72 pass (symbolic-link permission required for the
backend-boundary case). No commit/merge/push performed.
