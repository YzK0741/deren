# amnis

**amnis** is the lowercase umbrella codename for the project's recent texture residency,
streaming and supporting measurement work. Subfeatures retain their actual technical names.
The name does not change existing APIs, configuration, environment variables or defaults.

> **Phase closed (2026-10-28).** The opt-in baseline includes low-resolution fallback residency,
> asynchronous uploads, progressive mip demand, immutable source ownership/consuming import and
> asynchronous feedback copy readback. Further feature expansion is deferred. See the
> [phase closeout](reviews/2026-10-28-amnis-phase-closeout.md) for accepted scope, measured results,
> known limits, deferred directions and the next renderer investigation.

## Responsibility boundary

amnis chooses which textures/mips to request, retain, upgrade or evict and schedules their preparation.
[subtilis](subtilis.md) chooses geometry representation/LOD and performs geometry occlusion and optional
software routing. amnis does not select cluster cuts or replace subtilis HZB culling.

Both use shared visibility rendering, synchronization and resource services. Visibility resolve can
emit visible-material and screen-footprint feedback for amnis, with or without subtilis DAG LOD.
subtilis can likewise run with fully resident textures. Sharing these services does not imply a
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

CPU occlusion culling and dual-view preparation retain their technical names and must be
identified as experiments; the amnis name alone does not imply they are enabled.
Texture source sharing and post-import texture data cleanup are implemented CPU memory improvements;
see [measurements and limits](benchmarks/2026-10-27-texture-source-memory.md).
The remaining progressive-mip CPU cost against eager residency is isolated in the
[amnis CPU/residency comparison](benchmarks/2026-10-27-amnis-isolation.md).
That comparison predates [asynchronous feedback copy readback](benchmarks/2026-10-28-feedback-copy-readback.md),
which reduces the measured full-frame cost by keeping GPU writes separate from CPU reads.

## Asynchronous feedback readback

Progressive mip and two-tier streaming write feedback into GPU storage, then copy it to independent
coherent CPU-readable buffers for each frame slot. CPU consumption follows the existing completed-slot
wait; the change adds neither a same-frame checkpoint nor an extra frame of feedback delay. A shared
immutable zero source resets GPU feedback by copy before shader atomics. Original same-frame visibility
streaming retains its checkpoint path. Cached host memory is preferred for the readback allocation,
but actual memory type selection and performance remain device dependent.

The measured implementation adds 64 KiB of device-local live buffer allocation and about 196 KiB
of total live resource allocation. VMA reserved host block capacity can grow separately (32 MiB in
the captured run). See the linked report for full-frame timings, GPU tradeoffs and image limits.

## CPU source ownership

The loader publishes a shared immutable owner for each resolved mip chain. Import propagates
that owner to lazy/progressive streaming, so those systems can retain the same source for later
uploads and restoration. Sharing requires an explicit `shared_ptr<const vector<uint8_t>>` owner
whose complete vector exactly matches the byte span. Mutable owners, borrowed data and partial
spans retain snapshot-copy behavior. Every alias of a shared source must remain immutable.

The application releases decoded source pixels after material resolution when no toon sidecar
needs them. The consuming resolve path moves RGBA8 pixels directly into mip0 and releases
unreferenced decoded textures before mip generation, so import does not retain the decoded set
and the mip chains at the same time. It drops its resolved-material references after import;
streaming retains needed owners, while eager uploads can release their CPU sources. Geometry and
animation data remain alive. `release_texture_pixels` preserves metadata and resolved owners, but
resolving materials again requires reloading the source. Toon-sidecar models conservatively retain
decoded pixels.

This reduces retained CPU allocations and the import peak (about 5.3 to 3.1 GiB in the measured
textured-DAG capture). It does not reduce texture quality or GPU residency budgets; the remaining
peak is dominated by the full mip chains, not by a second decoded copy.
