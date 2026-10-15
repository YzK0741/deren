#pragma once
#include <cstdint>
// Only linked when VR_BUILD_TESTS is enabled. No public utility API or production callbacks.
#if defined(_MSC_VER) && defined(DEREN_MEMORY_TESTING)
#define MEMORY_TEST_API __declspec(dllexport)
#elif defined(_MSC_VER)
#define MEMORY_TEST_API __declspec(dllimport)
#else
#define MEMORY_TEST_API __attribute__((visibility("default")))
#endif
extern "C" MEMORY_TEST_API void utility_memory_test_fail_after(std::int64_t successful_allocations) noexcept;
extern "C" MEMORY_TEST_API bool utility_memory_test_should_fail() noexcept;
extern "C" MEMORY_TEST_API void utility_memory_test_pause_first_failure() noexcept;
extern "C" MEMORY_TEST_API bool utility_memory_test_failure_ready() noexcept;
extern "C" MEMORY_TEST_API void utility_memory_test_before_failure_write() noexcept;
