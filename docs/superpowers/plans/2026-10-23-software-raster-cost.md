# Software raster cost reduction implementation plan

> Execute inline with executing-plans; the user approved CPU reuse followed by GPU arithmetic optimization.

Goal: reduce software raster overhead without changing routing thresholds or whole-frame hardware fallback.

Architecture: cache same-frame projected records during parallel item planning and reuse them in stable splitting and CSR construction. Then move reciprocal/edge/depth setup from per-pixel work to triangle preparation. Keep exact coverage/ID checks and measured depth tolerance; reject changes that alter accepted output beyond those bounds.

## Tasks

- [x] CPU: add an optional same-camera projection cache to item_records; validate cache size; test cached and uncached split/CSR equivalence and malformed cache rejection. Populate cache in runtime parallel planning and reuse it in the serial CSR pass. Build with 14 workers; test relevant suites; capture CPU/GPU reference and timing before proceeding.
- [x] GPU: precompute edge coefficients and depth plane during triangle preparation. Check coverage, ID, finite values and degenerate handling. Run raw GPU fixture across ring slots and real-scene image comparison; retain the original arithmetic if edge/depth errors exceed acceptance.
- [x] Measure each stage against the corrected 2026-10-23 baseline using unchanged asset, settings, cameras and thresholds. Record executable/shader identities, CPU-active and GPU pass times; do not promise net benefit in advance.
- [x] Full regression and final review; document measured gains and limitations. No commit or push without user direction.

Cache contract: projections belong to the exact records/model/camera for the current frame. Empty cache selects the reference path; a nonempty cache with incorrect length is refused by the CSR builder. This cache is transient, with no cross-frame reuse.

Outcome: retain CPU projection reuse. Reject affine GPU arithmetic for lack of clear performance benefit and a reviewed pixel-edge coverage regression. See docs/reviews/2026-10-23-software-raster-cost.md. Parallel CSR construction remains a separate follow-up, rather than being combined with projection reuse.
