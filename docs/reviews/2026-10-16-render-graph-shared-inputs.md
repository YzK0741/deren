# Shared scene inputs and task/mesh stage acceptance

## Implemented scope

The opted-in main graph imports eight read-only shared buffer generations. Geometry
declares camera uniform reads in mesh+fragment, materials storage reads in fragment,
and instance/previous-transform/current-skin/previous-skin/morph storage reads in
mesh. Deferred lighting declares camera/light uniform fragment reads. Camera is
imported once and its ref is reused by both consumers.

The input bundles are optional for attachment-only CPU append clients. The runtime
requires complete bundles, selects the completed frame slot for per-slot resources,
checks device/generation/expected buffer identity and retains aliasing generation
leases through existing GPU completion retention. Shared tables use whole-buffer
ranges. Creation and CPU writes remain external; read-state imports are not host
write synchronization and add no fake GPU upload passes.

Task=256 and mesh=512 were added without changing existing stage bits. CPU scope
validation, RHI bridge, JSON diagnostics and Vulkan native mapping understand both.
The Vulkan capability mask advertises them only for an enabled device feature on
a graphics queue. Probe coverage includes unsupported mesh rejection, enabled mesh
acceptance and continued task rejection on a mesh-only probe. No task draw pipeline
was added; task mapping is CPU-tested rather than device draw-tested.

Read-only image transitions cover new stages when the image has live task/mesh
consumers in the same graph; ordinary post graphs keep their prior stage masks.
Cross-graph task/mesh readers need explicit boundary coverage.

## Verification

Observed missing task/mesh API RED before stage implementation and missing shared
bundle API RED before bundle implementation. Tests cover undefined shared input
rejection, thirteen geometry accesses, seven lighting accesses, canonical camera
reuse, no host pseudo-write barriers, stage lowering/names and conditional image
transition coverage.

Release build used -j12 and full CTest passed 44/44. ASan relevant targets were
rebuilt and the focused suite passed 17/17. Expanded backend boundary scan,
including geometry/lighting and all graph archives, remained zero.

On RTX 4060 Laptop GPU, synchronization validation and GPU timings were enabled.
Five DamagedHelmet legacy/main pairs were byte-identical: direct, FXAA, scale-0.5
upscale, debug fallback and windowed. The harness requires the scene-geometry main
activation log on supported branches. An additional animated_skin_plane pair at
animation_time=0.4 was also byte-identical, SHA256
ED0AC7F715CFE1F3FCCB33765E6DED6A47B17F385935CF8C989D22DB393A6799.
Three actual window resizes passed synchronization validation; final output
742x444 matched the client extent.

Evidence: ignored build-release-dyn-clang64/render-graph-shared-* logs and
render-graph-shared-check/ configs, application logs, JSON hashes and PNGs. ASan
evidence: build-vessel-asan-clang64/render-graph-shared-{build,test}.log.

## Independent review and remaining boundaries

Read-only independent review found no confirmed P1/P2 findings. Two nonblocking
limits were documented: graph preparation correctly refuses custom storage-only
camera/light buffers because shaders declare ConstantBuffer (the old provider
validation accepted them), and optional-stage cross-graph transition coverage is
explicit rather than inferred from a future graph.

Material textures/IBL, per-mesh addresses, meshlet tables/atomic statistics,
indirect commands, cluster production and other rendering branches remain outside
this slice. Meshlet statistics must eventually declare their atomic writes rather
than join the read-only bundle. No commit or push was requested.
