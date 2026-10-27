# amnis

**amnis** is the lowercase umbrella codename for the project's recent texture residency,
streaming and supporting measurement work. Subfeatures retain their actual technical names.
The name does not change existing APIs, configuration, environment variables or defaults.

## Responsibility boundary

amnis chooses which textures/mips to request, retain, upgrade or evict and schedules their preparation.
[Subtilis](subtilis.md) chooses geometry representation/LOD and performs geometry occlusion and optional
software routing. amnis does not select cluster cuts or replace Subtilis HZB culling.

Both use shared visibility rendering, synchronization and resource services. Visibility resolve can
emit visible-material and screen-footprint feedback for amnis, with or without Subtilis DAG LOD.
Subtilis can likewise run with fully resident textures. Sharing these services does not imply a
single subsystem switch or exclusive ownership of the renderer.

## Subfeatures

| Layer | Technical feature | Scope and documentation |
|---|---|---|
| Demand | GPU visibility feedback | Generate texture demand from visible materials. |
| Scheduling | Asynchronous texture upload and texture prefetch | Prepare candidates in advance and publish completed uploads. |
| Residency | Low-resolution fallback residency and full-texture upgrades | Two-tier residency; see [design](superpowers/specs/2026-10-27-progressive-textures-design.md). |
| Residency | Screen mip demand feedback and progressive mip residency | Select intermediate mips and refine incrementally; see [design](superpowers/specs/2026-10-27-screen-mip-residency-design.md). |
| Validation | Texture streaming benchmarks | Compare frame times, memory and images; see [three-mode results](benchmarks/2026-10-27-resource-memory-comparison.md). |

## Shared supporting infrastructure

These services support amnis and other renderer clients; they are not amnis-owned selection policies.

| Shared service | Scope and documentation |
|---|---|
| Lazy resources | Allocate and upload on demand; see [design](superpowers/specs/2026-10-26-lazy-resources-design.md). |
| Resource unload | Release allocations when dependencies allow, preserving reload sources; see [design](superpowers/specs/2026-10-26-lazy-unload-design.md). |
| Resource ownership and synchronization | Retain generations until dependent work completes; see [managed resources](managed_resources.md) and [RHI standard](rhi_api_standard.md). |
| GPU resource and vessel memory statistics | Shared allocation counters, not per-feature attribution; see [API guide](resource_memory_statistics.md). |

Subtilis M5 refers to future geometry-page streaming, not texture streaming. It remains deferred;
amnis texture streaming does not implement or complete that milestone.

## Comparison mode names

- **Full texture residency**: preload textures with streaming disabled; geometry culling can remain enabled.
- **Visibility texture streaming**: original same-frame GPU feedback with whole-image loading and unloading.
- **Two-tier texture streaming**: resident low-resolution fallbacks and asynchronous full-texture upgrades.
- **Progressive mip streaming**: low-resolution fallbacks, screen demand and asynchronous intermediate mip upgrades.

These are distinct modes; amnis is the umbrella name. Prefer these technical names over
ambiguous descriptions such as "pure GPU" or "the current approach".
Two-tier streaming requires `DEREN_PROGRESSIVE_TEXTURES=1`; progressive mip streaming also
requires `DEREN_PROGRESSIVE_MIPS=1`. Both depend on visibility texture streaming being configured.
The progressive paths remain opt-in.

## Experiments and pending work

CPU occlusion culling and dual-view preparation retain their technical names and must be
identified as experiments; the amnis name alone does not imply they are enabled.
Texture source sharing and post-import texture data cleanup are pending CPU memory improvements,
not completed capabilities.
