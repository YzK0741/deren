# Gaussian immutable asset storage — 2026-10-14

Geometry and SH buffers now belong to a scene-lifetime passchain subchain. Two appended engine ids
describe their device-wide storage; RHI ABI32 and existing heap/id values remain unchanged.
The uniform_gpu_only staging-upload intent plus storage/address flags remain unchanged.
Candidate capacities and addresses pass admission before borrowed handles and metadata are published.

Owned upload aliases retain packed geometry/SH through synchronous creation. Generation metadata and
the producer then release those aliases, retaining only separate content keys and byte counts. Production
transfers its packed asset to the shared upload overload and moves CPU geometry into raster_asset after
upload returns; it creates no additional packed-array copy and retains no duplicate SH upload data.
The legacy const-reference overload validates before making its owning packed copy; it is a convenience
path with an extra temporary copy/validation. Reference-only device callers still keep the device alive.

Runtime create_gaussian_asset supplies core_owner from initial creation. Device roots outlive the asset
subchain's drain and allocations; a retained storage snapshot independently keeps the device alive.
The radix sorter remains a frame-owned pipeline service, drained before its owner releases it.

Validation:

- Asset ownership RED1/394 → GREEN404/404, including uploaded bytes, source release, retained snapshot/device,
  missing source, wrong device lease, invalid addresses/capacities and complete candidate cleanup on failure.
- Initial-data protocol RED1/49 → GREEN65/65, including duplicate producer source merge and reset conflicts.
- CPU raster192/192, schema609/609, Release build successful, CTest24/24 in25.87s, actual Vulkan1164/1164.
- Static/rotation flowers, CPU+GPU12frames each: all four before/after RGBA and raw HDR comparisons identical,
  synchronization validation clean. Existing CPU/GPU differences remain44/static and58/rotation.
- GPU maximize/restore2/2 clean; final1280×720 image matches client size.
- Independent read-only review found no new actionable issue. Diff whitespace checks pass.

Evidence: [captures, raw logs/configs and SHA256](2026-10-14-gaussian-managed-assets/).
Large HDR files remain in build-release-dyn-clang64/gaussian-assets-managed-check; hashes are archived.
The frozen runtime.before includes the initial-data protocol and both prior Gaussian frame migrations.

Gaussian storage migration is complete across GPU frames, CPU upload frames and immutable point/SH assets.
RT build resources, shared textures and samplers still remain; the whole-pass goal stays active.
Changes are uncommitted.
