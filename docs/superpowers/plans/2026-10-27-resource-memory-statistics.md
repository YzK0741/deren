# Resource memory statistics implementation plan

> **For agentic workers:** Use superpowers:executing-plans inline in the existing feature checkout.

**Goal:** Query actual current/peak managed GPU allocations and shared vessel allocator usage.

**Architecture:** RHI POD snapshots, VMA allocation/destruction hooks, shared mimalloc usable-byte
atomic counters. Existing resource owners determine destruction; aliases do not drive counting.

**Spec:** ../specs/2026-10-27-resource-memory-statistics-design.md
**Tech Stack:** C++23 modules, Vulkan/VMA, mimalloc, existing CPU/device test harnesses.

## Global constraints
- Preserve previous changes/user files. Resource stats are observational and do not force allocation.
- Actual allocation bytes and device-local bytes differ from logical payload and WDDM usage.
- CPU tracking lives once in shared_utility; no allocation from the tracking hooks.
- RHI ABI 59; default unsupported snapshots for unimplemented backends.

## Review focus
- Failure/unload/view/dedup paths must balance exactly and retain lifetime peaks.
- Combined peaks must reflect concurrent live totals, not independent class peak sums.
- Track hidden staging buffers and failed-upload cleanup, not only public resource handles.
- CPU allocation failures, zero allocations, alignment and cross-thread/DLL frees remain correct.
- Statistics and VMA locking must not deadlock upload/render threads or recurse into vessel.

## Tasks
- [x] Add failing tests for CPU allocator stats and GPU counter aggregation.
- [x] Implement shared usable-byte tracking and public vessel snapshot forwarding.
- [x] Add RHI POD/method, ABI update, VMA allocation/free tracking and backend query.
- [x] Add actual-device create/free/view/dedup/lazy tests and run relevant regressions.
- [x] Full build/CTest/device validation; independent review and documentation.

## Ledger
- User directly authorized implementation of resource and vessel statistics. Proceed inline with
  writing-plans/TDD, preserving the current feature branch and unrelated files.
- Evidence and review: ../../reviews/2026-10-27-resource-memory-statistics.md.
