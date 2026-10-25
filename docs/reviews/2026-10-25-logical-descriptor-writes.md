# Logical descriptor writes checkpoint

Date: 2026-10-25. RHI ABI 55, selected and approved by the user.

## Delivered behavior

descriptor_space now supports a separate optional fixed shader-slot profile: fixed_slots(), write_image(),
write_buffer(), write_acceleration_structure() and bind(). Requests contain logical shader slots, portable
objects and resource-relative ranges. Vulkan resolves physical descriptor offsets and resource addresses
internally. Existing shader indices, resource_group and texture_group generation semantics are preserved.

Vulkan exposes [16384,18432), disjoint from native reserved windows and dynamic group storage beginning at
32768. Headers, profile/space/slot, live same-core resources, declared usage, ranges and alignment are
checked before encoding. AS provenance has its own live-object registry; foreign handles are refused
before downcasts. Uniforms satisfy logical uniform constraints while using native storage descriptors for
the current shader ABI. Default fixed-profile methods refuse unsupported, independently of group support.

All runtime constructor/frame/RT and self-test bindings now use descriptor_space. The software-visibility
pass's direct writes also migrated. A source boundary check covers these runtime units, including probes,
and rejects physical descriptor offset helpers/types. Shader-slot arithmetic, resource byte ranges, indirect
command offsets and genuine geometry device-address consumers remain valid and unchanged.

The low-level descriptor_heap service remains available outside these consumers. This is a fixed shader
profile, not dynamic layout registration or arbitrary layout virtualization. Host writes borrow resources;
callers synchronize replacement, serialize updates and retain operands until GPU completion. No hidden
descriptor versioning, retention or barriers were added. No FPS gain is claimed.

## Validation history

- New device checks first failed to compile because fixed_slots/write_buffer did not exist.
- Boundary regression first failed (469 checks, 10 failed) against remaining physical accesses, then passed.
- Independent review found probe bindings omitted from both migration and boundary coverage. Adding that
  file produced 475 checks/1 failure; migrating its two bindings fixed it.
- Full regression exposed the CPU tracking backend's missing fixed profile. Its logical methods now forward
  to the existing test tracking/failure-injection layer; this fixture is not a complete validation backend.
  Focused retests passed: Subtilis frame 4436/0, runtime injection 96/0.
- Additional runtime device testing exposed a stale test assertion: command_context::submit rejects a
  borrowed frame command by the existing contract. The test now verifies this refusal and uses
  frames->submit(), matching production runtime; production submission behavior was not modified.
- Independent implementation review and follow-up found no remaining important issues.

## Final evidence

- Complete Release dynamic Clang64 build with 14 workers: passed (tmp/logical-descriptor-final-build.log).
- Full CTest: 69/69 (tmp/logical-descriptor-final-ctest.log).
- Optional device lane: 7/7, counts 221/116/75/12/65/65/83; summary in
  tmp/logical-descriptor-final-device-probes.json. This includes texture-group rendering/binding,
  fixed buffer subrange and range/header/type/foreign-object refusals, sampled/storage images, AS publication,
  graph-frame and Gaussian GPU probes. AS-positive checks depend on ray-query capability.
- Actual runtime construction/teardown and frame-submit device probe: 1174/0
  (tmp/logical-descriptor-final-runtime-device.log). The probe's compute/graphics self-tests use logical bind.
- Archived device/runtime debug logs contain zero VUID/validation errors:
  tmp/logical-descriptor-final-device-debug.log and tmp/logical-descriptor-final-runtime-debug.log.
- Documentation/source-boundary checks: 475/0 (tmp/logical-descriptor-docs-final.log).
- Changed C++ sources pass clang-format; git diff --check passes. Independent review and its follow-up
  completed; the important probe-migration finding was fixed and tested RED to GREEN.

No new large-scene performance run, screenshot image comparison, concurrent update stress, driver-loss or
allocation-failure injection was performed. The CPU fixture preserves tracking/failure injection and does
not independently prove the full native validation contract. Existing caller synchronization/lifetime
obligations remain essential. Changes are left uncommitted in the existing workspace.
