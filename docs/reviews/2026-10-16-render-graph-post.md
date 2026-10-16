# Post-processing render graph migration — 2026-10-16

The opt-in `post_render_graph` path now records Bloom, composite and optional
FXAA/upscale through the graph executor. Resources are created outside the graph.
Legacy remains the default; this stage does not migrate the scene/G-buffer chain.

## Behavior and ownership

- Typed pass snapshots declare HDR, four Bloom levels, optional LUT and the
  selected LDR/output attachments. Disabled Bloom explicitly clears its levels.
- Existing generation owners retain images/views; leases, prepared plans and the
  output-view cluster remain in the paced frame slot through GPU completion.
  Bloom/LDR warm states publish only after successful submission.
- `rhi::frame_resources` is an optional capability, without changing existing
  virtual interfaces. Shared image snapshots pin the core and identify one acquire;
  they do not extend native swapchain generation validity through resize.
- Stale bindings fail before recording and between callbacks, including passes
  without barriers. Canonical owned identities now use VMA record addresses.
- Screenshot/readback remains between graph drawing and a separate graph final
  handoff. UI stays inside the final draw's rendering scope. PRESENT has empty
  destination stage/access; offscreen output ends sampled.
- Failed readiness/preparation/recording blocks submission. Legacy content-only
  functions share actual drawing with checked graph callbacks.

## Review and regression coverage

Independent read-only review found a missing-composite readiness return that did
not set the graph failure status. Fixed it and added a submission-blocking test.
Re-review found no further correctness issues.

Tests cover all six Bloom/resolve graph branches, LUT declaration, alias rejection,
final handoff scopes, runtime multi-frame/off branches, validation failure and
missing-pipeline refusal, stale acquire snapshots and invalidation between
barrier-free callbacks. Release full suite: 42/42. ASan graph/config/Boost subset:
14/14. Expanded engine/backend archive boundary: zero symbols/imports.

## Real GPU acceptance

NVIDIA GeForce RTX 4060 Laptop GPU, DamagedHelmet.gltf, 256x256, scene-framed fixed
camera, TAA off, GUI hidden, 8 frames, Vulkan synchronization validation enabled.
Each legacy/graph pair produced identical PNG bytes:

| Branch | SHA256 |
| --- | --- |
| Direct composite | `3E22A7524C9A7E19ADA574FD89BE5A4C9D0BAA37D2B5F996864E8297D8FA9187` |
| FXAA | `F3167767F0DAD210B9936382C073A733DE771A2B16C0606EE2E9BA0F88C5EC6E` |
| Upscale, scale 0.5 | `ADABFBAC2377121F8E22B2CDF776EC56734291F7E5D8DFDF12FE328FCB830273` |
| Bloom off via G-buffer debug | `6AF9455B7BB0F961EF482DF9D100E35D5DCC1FC7263AC7364A110B1E76D4F2A0` |
| Windowed composite | `3E22A7524C9A7E19ADA574FD89BE5A4C9D0BAA37D2B5F996864E8297D8FA9187` |

All eight offscreen runs were clean. The original short-run log filter omitted
bare `[ERROR]` diagnostics, so its initial windowed-clean verdict was incorrect;
the filter now includes both error and warning prefixes. A window/resize test exposed an acquire
WRITE_AFTER_READ hazard: the initial UNDEFINED transition had no source execution
scope to chain the COLOR_ATTACHMENT_OUTPUT semaphore wait. The Vulkan recorder
now adds ALL_COMMANDS to the source of a windowed frame snapshot's UNDEFINED
transition, keeping source access empty. Ordinary owned/offscreen images are
unchanged. Independent review accepted this boundary-specific correction.

With the corrected log filter and acquire fix, both windowed short captures
(`windowed-legacy-final` / `windowed-graph-final`) exited zero, remained validation
clean and reproduced the matching windowed SHA256 above.

Re-running the same harness with the fix passed three resizes over hundreds of
frames, synchronization validation clean, with a 522x324 screenshot matching the
final client area. The existing `check_gaussian_resize.ps1` harness was reused with
the Helmet config; its environment-controlled frame trace also works for meshes.

Evidence is in ignored build directories:

- `build-release-dyn-clang64/render-graph-post-*` build/test/boundary/GPU/resize logs.
- `build-release-dyn-clang64/render-graph-post-check/`: configs, harness, results.json,
  PNGs and application logs; `resize/` preserves failure, `resize-fixed/` passes.
- `build-vessel-asan-clang64/render-graph-post-*` build/test logs.

Broader scenes, GUI-enabled stress and graphics combinations remain default-switch
acceptance work. No commit or push was requested.
