# amnis

**amnis** is the lowercase umbrella codename for the project's recent texture residency,
streaming and supporting measurement work. Subfeatures keep their actual technical names.
The name does not change existing APIs, configuration, environment variables or defaults.

> **Phase closed (2026-10-28).** The opt-in baseline includes low-resolution fallback residency,
> asynchronous uploads, progressive mip demand, immutable source sharing/consuming import and
> asynchronous feedback copy readback. Further feature expansion is deferred. The phase closeout (accepted
> scope, measured results, known limits, deferred directions and the next renderer investigation) is
> maintainer material and is not distributed with this repository.

## Responsibility boundary

amnis chooses which textures and mips to request, keep resident, upgrade or evict, and schedules their preparation.
[subtilis](subtilis.md) chooses geometry representation/LOD and performs geometry occlusion and optional
software routing. amnis does not select cluster cuts or replace subtilis HZB culling.

Both use shared visibility rendering, synchronization and resource services. Visibility resolve can
emit visible-material and screen-footprint feedback for amnis, with or without subtilis DAG LOD.
subtilis can likewise run with fully resident textures. Sharing these services does not imply a
single subsystem switch or one subsystem driving the renderer exclusively.

## Subfeatures

| Layer | Technical feature | Scope and documentation |
|---|---|---|
| Demand | GPU visibility feedback | Generate texture demand from visible materials. |
| Scheduling | Asynchronous texture upload and texture prefetch | Prepare candidates in advance and publish completed uploads. |
| Residency | Low-resolution fallback residency and full-texture upgrades | Two-tier residency; the design record is not distributed. |
| Residency | Screen mip demand feedback and progressive mip residency | Select intermediate mips and refine incrementally; the design record is not distributed. |
| Validation | Texture streaming benchmarks | Compare frame times, memory and images; the three-mode results are not distributed. |

## Shared supporting infrastructure

These services support amnis and other renderer clients; they are not selection policies specific to amnis.

| Shared service | Scope and documentation |
|---|---|
| Lazy resources | Allocate and upload on demand; the design record is not distributed. |
| Resource unload | Release allocations when dependencies allow, preserving reload sources; the design record is not distributed. |
| Resource lifetime and synchronization | Keep each generation valid until dependent work completes; see [managed resources](managed_resources.md) and [RHI standard](rhi_api_standard.md). |
| GPU resource and vessel memory statistics | Shared allocation counters, not per-feature attribution; see [API guide](resource_memory_statistics.md). |

subtilis M5 refers to future geometry-page streaming, not texture streaming. It remains deferred;
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

## Experiments and memory status

CPU occlusion culling and dual-view preparation keep their technical names and must be
identified as experiments; the amnis name alone does not imply they are enabled.
Texture source sharing and post-import texture data cleanup are implemented CPU memory improvements;
the measurements and limits are not distributed with the repository.
The remaining progressive-mip CPU cost against eager residency was isolated in a local amnis
CPU/residency comparison. That comparison predates the asynchronous feedback copy readback described
below, which reduces the measured full-frame cost by keeping GPU writes separate from CPU reads.

## Asynchronous feedback readback

Progressive mip and two-tier streaming write feedback into GPU storage, then copy it to independent
coherent CPU-readable buffers for each frame slot. CPU consumption follows the existing completed-slot
wait; the change adds neither a same-frame checkpoint nor an extra frame of feedback delay. A shared
immutable zero source resets GPU feedback by copy before shader atomics. Original same-frame visibility
streaming keeps its checkpoint path. Cached host memory is preferred for the readback allocation,
but actual memory type selection and performance remain device dependent.

The measured implementation adds 64 KiB of device-local live buffer allocation and about 196 KiB
of total live resource allocation. VMA reserved host block capacity can grow separately (32 MiB in
the captured run). The full-frame timings, GPU tradeoffs and image limits were kept in a local report
that is not distributed with the repository.

## CPU source sharing

The loader publishes one shared immutable copy of each resolved mip chain. Import passes that
copy to lazy/progressive streaming, so those systems can reuse the same source for later
uploads and restoration. Sharing requires an explicit `shared_ptr<const vector<uint8_t>>`
whose complete vector exactly matches the byte span. A mutable source, data that other code
also references and partial spans still get a snapshot copy. Every alias of a shared source must
remain immutable.

The application releases decoded source pixels after material resolution when no toon sidecar
needs them. The consuming resolve path moves RGBA8 pixels directly into mip0 and releases
unreferenced decoded textures before mip generation, so import never keeps the decoded set
and the mip chains valid at the same time. It drops its resolved-material shared sources after
import; streaming keeps the sources it still needs, while eager uploads can release their CPU copies. Geometry and
animation data remain valid. `release_texture_pixels` preserves metadata and the resolved shared sources, but
resolving materials again requires reloading the source. Toon-sidecar models keep their decoded pixels
regardless.
