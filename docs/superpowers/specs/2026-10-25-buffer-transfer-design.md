# Independent buffer transfer service

User selected buffer-only transfers on the existing graphics submission/token path. Frame completion and
screenshot migration are deferred. ABI stays 54; no RHI interface, queue or descriptor model is added.

## Shape

Engine module deren.engine.buffer_transfer, alongside resource_cluster. A buffer_transfer_service is
constructed with a shared RHI core; a move-only buffer_transfer_batch records descriptions of uploads and
readbacks; submit consumes it and returns expected<buffer_transfer_ticket, error>. A ticket can poll/wait
and access each readback span only after completion. API members use ordinary unadorned names.

Each operation takes an owning resource_lease<buffer>, byte range and explicit before/after synchronization::scope.
Upload accepts span<const byte> and copies it into coherent staging before returning; source storage can
immediately be reused. Download returns an index into this batch's readback outputs. Empty copies, overflow,
out-of-range input and foreign-core leases are refused before appending. No data-dependent state guessing.

One submit records all copies and necessary buffer barriers into one caller-owned primary and returns one
existing submission_token. Each transfer is ordered, with destination/source pre/post transitions and a
host_read barrier for readback storage. Source/destination flags must permit transfer; backend validation
remains authoritative. A failed recording is never submitted. The initial implementation allocates staging
per operation rather than pooling. Empty batches are rejected.

## Ownership and completion

The service retains pending submission state, including the core, command owner, all operand leases, staging
and readback allocations. Dropping a ticket does not release in-flight dependencies. collect polls and
releases completed service records; tickets keep completed readback storage alive. A batch not submitted
has no GPU work and can be discarded immediately. No public raw buffer pointer is accepted as an owner.

Service destruction drains only its own pending tokens, never device-wide wait_idle. Explicit drain reports
errors; timeout leaves records retained. Destructor cannot safely drop pending resources on completion
failure, so uses the existing fatal-error policy if a final indefinite drain fails. All calls involving one
service/batch/ticket are caller-serialized; no background thread. The service cannot race destruction with
recording/submission. Returned spans live while their ticket lives; no mutable mapped API is exposed.

## Alternatives and constraints

Chosen: engine composition over existing command_submission and resource_cluster, starting with buffer
copy workflows. Alternative: a new transfer RHI extension would duplicate existing copy/token functionality.
Frame completion is intentionally separate because current tokens do not represent acquired-frame work.
The user approved waiting for service-owned submissions at destruction; it may block on GPU work.

Do not change synchronous create_buffer semantics, change screenshot behavior, add aliasing/copy queues,
or claim performance gains without measurements. Keep existing workspace changes and ABI 54.

## Verification

Headless: invalid/unsupported setup, range validation without side effects, upload input copied at enqueue,
failed append preserves previous operations. Device: upload plus download in one submission; queue gate
proves not-ready/timeout, retention after caller/ticket drop, no readback before completion; collect/drain,
foreign core, failures without a submission, ticket surviving service destruction after completion.

Builder-review rulings: require scoped_synchronization plus command_submission. Describe operands on the
service core before append, checking canonical provenance/range and transfer usage. Use precise scopes for
vertex/indirect as well as shader consumers; empty before means no prior access, after must be nonempty.
Validate transition groups before append, and use a host_read role barrier for the internal readback buffer.
Reject borrow() in resource_lease::from_shared so the owning-lease promise is true. Explicit drain timeout
is per pending token. Pending registry insertion precedes native submit; completed ticket state is cached.
