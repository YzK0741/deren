# Texture source ownership and import lifetime

User requested memory optimization after the CPU recorder work. The measured Bistro source stack
contains decoded pixels, resolved full mip chains and extra streaming copies. Reuse one immutable
mip owner and shorten import-only lifetimes without changing texture quality, GPU budgets or reload.

The loader publishes const shared owners for resolved mip data. Engine texture_input adds an optional
const shared byte-vector owner; generic image sources without it remain supported. Import propagates
compatible owners. Streaming shares an owner only when its entire vector exactly matches the data span;
borrowed/subspan/mismatched inputs retain the existing copy behavior. Source aliases must remain immutable
while retained, as already required by lazy resources.

Add an explicit loader release_texture_pixels(scenes&) operation that frees only decoded pixel storage,
preserving texture/node/animation metadata and resolved images. The application invokes it after material
resolution when no toon sidecar lookup needs raw pixels. Toon-sidecar models conservatively retain pixels.
After scene import the application drops resolved-material owners and its unused drawable iterator;
streaming/lazy owners survive. Keep scene geometry, nodes, animations and subtilis hierarchy alive.

Acceptance: sharing/fallback/lifetime CPU tests, immutable resolved views, idempotent raw-pixel cleanup
with metadata intact, full regression/GPU probes, actual fixed/moving memory captures and unload/reload
restoration checks. Use existing per-allocator counters; separately distinguish OS memory if sampled.
Preserve all preceding uncommitted recorder changes. No commit/push or feature-default change.

**Implemented 2026-10-27.** The loader also has a consuming resolve overload: RGBA8 decoded pixels
move into mip0, the mip chain owns the base level without a second copy, and unreferenced decoded
textures are released before mip generation. The app uses it whenever no toon sidecar needs raw
pixels. The copy and move paths produce byte-identical resolved slots in tests. Measured lifetime
peak falls from about 5.3 to 3.09 GiB while steady allocator and GPU counters remain unchanged; see
[the texture-source memory report](../../benchmarks/2026-10-27-texture-source-memory.md).
