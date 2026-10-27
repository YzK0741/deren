# Progressive texture residency

Part of [amnis](../../amnis.md): low-resolution fallback residency and asynchronous full-texture upgrades.

Implement the first ranked improvement approved in chat: retain a real low-resolution texture for
every streamed opaque texture while GPU feedback drives asynchronous full-resolution uploads.
Keep the existing same-frame GPU residency path as the default and comparison baseline.

The first increment has two quality levels, not virtual texturing or arbitrary mip residency.
`DEREN_PROGRESSIVE_TEXTURES=1` enables it when visibility texture streaming is configured. Generate
an at-most 128x128 RGBA8 fallback from mip zero, filtering sRGB colors in linear space. Preserve
aspect ratio and support non-power-of-two sizes. Alpha-sensitive, transparent, toon and overlay
textures retain the existing full-resolution pinning policy.

Consume each frame slot's GPU feedback only after its normal completion wait. Render the current
camera using available quality while uploads run; never freeze the view or synchronously demand an
unfinished full-resolution image. Publish descriptor changes only into the completed frame slot's
texture bank. Retain each bank's resource owners until that slot is reused. Upload jobs capture the
factory and immutable source, never the runtime; join them before shutdown's GPU idle wait.

Use separate per-frame descriptor banks (original 1024-slot bank and an additional bank at offset
2048). The original GPU path also uses the frame bank to keep shader addressing consistent. Asset
registration and one-shot mask baking retain the original bank. No RHI ABI change is needed.

Bound optional full-resolution payload to 512 MiB, in-flight upload payload to 128 MiB and active
jobs to two. Memory budgets are configurable through environment variables; job count is fixed at two. Low textures
and pinned full textures form a compulsory floor reported separately; allocation overhead and retired
frame owners can exceed payload budgets. Prefer visible/recent resident textures; retain invisible
textures for 30 updates when space allows, evict them first under pressure. Do not repeatedly upload
visible textures that cannot fit. Failed upgrades keep their valid fallback and are reported.
Disabling streaming requests eventual full residency and lifts the optional budget for restoration;
re-enabling streaming cancels that restoration and immediately restores normal budgeting.

Acceptance: pure tests for fallback filtering and budget decisions, full build, relevant CPU tests,
device validation and moving-camera capture proving no checkpoint in the progressive path, upgrades
complete, budget bounds hold, and the camera keeps changing. Compare fixed-view converged output to
the original complete-texture path. Record limitations and measurements; do not claim performance
benefits before measurements.
