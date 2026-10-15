# RT mask/skin output ownership — 2026-10-14

Mask and skin bake outputs now declare scene-lifetime GPU-only buffers through resource-only
passchain subchains. The phase borrows their addresses and retains snapshots; BLAS declarations
retain the corresponding generation leases until after AS destruction. Original vertex layout,
build-input flags, shader work and barriers are preserved. Production supplies the owning device root.
Normal frame reuse adds no allocation or reconciliation.

Three failure paths are fixed:

- Mask recording waits until every caster output and BLAS has been prepared. A later allocation
  refusal cannot free a destination already referenced by an unsubmitted mask command.
- Partial AS recording failure keeps the prepared resources alive and leaves the phase unready.
- Failed initial skin recording stops before AS construction, retains potentially referenced
  resources and propagates disable_skin_bake to runtime, matching the existing update failure path.

Validation:

- Failure regressions RED1/312, RED1/316 and RED1/321; final portable AS checks321/321.
- Actual Vulkan AS build/refit/micromap/traversal404/404.
- Final Release build successful; final CTest25/25 in25.84s, including runtime injection.
- RT smoke (14 MASK casters), fixed-time deformation/refit (one skinned caster), and scaled
  MegaLights+RT+TAA each run40frames with a .5-degree sweep. Final RGBA captures are pixel-identical
  to the frozen pre-batch runtime; all synchronization logs are clean.
- Maximize/restore2/2 clean, final client/output1280×720.
- Independent read-only review accepted ownership and failure retention. Its observation about
  initial skin failure propagation was addressed in runtime before the final build and regression.

Evidence: [captures, configs, logs, comparison and SHA256](2026-10-14-bake-resources/).
Comparison selects the newest capture from each scenario; earlier candidate captures are also retained.
PNG compression is lossless. Startup/teardown performance has not been separately benchmarked.

This batch is complete and work stops at the user's request. Changes remain uncommitted.
Micromaps, imported geometry ownership, TLAS scope/publication audit, shared textures/samplers and
the final inventory audit remain; the whole migration goal is not complete.
