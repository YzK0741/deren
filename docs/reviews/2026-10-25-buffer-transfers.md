# Independent buffer transfer checkpoint

2026-10-25. RHI ABI remains 54. Implements the user-selected independent buffer path; frame completion
and screenshot migration remain separate work.

## Delivered

Engine module deren.engine.buffer_transfer provides a service, move-only batch and copyable ticket.
Accepted upload spans are copied before return; one batch records ordered copies and explicit pre/post
scopes into one independent graphics submission. Readbacks expose bytes only after token completion.
The service owns pending command buffers, original operand leases, coherent staging and readback storage,
even when callers discard owners/tickets. Completed tickets keep output/core alive and cache completion.
Collection is nonblocking; timed-out/error drains retain uncertain work. The user approved destructor drain
of this service's tokens only; failure of a final indefinite drain invokes fast_panic without formatting.

The service requires command_submission and scoped_synchronization. Native describe verifies actual core,
range and transfer capability instead of trusting a lease's core field. Borrowed shared handles can no
longer become alleged owning resource leases through from_shared. Empty-before scopes express first use;
after scopes must be explicit and nonempty. No raw native descriptors, device-wide idle or copy queue added.

Added docs/buffer_transfers.md to the manual and cross-referenced the standard/runtime overview. The
standard cleanup is recorded separately in 2026-10-25-rhi-standard-cleanup.md.

## Evidence

- Owning-lease regression: 53 checks/1 failure before guard, passing after guard; headless capability-refusal
  checks added to test_resource_cluster. No pending work is created without required capabilities.
- Full Release dynamic Clang64 build with 14 workers: passed (tmp/buffer-transfer-full-build.log).
- Full CTest: 69/69 (tmp/buffer-transfer-ctest.log).
- Device lane: 6/6, counts 221/95/75/12/65/65 (tmp/buffer-transfer-device-probes.json).
- New transfer probe: 65/0, zero Vulkan validation errors in tmp/buffer-transfer-device-debug.log. Tests
  verify upload snapshots, range/flag/core refusal, queue-gated not-ready/timeout, retained work after owner
  and ticket drop, per-service drain and output surviving service destruction.
- Documentation checks: 439/0. Changed C++ sources pass clang-format; git diff --check passes.
- Independent read-only design and implementation reviews completed; no important implementation issues.

## Limits and next step

Staging is allocated per operation and there is no real-consumer migration or measured performance claim
in this checkpoint. Fault-injected device loss, allocation exhaustion and concurrent service access are not
covered; callers serialize access, and existing backend fatal allocation paths remain unchanged. Service
exit may block as approved. Next choose a concrete independent upload/readback consumer, migrate it with
explicit state/lifetime handling, then measure before deciding on staging pooling. Frame screenshot wait_idle
must not be replaced with independent tokens pretending to represent frame completion.
