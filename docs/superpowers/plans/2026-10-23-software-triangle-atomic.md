# Triangle-driven software raster prototype

User approved the UE-inspired prototype: a workgroup handles a software cluster, lanes rasterize triangle bounding rectangles, and a private uint64 atomic buffer keeps depth and ID together. Normal Z uses atomic min on `(depth_bits << 32) | packed_id`, preserving nearest-depth/minimum-software-ID ordering. Empty is uint64 max. Existing resolve still favors hardware on equal depth.

## Implementation

- [x] Expose enabled buffer-int64-atomic capability through RHI, default false for other implementations. Unsupported or failed preparation restores the complete hardware run before routing is published.
- [x] Add optional triangle pipeline and per-frame private scratch buffer. Clear, raster, and unpack in one pass with explicit internal barriers. Scratch is private to this pass; graph-visible outputs stay the same two software images.
- [x] Opt in through `DEREN_SOFTWARE_RASTER_TRIANGLES=1`. Keep old software pipeline for A/B. First reuse existing CPU classification/CSR construction, but dispatch a unique cluster list rather than tile entries. This isolates the GPU change; removing CPU CSR construction follows only after validation.
- [x] Validate paired atomic packing, unique dispatch list, both frame slots, same-depth candidates and image output. Build with 14 workers and run appropriate regressions.
- [x] Measure the same textured asset/cameras/settings in three rounds; compare GPU pass/frame and CPU-active cost. Do not promote the prototype based on pass time alone.
- [x] Final independent review and evidence report. No commit or push.

The prototype initially retains the current floating-point coverage convention. UE fixed-subpixel/top-left coverage and unique-vertex caching are separate follow-ups; do not silently change those contracts in the dispatch experiment. Fresh diagnostic prefixes are required. Failed capability/allocation/descriptor/pipeline preparation must be detected before shortening hardware runs.
