#include "log_test_hooks.hpp"

#include <atomic>
#include <thread>
namespace {
    std::atomic<int> remaining{-1};
    std::atomic<std::uint64_t> pauses{0}, snapshots{0};
} // namespace
extern "C" LOG_TEST_API void utility_log_test_pause_after(int writes) noexcept {
    remaining.store(writes);
}
extern "C" LOG_TEST_API std::uint64_t utility_log_test_pause_count() noexcept {
    return pauses.load();
}
extern "C" LOG_TEST_API std::uint64_t utility_log_test_snapshot_count() noexcept {
    return snapshots.load();
}
extern "C" LOG_TEST_API void utility_log_test_did_snapshot() noexcept {
    ++snapshots;
}
extern "C" LOG_TEST_API void utility_log_test_before_write() noexcept {
    for (;;) {
        int count = remaining.load();
        if (count < 0)
            return;
        if (count > 0) {
            if (remaining.compare_exchange_weak(count, count - 1))
                return;
            continue;
        }
        ++pauses;
        while (remaining.load() == 0)
            std::this_thread::yield();
    }
}
