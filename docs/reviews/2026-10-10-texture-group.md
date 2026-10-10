# Texture group implementation progress

## Resource ownership stage

ABI 31 adds a 16-slot immutable texture_group, assign_texture_group and load_texture_group.
The default backend refuses both operations with unsupported. Vulkan implements the factory:
empty optionals and optional(nullptr) are empty slots; foreign-device, borrowed, layered,
cube, depth and non-sampled images are refused before allocation. Shared whole-image views
retain their parent, and a pool of 1024 nonempty groups and 2047 user view leases can be recycled.

GPU descriptors, record publication, pipeline binding, command/submission retention and actual
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
