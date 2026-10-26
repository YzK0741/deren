# Image Upload Size Benchmark Plan

> **For agentic workers:** Use superpowers:executing-plans inline, with final independent review.

**Goal:** Measure existing host-image upload versus staging by size/shape before selecting policy.
**Architecture:** Opt-in native benchmark on the backend-created device; independent VMA allocations,
four measured variants and GPU readback checks; CSV summarized by a small Python driver.
**Tech Stack:** Vulkan, vendored VMA, C++23 modules, Python standard library.
**Spec:** docs/superpowers/specs/2026-10-25-image-upload-benchmark-design.md; user explicitly authorized this A/B experiment.

## Constraints and review focus
Preserve uncommitted bounded-file work. No production upload/RHI/default-policy changes or commit/push.
Same final layout and bytes; verify BC block/mip/layer packing and HOST/TRANSFER memory barriers.
Cold means absent staging buffer, not cold driver/OS cache. Warm staging cannot be reused before fence
completion. Validation/readback outside timers; validation-free timings are reported separately.
Report allocation footprint and all samples; do not infer a universal crossover or rendering speedup.

## Steps
- [x] Add opt-in bench_image_upload target plus separate vendored-VMA implementation TU; no headless CTest registration.
- [x] Implement setup/four paths, byte readback checks, CSV timings and warm/cold repeated samples.
- [x] Build -j14 and run validation correctness sweep; fix errors before timed measurements.
- [x] Run repeated validation-off matrix; summarize median/p95, device/driver and known scope limits.
- [x] Independent review and concise documented recommendations; leave policy unchanged pending evidence.

## Execution ledger
This is the user's requested experiment, not a production feature. Using existing pipeline barriers
mirrors VMA's current host strategy. Additional host-compatible staging variant isolates HOST_TRANSFER
image-usage changes. No large binary payloads are written to the crowded C: drive.

Ruling: initial7-case validation passed84 readbacks with no VUID/errors. Add native host layout
transition variant before final measurements; per spec host copies need no subsequent memory barrier
before queue submission. This avoids mistaking the current2-submit implementation for the API limit.

Completed: 24 cases, 12,096 validation-off samples over three rounds, and 1,536 byte readback
comparisons across four sweeps. No skipped cases or reported validation/backend errors. Independent
review found no blocking defect; documented capacity/residency distinction and final-iteration
readback interference, with an exclusion sensitivity analysis. Results and recommendations:
docs/benchmarks/2026-10-25-image-upload/README.md. No production policy changes.
