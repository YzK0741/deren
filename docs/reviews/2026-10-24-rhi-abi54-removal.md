# ABI 54: remove incomplete RHI surfaces

2026-10-24. User-authorized cleanup of previously reserved, unused interfaces.

Removed: query/query_desc and resource_factory::create_query; swapchain_desc and
frame_context::create_swapchain; the old mesh_shader extension/dispatch_mesh interface.
The Vulkan no-op implementations and all test-double overrides are removed together.

Preserved: the swapchain type and borrowed frame_swapchain; frame presentation/recreation;
GPU timing; device_capabilities::mesh_shader and command_recorder mesh drawing.
Extension bit 0x4 retains its mesh_shader enumerator as RETIRED, is absent from the active
set and answers null. The remaining active mask is 0x3FD3 with eleven entries.

The resource/frame vtables and active-enumeration return layout changed. RHI ABI is now 54;
executables, backends and plugins must be rebuilt together. Tests explicitly reject ABI 53
before use and exercise retired-bit refusal in the static and dynamically loaded probes.

Verification: removal/ABI static assertions failed before production changes (five expected
failures). Full 14-worker build passed; CTest 69/69; device probes 5/5 (221, 95, 75, 12, 65
checks). Documentation checks 429/429. Standard, dynamic-runtime overview, manual main page,
vessel compatibility note and device-lane header are updated. Previous review records retain
the ABI/version they actually measured rather than being rewritten as ABI 54 evidence.

Local evidence: tmp/rhi-remove-reserved-red.log, tmp/rhi-remove-reserved-final-build.log,
tmp/rhi-remove-reserved-ctest.log, tmp/rhi-remove-reserved-device-probes.json.
Final comment/header formatting cleanup is rebuilt in tmp/rhi-remove-reserved-clean-build.log.
No commit or publication performed.

Independent read-only review found no important issues. Final ABI/ownership probes pass after cleanup.
