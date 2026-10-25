#pragma once
#include <cstdint>
#if defined(_MSC_VER) && defined(DEREN_LOG_TESTING)
#define LOG_TEST_API __declspec(dllexport)
#elif defined(_MSC_VER)
#define LOG_TEST_API __declspec(dllimport)
#else
#define LOG_TEST_API __attribute__((visibility("default")))
#endif
// Test-build only; no callbacks or public production logging API.
extern "C" LOG_TEST_API void utility_log_test_pause_after(int writes) noexcept;
extern "C" LOG_TEST_API std::uint64_t utility_log_test_pause_count() noexcept;
extern "C" LOG_TEST_API std::uint64_t utility_log_test_snapshot_count() noexcept;
extern "C" LOG_TEST_API void utility_log_test_before_write() noexcept;
extern "C" LOG_TEST_API void utility_log_test_did_snapshot() noexcept;
