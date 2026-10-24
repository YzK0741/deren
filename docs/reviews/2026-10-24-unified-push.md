# Unified push implementation - 2026-10-24

## Result

RHI ABI 52 exposes one immediate shader-parameter operation:
`command_recorder::push_constants(uint32 offset, span<byte const>) noexcept -> error`.
Primary, mergeable and acquired-frame recorders use the same vocabulary. The public descriptor-heap
push verb, heap_push_info request and max_push_data property are removed, with no compatibility
forwarder. The old structure_type value remains reserved. Generic overflow-safe range validation
now lives beside the base error contract as validate_push_constants_range.

Vulkan still uses vkCmdPushDataEXT privately. Device capabilities report the active mechanism's
usable push capacity, or zero if its heap/entry point is unavailable. Engine mesh availability reads
this single budget instead of mixing native constants and heap limits. Engine stage packing is
unchanged; raw pushes defer range checking to the recorder rather than an arbitrary 256-byte wrapper.

Native recording-state, alignment, empty/overflow/range validation and protected resource-group /
texture-token fields remain enforced. Internal group publication retains its private write path.
The span is consumed before return. CPU probe capture is now recorder state, independent of its heap
extension. All engine and test consumers were migrated; GUI heap workaround and ownership stay intact.

## Verification

- Red contract checks failed on missing recorder method/helper and presence of the old heap method.
- Full Release build passed with 14 jobs (`tmp/push-unification-full-build3.log`).
- Full headless CTest: 69/69 passed (`tmp/push-unification-ctest.log`). CPU injection/capture checks
  exercise recorder pushes through the migrated runtime path.
- Resource-group device test: 134 checks, zero failures, zero validation errors. New coverage checks
  empty/unaligned/out-of-range/last-valid writes on primary, both secondary routes and frame recorder;
  before/after recording refusals; source mutation after push (GPU still writes original output lane).
  Existing mixed resource readback, storage-image content and group lifetime checks remain passing.
- Texture-group device test: 95 checks, zero failures, zero validation errors, including legacy token
  protection through primary and mergeable recorder pushes.
- Completion device test: 63 checks; frame device test: 12 checks; both pass validation-clean.
- Gaussian/PBR windowed captures: exit 0, fresh captures, zero validation errors. GPU sort diagnostic
  reports PASS 48 cases. Normal startup renders repeatedly and exits gracefully.
- Fresh independent review found no actionable correctness issues.
- git diff --check passed (pre-existing GUI line-ending notice only).

Initial migration build caught an accidentally renamed local in the probe's unrelated heap-image
failure hook; that local was restored before the successful full build. No failed run is counted
as passing. Device evidence is separated into tmp/push-unification-test_*.log and matching validation
logs; scene/startup evidence is in tmp/push-unification-smoke and tmp/push-unification-smoke.log.

## Boundaries

No lazy recording, dirty-state cache, adjacent-write coalescing or changes to draw/dispatch return
types. No alternate classic Vulkan constant implementation is added: this backend's pipelines remain
heap-native; the front-end operation allows other backends to implement their own mechanism.
No performance claim, native failure/entry-point fault injection, or blanket claim about pre-existing
unrelated device-suite failures. Unsupported mock implementations retain explicit unsupported returns;
a virtual call on a foreign mock executes that mock rather than a Vulkan extension with an explicit
target operand. Heap-bind and synchronization provenance tests remain.

Core/resource ownership and pipeline-lifetime preconditions are unchanged. No commit/push/publication.
