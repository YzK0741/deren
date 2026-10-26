# Same-frame visibility texture residency acceptance

Implemented against approved spec2026-10-26-vbuffer-residency-design.md; RHI ABI58, default off.
The current-frame visibility prefix and feedback complete at a graphics checkpoint before loading
required images and recording resolve/lighting/post. Geometry and CPU decoded sources stay resident.

## Validation and review

- Full Release build with14 workers passed. Full CTest71/71 passed.
- Device lane8/8 passed; the latest frame/feedback/window probe153/0 also passed separately with
  validation enabled and no VUID/Validation Error/SYNC-HAZARD/[ERROR] diagnostics. It exercises actual
  GPU single-pixel material bits, software-nearer winner, invalid-item and empty-image feedback,
  checkpoint buffer publication, stable frame snapshot/recorder, rendering-scope refusal,6 window
  frames across both slots,2 checkpoints per frame, final present and swapchain recreation after resize.
- CPU tests cover bounded feedback decoding/demand unions, shared material edges, overlay/MASK pinning,
  disable/restore state and prepared-graph partition ordering. Config tests cover default off and parsing.
- DamagedHelmet selective/eager decoded RGB matched. Bistro far/near/128-degree sweep final decoded RGB
  all match exactly. See ../benchmarks/2026-10-26-texture-residency.md and its results/analysis JSON.

Independent review found2 important defects, both fixed: disabling the mode must schedule full-residency
restoration at the next safe graph checkpoint; eye-dark overlays are excluded from feedback and have
toon_family0, so their textures must be explicitly pinned. Current-frame item bounds were also added.
Integration caught missing device-address allocation for feedback storage and mip0-only descriptor
publication; the corrected full-mip views restore exact Bistro captures. Standalone shader scripts now
include the new shader (test_shader_sources initially failed this parity check, then passed).

## Deliberate implementation choices

- Two prepared segments share one compiled graph schedule and its per-pass barriers/owners, partitioned
  at the named resolve pass. There is no wait inside a pass callback or arbitrary execution cutoff.
  Cross-submission layouts/contents remain exactly those compiled for the unchanged pass order.
- The2052-byte feedback buffer is coherent mapped storage. Its pass epilogue publishes shader writes
  for host reads; completion precedes CPU reads. This avoids an extra copy/readback resource. Graph
  declarations cover its GPU access; host publication is an explicit epilogue.
- Auxiliary toon/overlay/transparent textures and MASK alpha inputs are conservatively pinned rather
  than claiming they appear in main-view feedback. RT-shadow/ineligible opaque frames use full demand.
- Descriptor removal and physical reclamation are separate. Live leases defer unload; no forced
  eviction, group cache invalidation or whole-device wait is introduced in the residency path.

## Limits

Tests prove serialized graphics consumers. No multi-queue texture consumer, source mutation tracking,
device-loss/submit-OOM injection, reverse-camera stress, dynamic-scene rebuild stress, virtual texture,
geometry streaming or measured OS VRAM-release guarantee. Disable restoration is covered by control
state tests and code-path review, not a captured live toggle. Performance figures are single-run smoke
samples with first-load and CPU idle included in elapsed GPU intervals; no stable FPS claim.

Logs: tmp/residency-final-build.log, tmp/residency-final-ctest.log,
tmp/residency-final-device-probes.json, tmp/residency-final-validation/ and tmp/residency-bistro-final/.
No commit or push. Existing unrelated untracked files preserved.
