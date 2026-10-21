module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>
module deren.asset.subtilis.builder;

namespace deren::asset::subtilis::builder_detail {
    namespace {
        auto fail(error_code code, char const* message) {
            return std::unexpected(error{code, message, {}, {}});
        }
    } // namespace

    result<void> run_ordered(size_t count, uint32_t threads, std::span<size_t const> estimates, size_t max_known_work_bytes, task_fn prepare, commit_fn commit) {
        if (estimates.size() != count)
            return fail(error_code::invalid_options, "workspace estimates must match the task count");
        if (threads == 0)
            return fail(error_code::invalid_options, "thread count must be at least 1");
        if (count == 0)
            return {};
        if (!prepare || !commit)
            return fail(error_code::invalid_options, "task callbacks are required");
        size_t max_estimate = 0;
        for (auto estimate : estimates)
            max_estimate = std::max(max_estimate, estimate);
        if (max_estimate == 0)
            max_estimate = 1;
        // Unified workspace budget: one maximum-size task must fit, and only as many maximum-size
        // tasks as the known-work budget can hold may run at once. The cap is a pure function of
        // the estimates and the budget, so it is reproducible instead of completion-order derived.
        if (max_estimate > max_known_work_bytes)
            return fail(error_code::capacity, "known workspace budget cannot hold one reduction task");
        size_t const by_budget = max_known_work_bytes / max_estimate;
        size_t const concurrency = std::min({size_t(threads), count, by_budget});
        // Coarse tasks first for load balance; index ties stay ascending. This only changes dispatch
        // order, never commit order or any visible ID.
        vector<size_t> order(count);
        std::iota(order.begin(), order.end(), size_t(0));
        std::stable_sort(order.begin(), order.end(), [&](size_t left, size_t right) {
            return estimates[left] != estimates[right] ? estimates[left] > estimates[right] : left < right;
        });
        if (concurrency <= 1) {
            // Task too small to parallelize, or threads=1: stay on the calling thread, like the
            // serial reference path. Prepare-then-commit order remains index order.
            for (auto index : order) {
                auto prepared = prepare(index);
                if (!prepared)
                    return prepared;
            }
        } else {
            std::mutex mutex;
            size_t next = 0;
            bool cancelled = false;
            vector<std::optional<error>> failures(count);
            vector<std::thread> workers;
            workers.reserve(concurrency);
            for (size_t worker = 0; worker < concurrency; ++worker) {
                workers.emplace_back([&] {
                    for (;;) {
                        size_t index = 0;
                        {
                            std::lock_guard guard(mutex);
                            if (cancelled || next == order.size())
                                return;
                            index = order[next++];
                        }
                        auto prepared = prepare(index);
                        if (!prepared) {
                            std::lock_guard guard(mutex);
                            failures[index] = prepared.error();
                            cancelled = true;
                            return;
                        }
                    }
                });
            }
            for (auto& worker : workers)
                worker.join();
            for (size_t index = 0; index < count; ++index)
                if (failures[index])
                    return std::unexpected(*failures[index]);
        }
        // Commits stay on the calling thread and always run in ascending ID order, never in task
        // completion order and never interleaved with concurrent prepares (callers may append to
        // shared output that other tasks read).
        for (size_t index = 0; index < count; ++index) {
            auto committed = commit(index);
            if (!committed)
                return committed;
        }
        return {};
    }
} // namespace deren::asset::subtilis::builder_detail
