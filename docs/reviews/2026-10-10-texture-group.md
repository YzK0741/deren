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
