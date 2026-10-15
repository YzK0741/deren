# Gaussian CPU upload storage — 2026-10-14

CPU raster frame instance/reference buffers now belong to a resource-only passchain subchain.
Each completed slot describes two coherent storage/address buffers, borrows their handles and retains
a resource snapshot. Normal reuse skips reconciliation and GPU allocation. Sorting, projection, SH,
upload payloads and the nonowning returned reuse pointer keep their existing contracts.

CPU results are staged until mappings, capacities and addresses pass admission. Failed growth preserves
the previous frame's CPU results, mapped bytes, resource snapshot and draw parameters. A frame slot keeps
two reference/owner vector workspaces; successful publication swaps them, and RAII returns the old or
rejected workspace without freeing its capacity. After warmup, stable clouds avoid full-cloud vector churn.
The second workspace adds one retained CPU reference-vector capacity per slot.

Production supplies core_owner initially. Reference-only callers retain the core themselves; cross-core
reuse and promotion of an allocated legacy frame to an owning lease are rejected before mutation.
Device/asset roots outlive subchain completion drain. A retained managed snapshot retains the device.
CPU byte_size remains the size of this frame's upload payload, preserving its existing reporting semantics.

Validation:

- Ownership regression RED1/161; expanded suite GREEN192/192. Covers mapped payloads, stable reuse,
  rejected growth preserving old CPU/GPU data, successful growth, snapshot/device retention, foreign core,
  lease transition and empty-frame reuse. GPU frame48/48 still passes.
- Release build successful; CTest24/24 in27.77s; actual Vulkan runtime1164/1164.
- Static/rotation flowers: CPU+GPU12frames each, clean synchronization. All four same-path before/after
  RGBA and raw HDR comparisons are identical; existing CPU/GPU differences remain unchanged.
- CPU maximize/restore2/2, clean synchronization, final1280×720 client/output match.
- Read-only review found workspace churn; retained staging workspaces fix it. Follow-up review found no new issue.

Evidence: [captures, configs, logs and SHA256](2026-10-14-gaussian-managed-cpu/).
The frozen baseline executable hash matches the preceding GPU batch's final captures, reused here as baseline
evidence. Large HDR files remain in build-release-dyn-clang64/gaussian-cpu-managed-check, with archived hashes.

Immutable Gaussian point/SH asset uploads are next. RT build resources and shared textures/samplers remain.
The persistent whole-pass goal is active; changes are uncommitted.
