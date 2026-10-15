// Plain TU: keep platform headers out of the module's global fragment.
#include "allocation_failure.hpp"

#include <atomic>
#include <cstdlib>
#if defined(DEREN_MEMORY_TESTING)
#include "memory_test_hooks.hpp"
#endif
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace deren::utility::memory::detail {
    [[noreturn]] void terminate_allocation(char const* reason, std::size_t bytes, std::size_t alignment) noexcept {
        static thread_local bool reporting = false;
        if (reporting)
            std::_Exit(EXIT_FAILURE);
        reporting = true;
        {
            char text[192];
            std::size_t count = 0;
            auto const append = [&](char const* value) {
                while (*value != '\0' && count < sizeof(text))
                    text[count++] = *value++;
            };
            auto const number = [&](std::size_t value) {
                char digits[3 * sizeof(std::size_t)];
                std::size_t used = 0;
                do {
                    digits[used++] = static_cast<char>('0' + value % 10);
                    value /= 10;
                } while (value != 0);
                while (used != 0 && count < sizeof(text))
                    text[count++] = digits[--used];
            };
            append("deren allocation failure: ");
            append(reason);
            append(" bytes=");
            number(bytes);
            append(" alignment=");
            number(alignment);
            append("\n");
#if defined(DEREN_MEMORY_TESTING)
            utility_memory_test_before_failure_write();
#endif
#if defined(_WIN32)
            DWORD written = 0;
            WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(count), &written, nullptr);
#else
            static_cast<void>(::write(STDERR_FILENO, text, count));
#endif
        }

        // No formatting, log queue, GPU wait, callbacks, retries or static destructors.
        std::abort();
    }
} // namespace deren::utility::memory::detail

#if defined(DEREN_MEMORY_TESTING)
namespace {
    thread_local std::int64_t allocation_countdown = -1;
    std::atomic<bool> pause_first_failure = false;
    std::atomic<bool> first_failure_ready = false;
    std::atomic<unsigned> failure_writers = 0;
} // namespace
extern "C" MEMORY_TEST_API void utility_memory_test_fail_after(std::int64_t count) noexcept {
    allocation_countdown = count;
}
extern "C" MEMORY_TEST_API bool utility_memory_test_should_fail() noexcept {
    if (allocation_countdown < 0)
        return false;
    if (allocation_countdown == 0) {
        allocation_countdown = -1;
        return true;
    }
    --allocation_countdown;
    return false;
}
extern "C" MEMORY_TEST_API void utility_memory_test_pause_first_failure() noexcept {
    pause_first_failure.store(true);
}
extern "C" MEMORY_TEST_API bool utility_memory_test_failure_ready() noexcept {
    return first_failure_ready.load();
}
extern "C" MEMORY_TEST_API void utility_memory_test_before_failure_write() noexcept {
    if (pause_first_failure.load() && failure_writers.fetch_add(1) == 0) {
        first_failure_ready.store(true);
        while (pause_first_failure.load()) {
        }
    }
}
#endif
