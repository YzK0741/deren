# GBuffer handoff and deferred lighting graph — 2026-10-16

Implemented the next incremental main-chain slice: legacy geometry produces the
GBuffer/depth/emissive, then one composed graph owns their lighting handoff,
deferred LOAD lighting and the existing Bloom/composite/FXAA/upscale path.
Geometry, shadow/cluster preambles, static bindless material/IBL inputs and host
publication remain explicitly external. This is not complete main-chain migration.

## Behavior

- Added CPU-only lighting::append with three sampled surface inputs, sampled depth
  and HDR preserve-write. Aliases are rejected. Exporting the final post output
  retains lighting; no intermediate root or manual HDR handoff is needed.
- Added deferred_pass::record_content: checked rendering begin/push and error
  result, without legacy barriers. Legacy record keeps its barrier then shares the
  same content body.
- Added main_render_graph configuration and startup setter, default false,
  implying post graph. Supports the ordinary deferred tail with TAA off. TAA/debug,
  ray shadows, MegaLights, Gaussian, transparency and forward/toon/Goo contour
  branches retain the old tail, selected before any graph recording.
- Imported current-frame producer scopes and retained existing GBuffer generation
  owners, avoiding a new cross-frame state cache. During graph deferred preparation
  only surface/depth ensure callbacks are suppressed. Velocity remains external.
- Retained existing inactive descriptor-layout recipes and screenshot/final
  handoff protocols. Failed preparation or callback blocks submission rather than
  falling back after partial graph recording. Positional GPU marks remain ordered.

## Review and tests

Independent review found a P2: forward-character readiness does not imply contour
readiness. A rim can remain enabled while the forward pipeline is unavailable.
Both toon_screen_rim and goo_rim are now explicitly excluded from main eligibility.
Observed RED on both rim-only probe cases, then GREEN after fixing the predicate.

The probe gained opt-in graphics factory/mesh capability stubs (default behavior
unchanged, shader bytes are not validated). Runtime tests now cover an eligible
main frame, both contour-only fallbacks, graph callback failure and pre-record
validation failure. Real scene_pass receives make_scene_frame through prepare;
the test checks that its empty-scene attachment clear opens a rendering instance.
This exercises the actual external producer rather than only deferred dispatch.

CPU lighting tests cover missing HDR contents, preserve-write LOAD dependency,
lighting-to-post sampled transition and alias refusal. Configuration tests cover
explicit true and default false. Existing tests also cover startup locks and
post/main selection consistency.

## GPU acceptance

RTX 4060 Laptop GPU, DamagedHelmet.gltf, 256x256, fixed scene-framed camera,
8 frames, GUI hidden, synchronization validation enabled. Five legacy/main pairs
were byte-identical and logs were clean. The harness requires an actual main graph
activation log for supported branches and a fallback log for debug:

| Branch | SHA256 |
| --- | --- |
| Direct | `3E22A7524C9A7E19ADA574FD89BE5A4C9D0BAA37D2B5F996864E8297D8FA9187` |
| FXAA | `F3167767F0DAD210B9936382C073A733DE771A2B16C0606EE2E9BA0F88C5EC6E` |
| Scale 0.5 upscale | `ADABFBAC2377121F8E22B2CDF776EC56734291F7E5D8DFDF12FE328FCB830273` |
| Debug fallback | `6AF9455B7BB0F961EF482DF9D100E35D5DCC1FC7263AC7364A110B1E76D4F2A0` |
| Windowed | `3E22A7524C9A7E19ADA574FD89BE5A4C9D0BAA37D2B5F996864E8297D8FA9187` |

A sixth legacy/main pair with TAA enabled verified fallback and identical bytes:
`D05F6364623591FCBE8CA985BC25421906CD418F4FABCF0AB1C4AB14316D420A`.
Three actual window resizes over hundreds of frames passed sync validation and
produced a 522x324 screenshot matching the final client area. Existing resize
harness was reused with the Helmet config. Other complex feature combinations
still need real-device acceptance; the default remains legacy.

An additional direct pair with GPU timings enabled matched the direct hash above,
with no synchronization or timestamp-order warnings.

## Evidence

Ignored build directories hold render-graph-lighting-* build/config-red/test/GPU/
resize/boundary logs, and render-graph-lighting-check/ configs, result JSONs, PNGs
and application logs. The rim-only RED is render-graph-lighting-rim-red.log.
ASan evidence is build-vessel-asan-clang64/render-graph-lighting-*.log.

Expanded boundary gate includes librender_graph_lighting.a and remains zero.
Release build (-j12) passed; full CTest passed 44/44 and the focused ASan suite
passed 16/16. Final explicit-header rebuild and post/lighting/docs checks passed
3/3. A concurrent GPU capture initially held the Windows executable open during
linking; rebuilding after the capture exited succeeded.
No commit or push was requested.
