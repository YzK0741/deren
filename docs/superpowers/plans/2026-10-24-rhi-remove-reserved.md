# Remove reserved RHI interfaces

User approved removal of query/create_query, create_swapchain, and the old mesh_shader dispatch extension.
Use inline execution; preserve existing uncommitted work. Keep frame_swapchain, GPU timing and tier-1 mesh
commands. Bump ABI to 54, keep retired enum bit 0x4, exclude it from active capability enumeration.

- [x] Observe compile-time removal/ABI assertions fail before changing the contract.
- [x] Remove declarations, no-op overrides and stale current-standard clauses.
- [x] Verify ABI mismatch rejection, active capability enumeration and existing rendering paths.
- [x] Build at 14 workers, run CTest/device probes, independent read-only review.

Completed: ABI 54 build, CTest 69/69 and device lane 5/5 passed. Independent read-only review found no important issues.
