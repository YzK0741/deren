# Texture group implementation progress

## Resource ownership stage

ABI 31 adds a 16-slot immutable texture_group, assign_texture_group and load_texture_group.
The default backend refuses both operations with unsupported. Vulkan implements the factory:
empty optionals and optional(nullptr) are empty slots; foreign-device, borrowed, layered,
cube, depth and non-sampled images are refused before allocation. Shared whole-image views
retain their parent, and a pool of 1024 nonempty groups and 2047 user view leases can be recycled.

GPU descriptors, record publication, pipeline binding and actual
sampling are pending. load_texture_group currently returns unsupported; this stage does not
claim draw-ready texture groups. Device/DLL roots must outlive all resource references.

Validation: Release full build; test_dynamic_link 252 checks; runtime device 1116 checks;
CTest 20/20. The runtime tests cover 1024 groups sharing 16 views, pool exhaustion/recovery,
slot 15, empty groups, caller image release and same-tag foreign-device input.

An incremental build embedded ABI 30 in a runtime comparison while formatting ABI 31 in
its log. Disassembly confirmed the stale comparison. Rebuilding the promise PCM/BMI
artifacts inside the build directory and doing a full build resolved it.

The project disables C++ exceptions. Explicit pool exhaustion and nothrow object allocations
have error results; STL control-block/container allocation failures retain the existing
process-termination behavior. Native view creation failure currently returns operation_failed.

## Recording and submission stage

Owned commands retain secondary command allocations. A referenced secondary cannot be
overwritten; pending primary commands cannot be recorded or submitted again before completion
is observed. Each owned submission has an internal fence; actual frame submission snapshots
references against its slot timeline. Completion polling releases references outside registry
locks, and device teardown clears them before native cleanup.

The validation-enabled device probe queues a timeline wait, submits a primary executing a
secondary that writes 0x11223344, drops both caller references, then signals the wait. GPU
readback matches the literal value. The probe passes 22 checks with no VUID/error log.
It also exposed and fixed the existing NULL inheritance-info case for a plain secondary.

Validation: runtime device 1124/1124; CTest 20/20; frozen rendering 14/14 matched,
with validation/log checks passed. Group retention by load and GPU sampling remain pending.

## Heap sampling stage

pipeline_desc::texture_group declares the reserved uint token offset and shader stages.
SPIR-V reflection validates its type, offset, stage coverage and device push limits.
Binding an enabled pipeline initializes the empty group; load retains the group, and portable
push writes cannot overlap its token. Registry checks reject foreign groups/commands/pipelines.

The backend publishes 80-byte records and full-mip sampled views in an independent heap window:
table slot 17664, texture base 17665. The originally planned slot overlapped the existing legacy
array, so this window is separately reserved and checked against capacity. Slang padding uses
uint[3]; uint3 would align the record to a 96-byte stride and was caught by GPU readback.

The validation-enabled probe passes 89 checks, including primary and secondary sampling behind
a timeline gate, caller image/group/command release, new group creation while earlier work waits,
A/B/empty groups, slot 15, failed load preserving state and pipeline switching. Release build,
runtime device 1124 checks, CTest 20/20 and frozen rendering 14/14 pass.

Records are host-coherent and written before submission. Visibility follows Vulkan's
[Host Write Ordering Guarantees](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-submission-host-writes);
load does not insert a HOST barrier inside dynamic rendering. Actual draw integration remains pending.

## Scope correction: API binding unification

User clarification: Vulkan requires no backwards-compatible descriptor-array path.
The common assign/load contract unifies API backend operations; Vulkan uses only descriptor heap.
The uncommitted Vulkan array experiment was withdrawn. The GPU table now contains 1025 records
(empty + 1024 groups), removing the second record reserved per group. Descriptor capacity remains
2048 views including dummy; its 2049-descriptor reservation includes the table descriptor.
Other API backend mappings are future work; ordinary graphics probe and unlit integration remain pending.

Scope correction validation: full Release build, CTest 20/20, texture-group GPU 89/89,
runtime device 1124/1124. User also requests local merge into master and future work directly
on master. Existing tracked probe-DLL deletion remains outside this change.
Frozen rendering regression: 14/14 matched with validation/log checks passed.
Pre-merge independent review found no important or critical defect in the texture-group changes.
