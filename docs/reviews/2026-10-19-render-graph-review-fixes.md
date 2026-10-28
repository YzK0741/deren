# Render graph follow-up fixes — 2026-10-19

Scope: four findings from the current graph/ownership review, followed by a subtilis design refinement. No commit or dependency import in this batch.

## Changes

1. **Shared ownership:** replaced the `dropping` flag protocol with one implicit weak reference held by the strong group until object destruction finishes. This closes both concurrent last-strong/last-weak deletion windows and keeps self-observing destruction safe. RHI ABI is now **46** because old inline ownership operations cannot share the new control-block protocol; GUI ABI remains 5. Rebuild host and backend together.
2. **RT input stages:** the RT adapter declares surface/depth sampling at `ray_tracing`, matching the actual ray-generation consumer. An adapter-level test checks all four geometry-to-RT input barriers.
3. **GUI handoff:** composite, FXAA and upscale insert the color-attachment write → read/write dependency before the separate LOAD rendering instance. Barrier, begin-rendering and execute errors propagate through recording. Callbacks stay installed before GUI initialization; the optional live `has_recording` predicate skips empty overlays after each frame's recording.
   - The role-barrier backend now accepts registered, current frame snapshots only in the acquired frame primary, matching scoped synchronization. It checks ownership and acquire epoch before native access; reusable buffers cannot retain this dependency. A real-device test reproduced rejection of valid snapshots, and a second regression test reproduced incorrect acceptance in reusable buffers before the final guard.
4. **Inheritance lifetime:** `overlay_begin` copy/move constructors and assignments rebind its two internal pointers to its own storage. Correctness no longer depends on NRVO.

## Regression evidence

- Ownership race: the new 30,000-iteration concurrent-release test reproduced an ASan double-free before the fix; it passes afterward. Also covers lock, expiration and self-observing object destruction.
- RT stage test: four destination-stage checks failed before the declaration correction, then passed.
- Overlay value lifetime: five explicit copy/move pointer checks failed before rebinding, then passed.
- Overlay callback registration: seven checks failed with the initialization-time guard; always registering plus a live recording predicate fixes them. The handoff test covers barrier ordering, barrier failure, callback failure, and false → true recording availability.
- Release full build (`-j12`) succeeded; CTest **50/50 passed**.
- ASan relevant targets rebuilt (`-j12`); CTest **7/7 passed**: ownership, pass, graph lighting/post/execute/compose and runtime injection.
- The added frame test also rebuilt and passed under ASan (1/1); together, eight distinct relevant tests passed. Its optional real-device mode passed 11 checks, including reusable-command-buffer refusal.
- Full render gate: 15/15 scenarios deterministic and identical to the checked-in `docs/reviews/2026-10-17-render-graph-gate/legacy-hashes.txt`; references unchanged. The script's older built-in `--frozen` set gave 1/15 matches, a pre-existing stale baseline also documented in the graph review. Verdict uses the explicit recent file comparison (`review-fixes-gate-comparison.log`).
- Windowed GUI, direct/FXAA/upscale: 8 frames each with synchronization validation, fresh screenshots and no recording refusals or validation findings; the direct screenshot visibly contains the panel. Headless capture suppresses GUI and is not counted as GUI coverage.
- RT device acceptance passed: both TLAS slots, hit/miss/MASK traversal, 14 baked MASK casters, 40 frames, clean validation.

Logs are local ignored artifacts under `build-release-dyn-clang64/review-*` and `build-asan/review-*`; red/green ownership logs include the original sanitizer diagnostic.

## Independent review

Read-only review found an initialization-order regression in an intermediate GUI callback guard; it was reproduced and repaired before completion. Re-review found no further callback issue. A final backend review requested the same frame-primary restriction already enforced by scoped synchronization; the role path now preserves it. Ownership protocol, ABI change, RT scope and pointer rebinding had no further concrete findings.

subtilis review identified the existing toon depth-EQUAL re-shading consumer. The proposed M1b now keeps these items at LOD0 until that consumer can use the same cut.

## subtilis

The existing [design](../superpowers/specs/2026-10-18-subtilis-design.md#11-2026-10-19建议的轻量实现路线) now proposes M1a (LOD0 visible-run bridge), M1b (full-resident static opaque hierarchy), M2 (GPU selection/finalize), then Hi-Z. It specifies complete publication/fallback, per-item and ID capacity, worst-cut budget, boundary preservation, error metric/transform handling and separate shadow/BLAS policy. This is a proposed implementation route, not shipped geometry functionality.
