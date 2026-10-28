# Texture source memory optimization plan

**Goal:** Reduce retained CPU texture copies while preserving streaming/reload and image output.
**Spec:** ../specs/2026-10-27-texture-source-memory-design.md
**Method:** Execute inline using writing-plans/TDD. User authorized memory work; previous performance
work remains intact and is no longer being optimized in this increment.

- [x] Add failing tests for optional owner propagation, exact-owner sharing, borrowed/subspan fallback,
  immutable resolved owners and explicit decoded-pixel release/lifetime.
- [x] Implement shared immutable source propagation and import-only cleanup, retaining toon-sidecar pixels.
- [x] Build and test CPU/GPU behavior, including asynchronous upgrade and unload/restoration.
- [x] Compare old/new memory on identical eager/GPU/progressive/composed scenarios, record peaks and images.
- [x] Review changes and document exact accounting, savings and retained-data limitations.

Completed 2026-10-27: [texture source memory report](../../benchmarks/2026-10-27-texture-source-memory.md).
All four measured profiles retained about 5.069 GiB less project CPU memory; the comparison and its
limits (allocator workset, import peak, retained streaming source) are recorded there. The ownership
boundary suite, loader tests and full CTest 73/73 pass on the current tree.

Follow-up same day: the consuming resolve path moves decoded RGBA8 into mip0 and releases unreferenced
decoded textures before mip generation. A separate 240-frame sampler measures the initialization peak
falling from 5.280 to 3.094 GiB for the matched textured-DAG eager asset. The new two-tier streaming
capture observes 3.090 GiB; it omits `--mips` and is not a matched steady-state comparison against
the earlier progressive-mip captures. Resolved slots are byte-identical against the copy path.

Files: source/engine/primitive/primitive.cppm; runtime/runtime.declarations.cppm and
runtime.constructor.cppm; source/gltf_loader/{gltf_loader.cppm,gltf_loader.cpp}; source/app/main.cpp;
source/tests/{test_runtime_injection.cpp,test_gltf_loader.cpp}; docs/amnis.md and benchmark evidence.
