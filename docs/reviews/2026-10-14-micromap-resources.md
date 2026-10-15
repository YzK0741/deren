# Managed opacity micromaps — 2026-10-14

Each MASK caster's micromap is now owned by a scene-lifetime resource-only subchain. BLAS retains
the micromap generation as an explicit dependency, so its raw geometry pointer cannot outlive the
managed object. Production forwards its owning device root; legacy reference callers keep their
existing device-lifetime obligation. Backend storage/scratch/setup allocations remain inside RHI.

Typed requests contain scalar micromap metadata, a stable initialization key and shared immutable
upload vectors. Stored RHI spans must be empty; creation supplies temporary spans synchronously.
Generation metadata and the provider release upload vectors after creation. Same-key reuse needs no
payload; recreation without it returns not_ready and preserves active resources. Payload vector lengths
are validated. UNKNOWN four-state values, strides, triangle ordering, builds and barriers are unchanged.
Production moves upload vectors instead of copying them. Guard triangle data-offset overflow.
Append engine resource id/kind; RHI ABI remains32.

Validation:

- Ownership RED1/356 before implementation; expanded portable checks373/373.
- Actual Vulkan AS build/refit/micromap/traversal456/456.
- Release build successful; CTest25/25 in26.58s.
- Three40-frame/.5-degree RT/animated-refit/scaled-ML captures: RGBA pixel-identical, sync-clean.
  RT smoke exercises14 micromaps and34940 triangles.
- Maximize/restore2/2 clean; final client/output1280×720.
- Upload-source release, same-generation reuse, missing-source refusal, malformed payload/raw spans,
  preserved active snapshots and retained device roots have portable coverage.

Evidence: [configs, captures, logs, comparison and SHA256](2026-10-14-micromap-resources/).
Frozen runtime.before includes the completed mask/skin batch. PNG recompression is lossless.
Normal frames add no reconciliation; per-object teardown drains have not been separately benchmarked.
Whole-plan review remains a final gate. Imported geometry, TLAS audit, shared textures/samplers and
final ownership/publication audit remain. Changes are uncommitted; user authorized continued work.
