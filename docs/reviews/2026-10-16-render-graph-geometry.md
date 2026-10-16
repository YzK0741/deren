# Geometry attachment graph acceptance

## Scope

The opted-in ordinary deferred path now records scene geometry, deferred LOAD
lighting and post in one composed graph. Geometry declares six discard writes:
three surfaces, velocity, HDR and depth. The imports have undefined contents;
geometry initializes them, then lighting reads the surface/depth and preserves HDR
emissive. The runtime retains existing generation owners for every attachment.

Eligibility is selected before scene recording. Unsupported TAA/debug/RT/MegaLights/
Gaussian/transparent/character/contour tails keep legacy geometry and tail, followed
by post graph. Default selection remains false. Graph recording failure blocks
submission; no fallback is attempted after partial recording.

Existing secondary segmentation and scheduling remain inside scene_pass. Its
record_content checks targets/frame and returns errors for rendering begin and
secondary begin/end/execute failures. An opened rendering scope is always closed.
Mesh/material/bindless/deformation inputs, host publication, shadow/cluster and
inactive descriptor-layout protocols remain external. This is attachment dependency
modeling, not a complete scene resource-access inventory.

## Automated acceptance

Observed RED: runtime tests failed three assertions because geometry recorded
before graph preparation; the CPU test also failed to build without the new
geometry module. Both pass after migration.

CPU tests compile a seven-pass geometry/lighting/post chain from undefined
attachments, check six producer accesses and five lighting handoffs, and reject
attachment aliases. Runtime probes cover deferred geometry recording, real empty
scene clears, both rim-only fallbacks, deferred callback failure, scene callback
failure and preparation failure before any scene rendering. Scene tests cover
single/parallel secondary begin, end and execute failures and scope closure.

Release build used -j12. Full CTest passed 44/44; focused ASan passed 17/17.
Expanded backend boundary gate includes librender_graph_geometry.a and found zero
backend symbols/import sites. Independent read-only review checked attachment
order, fallback, prepare failure, secondary errors, timings and span/payload
lifetimes; no actionable P1/P2 findings.

## GPU acceptance

RTX 4060 Laptop GPU, DamagedHelmet.gltf, fixed scene-framed camera, 256x256,
eight frames, GUI hidden. GPU timings and synchronization validation enabled.
Legacy/main PNGs were byte-identical for direct output, FXAA, scale-0.5 upscale,
debug fallback and windowed output. Debug is named bloom-off in the harness and
explicitly exercises fallback, not a separate supported bloom-off scene.
Supported cases require the scene-geometry activation log.

An additional TAA-enabled pair verified legacy geometry/tail fallback with matching
PNG SHA256 D05F6364623591FCBE8CA985BC25421906CD418F4FABCF0AB1C4AB14316D420A.
Its evidence is render-graph-geometry-taa-gpu.log and results-taa.json.

Twenty rapid window resizes over more than 2500 frames passed synchronization
validation. Final PNG was 630x374, matching client extent; visual inspection showed
the complete Helmet image without stale attachment content or resize corruption.

Ignored build directories retain render-graph-geometry-* build/red/test/boundary/
GPU/resize logs. render-graph-geometry-check/results.json stores hashes and capture
paths; its subdirectories contain configs, application logs and PNGs. ASan logs
are build-vessel-asan-clang64/render-graph-geometry-{build,test}.log.

No commit or push was requested. Complex tail combinations still need their own
graph declarations and device acceptance before expanding eligibility.
