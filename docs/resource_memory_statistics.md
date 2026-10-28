# Resource memory statistics

Shared resource and memory measurement used by [amnis](amnis.md), [subtilis](subtilis.md) and other clients.
GPU snapshots cover an api_core's managed allocations; vessel snapshots cover the shared project allocator.
Neither attributes allocations to an individual feature. Feature comparisons require controlled runs with
other settings held constant.

```cpp
auto gpu = core.resources->memory_statistics();
if (gpu.supported) {
    auto buffer_bytes = gpu.buffers.current_device_local_bytes;
    auto image_bytes = gpu.images.current_device_local_bytes;
    auto live_bytes = gpu.total.current_device_local_bytes;
    auto peak_bytes = gpu.total.peak_device_local_bytes;
}
auto cpu = deren::utility::vessel::memory_statistics();
auto live_cpu_bytes = cpu.current_bytes;
auto peak_cpu_bytes = cpu.peak_bytes;
```

All sizes are bytes. GPU `buffers`, `images`, and `total` also expose
`current_bytes`/`peak_bytes` for all memory types, plus
`current_allocations`/`peak_allocations`. Device-local memory can also be host-visible;
select the device-local fields when comparing resource VRAM allocations.

Vulkan records successful VMA allocations and actual destruction, including internal buffers.
Shared image handles/views count once. Unloaded lazy resources count zero until materialized;
retained/in-flight resources continue to count until actually freed. Peaks last for the core's
lifetime, and the combined peak measures concurrent usage rather than adding category peaks.

`allocator_allocation_bytes` reports VMA's current allocated bytes;
`allocator_reserved_bytes` and `allocator_device_local_reserved_bytes` report backing-block
capacity, including free space cached by VMA. These are separately sampled from the coherent
resource counters, so concurrent operations can cause temporary differences. The query walks
VMA metadata; use a monitoring cadence rather than querying per draw. Unsupported backends
return `supported == false`.

These figures exclude driver allocations and borrowed native swapchain images, and do not
represent OS GPU residency. Allocation sizes include alignment requirements, not just image
pixels or buffer payload.

CPU statistics track actual mimalloc usable bytes through the shared project allocator.
They include vessel capacity, allocation rounding and other explicit project allocator clients.
They exclude unrelated standard allocations, process RSS and mimalloc reserved pages. Cross-DLL
and cross-thread allocations/frees share the counters. See [vessel](vessel.md) for concurrency
and allocation-policy details. Neither API resets lifetime peaks.

The Vulkan backend also logs GPU and vessel current/peak usage during teardown. At that point
GPU current usage normally reaches zero; the recorded peaks remain useful.
