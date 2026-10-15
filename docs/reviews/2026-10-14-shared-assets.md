# Shared images, samplers and final ownership audit — 2026-10-14

Shared textures, white fallback, IBL environment/irradiance/BRDF LUT, furnace cube, display LUT and
Goo FGD now declare image creation through resource-only subchains. Seven samplers use typed registry
descriptions with all original filtering/address/compare parameters. Runtime fields are shared_ptr
aliases owning those subchains, which retain device roots and drain before releasing objects.
Image/view aliases share one chain, so the whole view remains alive with the image allocation.
Shared images request whole views only; existing shadow/attachment requests keep layer-view defaults.

Image initial contents use the existing immutable upload/key protocol. Temporary owned bytes are copied
from input spans, borrowed synchronously by RHI creation, then released by producer and generation metadata.
Same-key reuse needs no source; recreation without source refuses. Format, mip count, dimensions,
cube compatibility and content bytes stay unchanged. Display grading remains the existing2D LUT layout.
No reconciliation/copy is added to normal frames. Import-time CPU copying and per-chain teardown drains
have not been separately benchmarked. Append engine resource ids/kinds; RHI ABI remains32.

The final allocation census found the RT shader binding table still used runtime's generic upload hook.
That hook now declares managed coherent/addressable buffers and retains a subchain alias. Preserve its
flags, bytes, addressing and pass-owned pipeline/region metadata. Publish display/FGD images in the
typed frame table; existing heap slots and sampler-hint access remain unchanged.

Two P1 findings from independent whole-plan review were fixed:

- Refused micromap build or ordering barrier now returns before dependent BLAS recording, leaves
  readiness false and keeps prepared resources through completion. Bake ordering refusal also stops.
- IBL keeps previous owners until all three heap writes succeed. On refusal, completed writes roll back.
  If rollback fails, candidate image chains remain retained and acquisition refuses until a complete
  successful replacement restores publication. LUT setters also publish before replacing owners.

IBL replacement stages a complete triple/sampler and replaces the previous vector, fixing the old
append behavior where frame-table publication continued naming the first triple. It waits for prior GPU
readers before heap changes. Setters retain the existing requirement that callers use frame boundaries
and do not mutate runtime state concurrently with CPU recording.

Validation:

- Initial image/sampler protocol RED2/394 → expanded portable403. Review refusal regressions
  RED2/418 (AS) and RED12/78 (shared replacement) → final portable418, Vulkan AS/traversal501,
  shared replacement88. Tests inject each IBL heap-write refusal, LUT refusal, rollback refusal/recovery.
- Final Release build successful; CTest25/25 in27.14s. Schema610 checks and RHI ABI32 pass.
- Fifteen40-frame scene sweeps: zero changed pixels and clean synchronization logs.
- Flower CPU/GPU paths each run40frames/.5-degree sweep before/after: RGBA and HDR bytes identical
  for each path. Their small pre-existing CPU-versus-GPU differences are unchanged.
- Maximize/restore2/2 clean; final client/output1280×720.
- Fresh independent review covered the whole migration; follow-up accepts both P1 fixes and found
  no concrete new issue in rollback, retention, descriptor compatibility, uploads or SBT ownership.

Evidence: [captures/configs/logs/comparison/SHA256](2026-10-14-shared-assets/).
Baseline includes completed geometry migration and the same executable-relative FGD fixture.
Comparison JSON names selected capture files. PNG recompression and HDR gzip are lossless.

The production allocation audit is complete. Remaining direct creation calls are diagnostic probes/HDR
capture readback and unused legacy helper definitions. Backend-internal storage/scratch/heap services,
borrowed presentation images/views and explicitly caller-owned legacy imports keep their documented
ownership contracts. Automatic barriers, a full resource-use graph, exhaustive bindless-element
publication and legacy API removal are outside this ownership migration.

All planned production pass/shared-resource migrations are complete. Changes remain uncommitted and
have not been pushed. The reports and plan preserve the batch evidence and caller obligations.
