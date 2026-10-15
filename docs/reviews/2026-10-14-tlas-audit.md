# TLAS readiness and publication audit — 2026-10-14

Each TLAS slot now separates allocated storage from traversal readiness. begin() and record_build()
clear readiness; only successful nonempty recording enables the public structure handle. Empty or
failed builds retain reusable allocations/snapshots but return a null traversal handle. Retrying a
build also clears prior success before recording. Guard an invalid/zero-slot build before indexing.

The RT phase begins the slot before skin/refit recording, so early update errors cannot expose stale
TLAS data. A failed per-frame skin callback now returns disable_skin_bake rather than silently
continuing with the previous deformation. Runtime already handles that update flag. Resources remain
retained through possible partial recording. Heap descriptors are gated by ready handles; existing
null handling leaves old heap bytes unused, rather than writing a null AS descriptor.

TLAS schema now uses per_frame_slot. resolved_binding gains a typed AS pointer, frame publication
publishes each slot's ready handle, and declaration resolution accepts that pointer as a resource.
Missing/wrong-slot AS dependencies refuse resolution. Existing heap writes and feature gating remain.

Validation:

- Empty/failed readiness RED2/380; schema RED1/610; publication RED1/200;
  ignored per-frame skin failure RED1/385. Final portable AS385/385, pass205/205, schema610/610.
- Actual Vulkan AS build/refit/micromap/traversal468/468.
- Release successful, final CTest25/25 in33.90s.
- Three RT/animated-refit/scaled-ML40-frame captures: zero changed pixels, synchronization clean.
- Maximize/restore2/2 clean; final client/output1280×720.
- Checks cover empty frames, failed/retried recording, retained allocations, skin-update failure,
  slot selection, missing AS and declaration-driven AS resolution.

Evidence: [captures/configs/logs/comparison/SHA256](2026-10-14-tlas-audit/).
Frozen runtime.before includes micromap migration. Whole-plan review is still pending.
Imported geometry retention, shared textures/samplers and final audit remain; changes uncommitted.
