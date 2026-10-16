# Bloom render graph integration — 2026-10-16

## Behavior

Adds a default-off `render.bloom_render_graph` startup option and
`runtime::set_bloom_render_graph(bool)`. Selection is locked at the first pace
attempt so the runtime never mixes legacy and graph state records mid-session.
Bloom intensity and debug-view feature gates remain live.

`deren.engine.render_graph.bloom` compiles four graphics passes (prefilter and
three downsamples), exports all four levels, then adds an explicit sampled-reader
boundary before the external composite. The HDR incoming transition stays with
its established owner. Existing `post_bloom_pass::record` retains the old barrier
path; `record_content` shares the rendering instance, constants and draw without
legacy barriers, and returns checked errors for the graph caller.

Runtime bindings retain existing output generations through shared image leases.
Those generations include the actual views referenced by descriptors. Frame slots
retain prepared tokens/off-path leases until pacing completes the slot. Existing
runtime ownership and waits continue to protect pipelines and descriptor storage.

Initialization is recorded per generation ownership identity and image instance,
only after successful submission. New generations start UNDEFINED; reused ones
retain fragment-read/color-write scopes while clearing texels. A global unrelated
resource reset does not erase another generation's state. Weak generation keys
also handle a return to a still-live older generation without allocator ABA or
keeping dead generations alive. Expired entries are removed at graph boundaries.

The disabled branch establishes descriptor layouts through scoped barriers; it
makes no graph content-read claim about undefined texels. Graph preparation or
recording failure prevents end/submit success, with no partial legacy fallback.
Successful submission remains published even if presentation later fails.

## Review and checks

- Independent read-only review found no blocking issue in synchronization,
  generation retention, pass-content separation or error propagation.
- RED: missing Bloom module, runtime selection method and config field each failed
  compilation before implementation. Runtime reuse exposed a probe-only bug that
  rejected accumulated source writes in a sampled abstract layout; validation now
  distinguishes source frontiers from destination use.
- Tests cover cold/reused scopes, alias refusal, four actual Bloom content calls,
  source lanes/push modes, final sampled states, three runtime frames, disabled
  Bloom, session selection lock, generation replacement/return, and a late graph
  failure that blocks submission. Config default/round-trip tests pass.
- Bloom test: 65 checks, zero failures. Follow-up self-check replaced a global
  initialization reset with generation-owned records; replacement/return coverage
  verifies a still-live generation is not treated as fresh again.
- Full Release build -j12, complete CTest 40/40 passed.
- ASan/UBSan build -j12, focused graph/Boost/config tests 12/12 passed.
- Backend gate includes graph/core/RHI/execute/Bloom and resource cluster archives:
  zero backend symbols, reference sites and owning-STL crossings. Native-zero,
  documentation/CI-list and dynamic boundary tests passed.

## Real GPU check

NVIDIA GeForce RTX 4060 Laptop GPU, DamagedHelmet.gltf, 256x256 offscreen,
8 frames, fixed scene-framed camera, TAA off, GUI hidden, validation enabled.

Legacy, graph, and graph with `VK_LAYER_VALIDATE_SYNC=1` produced identical PNG
bytes (SHA256):

`3E22A7524C9A7E19ADA574FD89BE5A4C9D0BAA37D2B5F996864E8297D8FA9187`

All three logs were validation-clean. A fourth graph run with gbuffer_debug=true
(Bloom disabled) and the same synchronization-validation environment was also
clean. Logs were additionally searched for VUID, Validation Error, SYNC-HAZARD and
graph recording refusal. The normal graph image was visually inspected and shows
the rendered helmet. Byte equality needs no external image-decoding dependency. The capture harness
reported missing optional Pillow statistics; the renderer itself exited zero and
produced the inspected PNGs. The first two wrapper commands inherited the optional
Python import failure status, not a renderer or validation failure.

Evidence lives in ignored build directories:

- build-release-dyn-clang64/render-graph-bloom-{red,runtime-red,config-red,
  full-build,full-ctest,boundary,gpu-legacy,gpu-graph,gpu-sync,gpu-off}.log
- build-release-dyn-clang64/render-graph-bloom-boundary.json
- build-release-dyn-clang64/render-graph-bloom-check/: configs, PNGs and app logs
- build-vessel-asan-clang64/render-graph-bloom-{build,test}.log

## Remaining acceptance

This is opt-in, not a default-path switch. Windowed WSI/resize, longer stress,
more scenes and broader graphics settings still need acceptance before enabling
by default or deleting the legacy implementation. No commits or pushes.
