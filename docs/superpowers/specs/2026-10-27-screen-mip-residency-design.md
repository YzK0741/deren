# Screen-driven mip residency

Part of [amnis](../../amnis.md): screen-demand feedback and progressive mip residency.

Continue the approved progressive streaming work with intermediate mip levels and screen demand.
Keep the existing default GPU baseline and two-tier progressive mode available as controls. Enable
the new behavior only with both DEREN_PROGRESSIVE_TEXTURES=1 and DEREN_PROGRESSIVE_MIPS=1.

Reuse the visibility resolve's UV-per-pixel footprint. An optional independent per-slot GPU buffer
records the maximum requested texture extent per material (2 / footprint, one extra mip of safety,
clamped to 16384; degenerate footprints conservatively request full quality). It is independent of
the actual resident texture dimensions, so low textures cannot suppress their own upgrade requests.
The resolve declares this storage access to the graph and publishes it to host reads after dispatch.
Consume only the completed slot's result, retaining the normal two-frame feedback latency.

Represent a resident tier as a smaller physical image containing a suffix of the original RGBA8 mip
chain. For single/partial-mip sources, generate the missing requested level on a worker. No sparse
allocation, page table, RHI ABI changes or CPU occlusion. The existing 128-pixel fallback stays resident.
Upload at most one finer mip per texture at a time, retain the preceding valid image until publication,
and preserve completed-slot descriptor ownership. Under pressure select smaller mip images rather
than choosing full images exclusively. Prioritize visible entries, then resident/recent entries, giving
each selected visible entry an intermediate tier before allocating further detail.

Use the existing 512 MiB optional / 128 MiB pending / two-job defaults. Account actual tier byte sizes,
pending replacement bytes and old frame owners separately. Keep failed upgrades on a valid image.
Disabling streaming restores full quality asynchronously (direct full uploads allowed); re-enabling
cancels restoration and restores normal budgets. Pinned alpha/transparent/toon/overlay data stays full.
Non-visibility fallback conservatively requests full quality without assuming a screen footprint.

Acceptance: pure tests for mip suffix offsets, NPOT layouts, missing mip generation and multi-level
budget selection; full build/CTest; validation of fixed/swept/zero budget/restoration and partial mip
fixtures; compare frame time, pending bytes, low/intermediate/target-ready counts and final image
differences against both two-tier and full-preload controls. Do not call payload accounting total VRAM,
or claim equal image quality without comparison. Preserve source and measurements, no commit/push.
