# Independent buffer transfers

The engine module `deren.engine.buffer_transfer` batches buffer uploads and readbacks on the existing
independent graphics submission path. It adds no RHI virtuals, queues or ABI change (current RHI ABI 59).
It requires both `command_submission` and `scoped_synchronization`; an unavailable mechanism returns
`unsupported`. Acquired-frame submission and screenshot readback are outside this service.

## API and ownership

- `buffer_transfer_service(core)` holds an owning core. `make_batch()` returns a move-only batch.
- `batch.upload(lease, offset, bytes, before, after)` copies accepted input bytes into coherent staging
  before returning. The caller may then change/free that input span. Empty uploads are invalid.
- `batch.readback(lease, offset, size, before, after)` returns a batch-local output index. Size must be nonzero.
- `service.submit(std::move(batch))` records the operations in order into one independent primary command
  buffer and returns `expected<buffer_transfer_ticket, rhi::error>` with one completion receipt.
- `ticket.poll()` never waits for GPU completion. `ticket.wait(timeout_ns)` uses the receipt's existing
  timeout rules. `ticket.readback(index)` polls and returns a read-only span only after completion.

A lease must own a resource on the service's core; `resource_lease::from_shared` rejects `borrow()`.
Backend `describe()` additionally verifies actual provenance, byte size and declared transfer capability;
a lease's caller-supplied core is not trusted as evidence of provenance. Offset/size validation avoids
addition overflow and happens before appending. Invalid input leaves previously accepted operations intact.
The source must support transfer-source for readback and the destination transfer-destination for upload.

The service retains pending command owners, all input leases, staging and readback allocations. Dropping
caller leases or the receipt does not cancel work or release those dependencies early. Successful completion
is cached; completed tickets retain their readback output but release other transfer operands/staging.
Returned spans borrow the ticket's storage and must not be used after its last owning ticket is destroyed.

## Explicit state and submission boundaries

`before` and `after` are `rhi::synchronization::scope` values, not native API masks. An empty `before` means
no previous access, useful for a fresh allocation; `after` must name a nonempty stage/access scope. The
service validates pre/post transitions and inserts copy dependencies. Internal readback buffers receive a
transfer-write -> host-read dependency before submission completes.

The caller describes the actual state, including relationships to other submissions. No state is guessed,
no frame is implicitly acquired and no global device idle is used. If several operations name one buffer,
the supplied scopes must describe their sequential accesses. The service does not authorize concurrent CPU
writes or unrelated GPU uses of the same range.

A nonempty same-core batch is consumed when submission recording starts, including on a later recording
or submit failure. A failed recording is not submitted. Preflight refusal may leave the batch intact;
create a fresh batch when recovering from a consumed failure. Empty batches are invalid.

## Collection and exit

All access to a service and tickets sharing its submission state is caller-serialized; there is no worker
thread or concurrent poll/read/destruction guarantee.

- `collect()` polls tracked submissions and removes completed records; pending work is normal and returns
  `ok`. `pending_count()` counts records not yet observed complete, not an instantaneous GPU queue metric.
- `drain(timeout_ns)` waits for each tracked receipt. The timeout applies to EACH token, not one shared
  deadline. A timeout/error retains every still-uncompleted record for retry; no uncertain work is freed.
- Service destruction performs an indefinite drain of its own submissions only. This may block. If final
  completion cannot be established, the existing fatal-error policy terminates rather than freeing resources
  that the GPU might still access. Explicit drain allows errors to be handled before destruction.
- Completed tickets may outlive the service and keep their output/core alive. Unsubmitted batches have no
  GPU work and can be discarded directly.

## Example

The caller already owns `core` and a destination `resource_lease<rhi::buffer>` created with transfer-source
and transfer-destination flags. Error handling is abbreviated here; check each returned result in real code.

```cpp
import deren.engine.buffer_transfer;
namespace rhi = deren::promise::rhi;
namespace sync = rhi::synchronization;

deren::engine::buffer_transfer_service transfers(core);
auto batch = transfers.make_batch();
sync::scope transfer_read{sync::stage::transfer, sync::access::read};
auto uploaded = batch.upload(destination, 0, payload_bytes, {}, transfer_read);
if (uploaded != rhi::error::ok) { /* handle refusal */ }
auto output = batch.readback(destination, 0, payload_bytes.size(), transfer_read, transfer_read);
if (!output) { /* handle refusal */ }
auto request = transfers.submit(std::move(batch));
if (!request) { /* handle refusal */ }
// Poll on later host iterations; a pending readback returns not_ready, never partial data.
if (request && output && request->poll() == rhi::error::ok) {
    auto bytes = request->readback(*output);
    if (bytes) { /* consume while request remains alive */ }
}
```

This first version allocates staging per operation and batches command submission; it is not a staging
ring, copy-queue scheduler or frame-capture replacement. Pooling can follow after profiling consumers.

## subtilis geometry consumer

`subtilis::upload_geometry` now uses this service for its immutable vertex/index pair. The default
`geometry_upload_mode::automatic` selects one batch when the core owns its lifetime and exposes both
required extensions; otherwise it retains synchronous creation with initial data. Explicit `synchronous`
and `batched` modes support comparisons. Forced batching without capabilities returns `unsupported`;
an attempted batch failure propagates rather than silently retrying another path.

The batch creates allocation-only buffers, leases their resource generations, submits both copies once,
and waits for its token before returning the uploaded owner. Callers therefore retain the ready-on-return
contract. Available vertex-input/shader readers receive explicit visibility; the acceleration-structure
build entry retains its own transfer-write-to-build-read barrier. No RHI ABI change is needed.

Separate batched uploads bypass the backend's initial-content cache: retain/share the returned
`uploaded_geometry` to share an asset. Existing resource-chain destruction can still wait for device idle;
this migration does not remove lifecycle-wide waits. The local upload latency measurements behind it are
not distributed with the repository, and they are not frame-rate improvements.

## Verification

Headless `test_resource_cluster` checks that borrowed handles cannot become owning leases and that a core
without the required extension refuses transfers without pending work. `test_buffer_transfer --with-device`
checks a batch roundtrip and copied upload input, invalid ranges/flags/core, queue-gated not-ready/timeout,
pending ownership after receipt/lease drop, service drain and output surviving service destruction.
The optional Windows device-probe lane includes this executable. These tests do not inject device loss,
allocator exhaustion or arbitrary concurrent access.
`test_subtilis_transfer --with-device` additionally compares all three consumer modes, reads back both
payloads byte for byte, checks the single upload receipt, and builds a BLAS when ray-query support exists.
Its optional `--benchmark` measures upload-call-to-ready latency with validation disabled.
