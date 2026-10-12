# Gaussian raster cost optimization

Approved scope: profile raster work, reduce repeated projection/color work or wasted fragment coverage according to the measurements, preserve current image thresholds, and retain only a measured improvement. Execute inline in the existing checkout: prior GPU changes and model orientation are prerequisites. No commit/push. Stop and record progress when remaining account quota reaches 3%.

- [x] Capture a current GPU baseline with the new bouquet orientation; use identical camera/config and 60 warmup + 300 timed frames.
- [x] Compile isolated diagnostic raster variants under the ignored build directory: unchanged vertex with fragment discard, and unchanged projection with constant SH color. Compare raster median/p95 in static and rotating runs. These variants intentionally change images and must never enter normal output.
- [x] Choose one optimization from the observed cost; document memory and arithmetic tradeoffs before implementing. Add a failing contract/resource test if a new cache is needed, then implement the minimal change and verify GREEN.
- [x] Run native Gaussian correctness probes with synchronization validation, CPU/GPU flower image acceptance and before/after GPU timing with repeated runs. Include additional memory in capacity limits and per-slot retention if caching is selected.
- [x] Keep the change only with repeatable measured benefit and accepted images. Run relevant tests/format/SPIR-V checks, review the diff, and save a report. If the quota stop is reached first, preserve a resumable ledger instead of declaring completion.

Outcome: 48-byte float32 projection/color cache retained. Gaussian GPU interval total improves 14-28% in six pairs; frame-time results vary, so no equivalent FPS claim. Six baseline/cache final PNGs match exactly; an additional HDR capture is byte-exact. Full tests, native probes, rapid resize and independent review pass. Details: [acceptance report](../../reviews/2026-10-12-gaussian-projection-cache.md).
