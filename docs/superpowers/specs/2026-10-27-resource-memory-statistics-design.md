# Resource and vessel memory statistics

Implement the user's requested current/peak buffer/image allocation statistics through
api_core.resources, and current/peak vessel allocator usage in the shared mimalloc service.

GPU API: resource_factory::memory_statistics() returns a non-owning POD snapshot with supported flag,
buffers/images/combined usage (current/peak bytes, current/peak device-local bytes and allocation
counts), plus current VMA reserved block/allocation bytes. Default backends report unsupported.
Vulkan tracks successful physical VMA buffer/image allocations and actual destruction, including
backend scratch/staging/descriptor buffers. Dedup aliases, handles and views do not allocate twice;
unmaterialized lazy objects and borrowed swapchain images add no allocation. Device-local uses VMA
memory type properties, not host visibility or logical payload size. Total peaks are recorded at
allocation events, not summed from disjoint per-kind peaks. Querying does not allocate or upload.
Resource vtable addition requires RHI ABI 58 -> 59 and fixture/document updates.

Snapshots are per-device allocator lifetime. Buffer/image/total counters share a mutex for coherence;
VMA block statistics are sampled separately and may differ transiently during concurrent allocation.
Reserved blocks, Vulkan resource allocations, driver allocations and WDDM process usage are different
quantities. No claim that these counters measure all physical GPU residency.

CPU API: memory::statistics() and vessel::memory_statistics() return process-wide shared-service
current/peak usable bytes and allocation counts. Track mi_usable_size at successful allocation/free,
including alignment/size-class rounding and reserved container capacity. Count all users of the
project allocator, including its allocate_shared users, not unrelated std allocations, stack arrays,
mimalloc reserved pages or process RSS. Null/failure paths do not change usage. Atomic hot-path
counters avoid per-allocation maps, headers, recursive allocations or DLL-local tracker duplication.
Support cross-thread/DLL frees. Peaks are lifetime-monotonic; no reset API in this increment.

Keep all previous uncommitted work. No commit, merge or push. Acceptance: failing-then-passing CPU
tests, physical GPU tests for create/free/view/dedup/lazy allocation and peak retention, full build,
complete CTest and device validation. Document APIs, accounting boundaries and evidence.
