module;
#include "allocation_failure.hpp"
#if defined(DEREN_MEMORY_TESTING)
#include "memory_test_hooks.hpp"
#endif

#include <mimalloc.h>

module deren.utility.shared;

namespace deren::utility::memory {
    std::expected<void*, allocation_error> try_allocate(std::size_t bytes, std::size_t alignment) noexcept {
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
            return std::unexpected(allocation_error::invalid_alignment);
        }
        if (bytes > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
            return std::unexpected(allocation_error::size_overflow);
        }
#if defined(DEREN_MEMORY_TESTING)
        if (utility_memory_test_should_fail()) {
            return std::unexpected(allocation_error::out_of_memory);
        }
#endif
        // A successful zero-size allocation still has storage and can be paired with deallocate.
        void* const result = mi_malloc_aligned(bytes == 0 ? 1 : bytes, alignment);
        if (!result) {
            return std::unexpected(allocation_error::out_of_memory);
        }
        return result;
    }

    void deallocate(void* pointer) noexcept {
        mi_free(pointer);
    }

    [[noreturn]] void allocation_failure(allocation_error error, std::size_t bytes, std::size_t alignment) noexcept {
        char const* reason = "out_of_memory";
        if (error == allocation_error::invalid_alignment) {
            reason = "invalid_alignment";
        } else if (error == allocation_error::size_overflow) {
            reason = "size_overflow";
        }
        detail::terminate_allocation(reason, bytes, alignment);
    }

    void* allocate(std::size_t bytes, std::size_t alignment) noexcept {
        auto const result = try_allocate(bytes, alignment);
        if (!result) {
            allocation_failure(result.error(), bytes, alignment);
        }
        return *result;
    }
} // namespace deren::utility::memory
