# RHI standard and implementation conformance checkpoint

Date: 2026-10-24. Base: `ddbd972e`, plus local changes. ABI remains 53.

## Agreed initialization rule

The user selected exact initialization: `size` must be nonzero; empty initial bytes mean allocation only
with undefined contents; otherwise the byte count must equal `size`. Both shorter and longer inputs are
refused. The shared validator checks this shape; support for uploading a particular memory intent remains
a separate backend decision. Initial bytes are consumed before the factory returns.

## Changes

- Aligned the standard with borrowed-resource ownership, caller-serialized recording, ABI rules and
  validation ordering. Replaced resolved historical questions with current rules and references.
- Documented intermediate/final application, error latching, rendering scopes, reuse restrictions and
  the Vulkan `prepare_native` / `finish_native` protocol. Logical pipeline/group/push state survives
  internal native binding changes, including sorting; borrowed operands remain the caller's responsibility.
- Unified buffer initialization validation in Vulkan and the CPU probe. Allocation-only vertex/index/GPU
  uniform creation no longer stages a null source pointer. Initialized allocation-only storage/AS roles
  now return empty before allocation rather than entering the allocator's fatal upload-failure path.
- Reject unmatched `end_rendering` and attempts to close an inherited scope with a latched error. The CPU
  probe also rejects nested `begin_rendering`, matching Vulkan.
- Added shared buffer and scope contract checks, including recovery with a fresh recording. Extended the
  device resource-group test with native push-state clobber followed by a compatible pipeline switch;
  GPU readback confirms that the copied logical push/group state is restored.
- Corrected the Bloom test's missing recording/pipeline setup. DLL lifetime tests now load the absolute
  path of the file they inspect, avoiding accidental loading of a same-name copy beside the executable.
- Updated the optional device-probe minimum for `test_resource_group` from 158 to 221 checks.

## Verification

Red tests demonstrated short/zero buffer acceptance in the CPU probe, a Vulkan allocation-only null-copy
crash, unmatched-end acceptance, and nested-begin acceptance before the corresponding fixes.

Final verification:

- Full Release dynamic build, Clang64, 14 workers: passed.
- Headless CTest, 14 workers: **69/69 passed**, including documentation and native/backend boundary checks.
- Device probes: **5/5 passed**, with no `VUID-` or `Validation Error` entries in each captured device log.
  Check counts: resource group **221**, texture group **95**, submission completion **63**, render graph
  frame **12**, Gaussian GPU frame **65**.
- Resource-group readback: **73 255 1 255 41** after native push clobber and compatible pipeline switch.
- Changed C++ sources and new helper headers pass clang-format; `git diff --check` passes.
- Independent read-only review found stale per-method clauses contradicting the clarifications; these
  were corrected and re-reviewed without further important findings.

Local evidence (not checked in): `tmp/rhi-standard-final-build4.log`,
`tmp/rhi-standard-conformance-ctest-final2.log`, `tmp/rhi-standard-conformance-device-probes.json`,
and the per-probe `tmp/rhi-standard-conformance-*-debug.log` captures.

## Limits and remaining work

This is targeted contract coverage, not proof of full backend conformance or a performance measurement.
The CPU probe remains a basic ABI/state double; it does not model every Vulkan native-state or retention
rule. The new restoration readback covers native interop and a compatible pipeline switch, not every
secondary-execution or sorting combination. Reserved ABI 53 query/swapchain factories remain empty;
remaining contract choices are listed in section 9.2 of the standard. No additional ownership-retention
policy or ABI change is introduced here.
