# Animation transform invalidation

User accepted the next focused change after common CPU attribution. Follow systematic debugging,
test-driven development and verification-before-completion. Keep amnis policy fixed.

Design: continue sampling every update, including paused clips, morph history and skin publication.
Compare each target's newly composed local matrix (including import shift) with its current local
matrix. Assign and report scene_changed only for actual differences. Use exact matrix comparison:
no epsilon that could accumulate missed motion. Do not cache by time alone: external edits, clip
switches and multiple runtime targets must still be observed. NaNs conservatively remain changed.

- [x] Add regression cases for repeated pinned pose, time advance/scrub, constant animation,
  multiple targets, import shift and external edits; observe failure before implementation.
- [x] Implement the local comparison and verify existing morph/skin tests.
- [x] Review independently and run appropriate CPU/GPU checks.
- [x] Freeze the new bundle and compare clean eager/progressive near/far/moving captures against
  the frozen pre-change bundle, with matching settings, reversed rounds and image checks.
- [x] Report measured changes, remaining CPU costs and limitations for genuinely animated scenes.

The earlier diagnostic source instrumentation is restored already. Its counters/logging perturb
runtime and cannot be used as clean performance evidence. Real transform changes still invalidate
the full BVH and Subtilis history; incremental BVH refitting is outside this change.

Evidence: docs/benchmarks/2026-10-28-animation-transform-invalidation.md, per-run CSV and gzip.
25 captures verified, 266 animation checks pass, five targeted CTest cases pass. Tiny-motion and
NaN-repair cases also pass. No temporary profiling remains in the production renderer.
Final independent acceptance recomputed all six group means, twelve paired improvements, image
metrics and the far GPU increase from retained evidence; no blocking findings remain.
