# Lazy unload acceptance (ABI 57)

Explicit unload preserves the shared uint8_t source, immutable metadata and logical object identity.
It releases this object's physical allocation reference and restores unloaded; native demand reloads
the initial data. No background eviction, residency manager or scene migration is included.

The first implementation is conservative and nonblocking. Live logical views, groups, fixed slots,
recorded commands and pending submissions refuse ready-resource unload with not_ready. Release/reset
those dependencies first, even after a reusable recording's GPU work completes. Shared ownership alone
does not block unload. Failed resources retain their saved failure. VMA blocks/shared allocations can
remain allocated after success; immediate process VRAM reduction is not guaranteed.

## Review and regressions

Independent review found radix sorting bypassed lazy demand and recording leases. The regression
failed twice before the fix (tmp/lazy-unload-sort-red.log); sorting now demands all operands before
native-handle reads. Tests also cover view/group/slot release, raw heap rejection, pending-only
submission ownership, completed reusable recording ownership and source-preserving reload.

## Verification

- Full Release build with14 workers: exit0 (tmp/lazy-unload-full-build.log).
- CTest71/71: tmp/lazy-unload-ctest.log.
- Device lane8/8: tmp/lazy-unload-device-probes.json. Eager group358/0; lazy group369/0.
- Dedicated validation-enabled lazy group369/0; shader readback73/255/1/255/41 unchanged.
  Logs in tmp/lazy-unload-acceptance contain no VUID/Validation Error/SYNC-HAZARD diagnostics.
- git diff --check clean. Existing uncommitted work retained; no commit or push.

Tests exercise the shared gate's serialization and failure states; no device-loss injection, TSan,
multithreaded Vulkan stress or allocator block-count measurement is claimed.
