#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>

namespace deren::vulkan::detail {
    // No allocation or Vulkan call under this lock. One coherent counter snapshot covers both kinds.
    template<class Snapshot>
    class resource_memory_tracker {
        mutable std::mutex mutex;
        Snapshot value{};
    public:
        void add(bool image, std::uint64_t bytes, bool device_local) noexcept {
            std::lock_guard guard(mutex);
            auto increment = [&](auto& usage) {
                usage.current_bytes += bytes;
                usage.peak_bytes = std::max(usage.peak_bytes, usage.current_bytes);
                if (device_local) usage.current_device_local_bytes += bytes;
                usage.peak_device_local_bytes = std::max(usage.peak_device_local_bytes, usage.current_device_local_bytes);
                ++usage.current_allocations;
                usage.peak_allocations = std::max(usage.peak_allocations, usage.current_allocations);
            };
            increment(image ? value.images : value.buffers);
            increment(value.total);
        }
        void remove(bool image, std::uint64_t bytes, bool device_local) noexcept {
            std::lock_guard guard(mutex);
            auto decrement = [&](auto& usage) {
                usage.current_bytes -= bytes;
                if (device_local) usage.current_device_local_bytes -= bytes;
                --usage.current_allocations;
            };
            decrement(image ? value.images : value.buffers);
            decrement(value.total);
        }
        [[nodiscard]] Snapshot snapshot() const noexcept {
            std::lock_guard guard(mutex);
            return value;
        }
    };
}
