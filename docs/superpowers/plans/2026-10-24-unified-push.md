# Unified push implementation plan

> Execute inline with superpowers:executing-plans; independent final review.

**Goal:** One public recorder shader-parameter push, independent of descriptor storage.
**Architecture:** Direct push_constants on the common recorder, backend-native immediate recording,
existing capacity query and protected resource-group fields; ABI 52.
**Spec:** docs/superpowers/specs/2026-10-24-unified-push-design.md
**Tech stack:** C++ modules, Vulkan descriptor heap, CMake/Ninja, CPU probe backend.

- [x] Add API/range failing tests and verify red.
- [x] Implement contract/backend push, effective capacity, remove old heap push API/property.
- [x] Migrate CPU probe, engine and tests; preserve byte packing and ownership semantics.
- [x] Build -j14, test headless/device/windowed paths, review and fix findings.
- [x] Update current API/manual documentation and verification record.

## Review focus

Before/after recording; empty/unaligned/overflow ranges; protected-byte overlap; mergeable forwards to
its actual recorder; effective capacity matches implementation; frame support; no borrowed temporary
bytes; no redundant public heap entry; GUI native workaround unchanged.

## Execution record

Implemented inline in the existing checkout. One successful full build and full CTest/device/windowed
verification after migration, with independent read-only review. Actual test counts and boundaries are
recorded in docs/reviews/2026-10-24-unified-push.md. Lazy state remains explicitly deferred.
