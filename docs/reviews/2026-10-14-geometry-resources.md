# Scene geometry upload ownership — 2026-10-14

Production create_primitive now describes vertex/index buffers in a scene-lifetime resource-only
subchain, preserving upload intent, flags and exact input bytes. Admission validates both capacities
before publication. Temporary immutable upload owners are released after synchronous creation.
This copies the input spans once on CPU during import; there is no additional GPU geometry allocation
or normal-frame reconciliation. Import/teardown performance has not been separately benchmarked.

Geometry views retain their owning generations and expose leases. BLAS declarations now retain both
source vertex/index leases as well as the previously managed mask/skin/micromap dependencies. Releasing
a leaf drains its subchain while retained BLAS snapshots can keep immutable geometry and device alive.
Moving a geometry view clears the source view. Legacy object_manager assignments transfer their single
RHI reference into a shared holder, preserving assignment/get/dereference/bool/reset operations.
Concrete C++ field types change; internal callers were rebuilt. RHI ABI and shader/heap layouts remain32.

Custom legacy primitives must still keep the external device alive. The unused legacy static batch
class keeps its original direct buffer owners; it has no production creation path. CPU caster pointers
remain borrowed from the scene tree; this change retains GPU allocations and does not authorize deleting
scene leaves concurrently with RT phase updates.

Validation:

- Geometry lease RED1/386 → expanded portable392/392; Vulkan AS/traversal475/475; schema610/610.
- Release successful; final complete CTest25/25 passed in31.86s, including the documentation checks.
- Fifteen40-frame scene sweeps are pixel-identical and have clean synchronization logs;
  maximize/restore2/2 clean, final client/output1280×720.
- Portable checks retain managed source geometry/device through leaf/registry/BLAS-owner destruction
  until the retained BLAS snapshot releases. Legacy geometry leases likewise survive view reset.

Evidence: [captures/configs/native logs/pixel comparison/SHA256](2026-10-14-geometry-resources/).
Frozen runtime.before includes micromap and TLAS audit batches. PNG recompression is lossless.
The first character baseline was invalid: its frozen executable directory lacked the FGD PNG,
while the candidate loaded it. Preserve those logs under invalid-baseline-laevatain; retain the old
screenshots alongside the valid rerun. After copying the same FGD fixture, the rerun is pixel-identical.
Comparison JSON names the valid capture files; archive the fixture and hash both runtime copies.
Shared textures/samplers and final whole-plan review/audit remain; changes uncommitted.
