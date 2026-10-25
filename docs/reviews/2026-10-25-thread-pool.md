# Thread pool completion and shutdown review

Date: 2026-10-25. Scope: approved small utility thread-pool repair, layered on the uncommitted
utility/outcome dependency split. RHI ABI remains 55. No commit or push requested.

## Behavior

- `wait_until_priority_done(p)` drains all queued/running work at priority p, including later
  submissions before zero is observed. It is not a snapshot or batch fence; other priorities do not
  block it. Equal-priority submissions from different callers share this wait condition.
- Completion includes releasing the pool-owned callable before decrementing pending counts and
  notifying waiters. Caller-owned copies of post_batch input remain caller-owned.
- Callable construction/copy and release occur outside access_mutex. The queue owns unique pointers
  to callables allocated through utility::memory::allocator, backed by the shared mimalloc service.
  Heap operations move only pointers, not user callables.
- `post` rejects an empty callable; `post_batch` rejects an entire nonempty batch containing any empty
  callable. Empty batches preserve their successful no-op behavior, including after shutdown.
- A worker count <= 0 panics with a named positive-count precondition diagnostic.
- `shutdown()` closes admission and requests stop under the submission mutex; it does not join.
  Wait policy drains queued work; discard skips unclaimed queued work. Already-claimed work may
  finish. Both paths release captures outside the mutex before publishing completion. Destruction
  joins workers before destroying synchronization members.

## Why indirect callable storage

Moving a std::function is insufficient to prevent user destructors under the queue lock. Local libc++
`__value_func` movement clones small-buffer targets and may leave the source populated; MSVC has
similar inline-target concerns. A callable whose destructor queries is_free reproduced a deadlock
with direct movable std::function queue storage. Owning pointers avoid this in heap relocation as
well as dequeue. Batch payloads are staged before locking, so user copy constructors can query the
pool too. Rejected staging is destroyed after the lock guard releases.

Tradeoff: one allocator-backed payload allocation per task and temporary pointer storage per batch.
No scheduling-throughput or rendering-performance improvement is claimed or benchmarked here.

## Verification

- RED against original implementation: 119 checks, 8 failures covering retained captures, invalid
  worker-count handling and accepted empty callables. The additional inline-callable destructor
  regression then reproduced a timeout and drove pointer-owned storage.
- Focused final test_utility: 147 checks, zero failures; 20 consecutive final runs passed. Tests cover
  worker-capture lifetime, copy/destructor reentrant queries, same-priority drain, unrelated-priority
  isolation, multiple waiters, whole-batch rejection, both shutdown policies, idempotent shutdown,
  post-shutdown rejection, destructor draining and invalid-count subprocess diagnostics.
- Full Release build with 14 workers: exit 0. Subsequent source changes were documentation comments only.
- Full CTest: 71/71 passed.
- Device probes: 7/7 passed; dynamic runtime with device: 1174 checks, zero failures, ABI 55.
- Independent read-only review identified the callable-move defect; final rereview found no actionable issues.
- CI tidy: exit 0; all 39 unique sources in the final gate covered. The run had 40 entries
  because thread_pool was redundantly listed twice; the duplicate entry was removed afterward.

## Evidence and limits

Local artifacts: tmp/thread-pool-{red-test,green-test,repeat-final,full-build,ctest,device,runtime-device}.log;
static checking uses tmp/ci-fix-tidy-primary-all.log. These temporary artifacts are not repository docs.

Concurrency tests use semaphores to hold tasks at known phases. A bounded future wait checks that a
held same-priority task prevents completion; a five-second deadline detects unrelated-priority
blocking. This is not a ThreadSanitizer proof; no local sanitizer rebuild or remote CI is claimed.
Public calls may race shutdown while the object lives, but destruction must not race public calls.
Tasks/capture destructors must not wait for their own completion or destroy their own pool.
No batch tokens, task graphs, work stealing, worker-count tuning or file-I/O changes were introduced.
