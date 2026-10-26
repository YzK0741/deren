# Lazy Resources Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans inline and a fresh final review.

**Goal:** Shared uint8_t source data is uploaded only at native command demand.
**Architecture:** Stable registered Vulkan resource objects retain immutable shared CPU payloads;
one materialization resolver publishes physical storage, and logical views/descriptors defer resolution.
**Tech Stack:** C++23 modules, Vulkan, VMA, existing operation stream and submission retention.
**Spec:** docs/superpowers/specs/2026-10-26-lazy-resources-design.md (approved; uint8_t correction).

## Global constraints
Use shared_ptr<const vessel::vector<uint8_t>> only; no payload copy, mutable source or callback.
No production scene migration or upload-policy switch; no automatic eviction, commits or push.
Work in current checkout to preserve preceding uncommitted work; builds -j14.

## Review focus
Source lifetime after caller release; concurrent demand; deferred views/groups; barriers before bind;
failed/unsupported uses never producing invalid native commands or submitted partial recordings.

## Tasks
1. RHI API and source validation (rhi.api_core.cppm, contract, test_resource_group).
   - [x] Add tests for source validation/type/state and observe RED.
   - [x] Add uint8_t source, descriptors, lazy interfaces and factories; advance ABI and fixtures.
   - [x] Build focused CPU contract tests and observe GREEN.
2. Vulkan materialization (new core.lazy_resources.cpp, core.declarations.cppm, allocation helpers).
   - [x] Device tests: unloaded creation, metadata queries/views, shared owner, first demand once.
   - [x] Implement serialized state and error publication, buffer/image creation and view resolution.
3. Bindings and retention (render operations, groups, descriptor space, barriers, lifetimes).
   - [x] Add supported bindings/device data and rejected-use tests before implementing integration.
   - [x] Defer descriptor creation, resolve on command demand, retain resources across submissions.
   - [x] Test superseded binds, logical writes, frame/independent/mergeable command paths.
4. Acceptance and standard.
   - [x] Update RHI standard/dynamic compatibility and document limited capability profile.
   - [x] Full build, CPU checks, focused validation-enabled device probes and fresh final review.

## Ledger
User approved implementation with uint8_t sources. No further design gate.
Existing benchmark/bounded-file changes are unrelated and must remain intact.

Completed: RHI ABI 56; Release full build -j14; CTest 71/71; device lane expanded to 8 probes including
test_resource_group --with-device --lazy. Final counts: normal group277/0, lazy group281/0; remaining
device counts recorded in tmp/lazy-device-probes.json. Final dedicated lazy validation run is under
tmp/lazy-acceptance-final (run.log + deren-debug.log).

Independent final review found duplicate texture publication, premature native reads, unchecked upload
results, legacy image range validation and lazy host-readback escape. All five addressed in one pass.
New shared loading gate has deterministic two-thread exactly-once and terminal-failure headless tests;
upload sequence tests inject begin/end/submit/wait/reset errors and verify no wait after failed submit.
The invalid image barrier test was observed RED (3 failed checks) then GREEN. No real device-loss injection
or multithreaded Vulkan recording stress is claimed; headless tests cover the exact shared gate used by
buffers, images and texture groups. State tests do not measure VMA backing-block allocation counts.

Ruling: after a successful upload submission, an unconfirmed completion (failed wait without device
loss) uses fast_panic instead of releasing resources. This matches existing fatal final-wait handling;
the cost is process termination rather than recoverable resource quarantine. Physical helper refusals
otherwise latch operation_failed. Upload policy (host image copy versus staging) is unchanged.
