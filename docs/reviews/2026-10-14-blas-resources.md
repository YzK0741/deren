# BLAS creation ownership — 2026-10-14

Each nonempty bottom-level entry now declares a scene-lifetime AS through a resource-only passchain
subchain, retains its snapshot and borrows the handle. The owned geometry vector supplies a temporary
pointer only during synchronous RHI creation. Refit flags, indexed/unindexed geometry, micromap pointers,
empty-geometry index preservation and build/refit recording remain unchanged. A new engine id is appended;
RHI ABI32 and existing heap/id values are unchanged.

Requests can retain explicit shared dependency leases. Generation objects release the AS before those
dependencies and the device root. Dependency/geometry identity participates in compatibility; all RHI
geometry fields are compared individually. Production RT forwards its owning core to BLAS creation.
Mask/skin/micromap resources are still phase-owned and will supply their managed leases in the next batch;
caller-owned imported geometry must still remain valid when a build/refit reads it.

Moving entries transfers subchain ownership without idle waits. Ordinary build/refit adds no registry
work, allocation or wait. Each owned entry drains at final destruction; this adds per-entry drain calls
during teardown, outside normal rendering. A retained snapshot keeps explicitly supplied dependency/device
roots alive. Startup/teardown cost has not been separately benchmarked.

Validation:

- Ownership RED1/280 → expanded GREEN310/310. Covers copied geometry, allow_update, skipped index,
  entry-vector movement without drains, allocation failure, explicit dependency/snapshot/device retention.
- Actual Vulkan build/refit/micromap/traversal393/393; runtime1164/1164.
- Release build successful, CTest25/25 in26.09s.
- RT smoke, scaled MegaLights+RT+TAA and fixed-time animated skin/refit:40frames/.5-degree sweep each,
  old/new RGBA identical, synchronization validation clean.
- Scaled MegaLights+RT+TAA maximize/restore2/2 clean,1280×720 client/output match.
- Independent read-only review found no new issue.

Evidence: [captures, configs, logs, scripts and SHA256](2026-10-14-blas-resources/).
Frozen runtime.before includes TLAS migration. PNG compression changes only lossless IDAT encoding.
Mask/skin outputs, micromaps, shared textures/samplers and final ownership/publication audit remain.
The whole-pass goal is active; changes are uncommitted.
