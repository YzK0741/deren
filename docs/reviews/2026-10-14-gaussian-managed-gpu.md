# Gaussian GPU storage — 2026-10-14

Ten dynamic GPU-frame buffers now belong to a passchain resource registry. Each completed frame slot
owns a resource-only buffer_storage pass, describes its dynamic capacities/flags, and borrows the
resulting snapshot. The runtime supplies its owning device lease. Ordinary reuse skips reconciliation;
sorter requirements, GPU reset/prepare/sort/finalize, barriers and shader layouts are unchanged.

The registry now supports admission before committing or binding a candidate. Gaussian admission
checks actual capacity and byte budget, addresses and coherent mappings. A rejected growth releases
its unused candidate and preserves active storage. This fixes an independently reviewed case where
an address rejection left larger buffers internally retained and a smaller retry underreported capacity.

Reference-only callers must keep the device alive. An already allocated legacy frame cannot gain
an owning device lease through reuse; that transition is explicitly rejected before state changes.
Create a fresh frame with the owning lease instead. Production already supplies a lease initially.

Validation:

- Ownership regression RED1/35 → GREEN42/42. Reviewed edge-case regression RED2/48 → GREEN48/48.
- Release build successful; CTest24/24, 32.87s; actual Vulkan runtime1164/1164.
- CPU and GPU paths each rendered static and rotating flowers for12frames with synchronization validation.
  All four same-path before/after RGBA comparisons have zero differing pixels; raw HDR bytes are identical.
  Existing CPU/GPU differences remain44pixels/static and58pixels/rotation, unchanged by migration.
- GPU maximize/restore regression2/2, clean synchronization, final1280×720 image matches the client.
- Independent read-only review and follow-up found both reported edge cases resolved, no new actionable issue.

Evidence: [captures, logs, configs and SHA256](2026-10-14-gaussian-managed-gpu/).
Large raw HDR captures remain in build-release-dyn-clang64/gaussian-managed-check; their hashes are archived.
PNG archive compression changes only lossless IDAT encoding. Frozen runtime.before includes scene-table migration.

CPU raster frame buffers and immutable point/SH assets still need migration. RT structure/build resources
and shared textures/samplers also remain; the persistent whole-pass goal is active. Changes are uncommitted.
