# Thread pool completion and shutdown design

User approved the concrete small-change proposal on 2026-10-25.

Keep wait_until_priority_done(priority) as a live drain of all queued/running tasks at that priority,
including later submissions before the predicate observes zero. It is not a batch fence or snapshot;
unrelated priorities do not block it. wait_until_free observes the entire pool empty and idle.

Queue entries own callable pointers allocated through utility::memory::allocator (the vessel backing allocator). Construct/copy callables
outside access_mutex; heap operations move pointers only. Release the worker-owned callable outside
access_mutex before publishing completion. Caller-owned copies (notably post_batch input) are outside this guarantee. Discarded queued work must also release
worker-owned captures before its pending count reaches zero, outside the pool lock.

Require positive worker count using the existing utility panic precondition path. Empty callable:
post returns false; any empty callable in post_batch rejects the entire batch without enqueuing.
An empty batch stays a successful no-op, including after shutdown, preserving existing behavior.

shutdown closes admission under access_mutex and requests stop without joining. Tasks already claimed
by a worker may finish; wait policy drains queued work, discard skips queued work. Destruction joins.
Concurrent post/wait/shutdown is supported while the object lives; destruction must not race public
calls, and workers/capture destructors must not synchronously wait for their own completion or destroy
their own pool. No restart, batch token, task graph or scheduling redesign.

Approaches considered: truthful live-drain semantics (chosen, fits current runtime calls); actual
snapshot sequence tracking (extra out-of-order completion bookkeeping); explicit batch tokens (useful
later if callers need isolated groups, requires new API). No RHI ABI change.
