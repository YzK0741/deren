module;

#include <cstdint>

export module deren.utility:thread_pool;
export import deren.utility.shared;
export import deren.vstd;

/**
 * @ingroup utility
 * @defgroup thread_pool Thread Pool
 * @file thread_pool.cppm
 * @brief a module provides raii thread pool (deren::utility::thread_pool)
 *
 * @code {.cpp}
 * import deren.utility;
 *
 * int32_t main() {
 *     deren::utility::thread_pool pool(4);
 *
 *     auto task = [] {};
 *     pool.post(task);
 *     pool.post(task);
 *     pool.post(task);
 *     // Submit additional work as needed.
 *     pool.wait_until_free();
 * }
 *
 *
 * @endcode
 */

namespace deren::utility {
    /**
     * @brief thread pool class
     */
    export class thread_pool { // NOLINT
    public:
        /// How the pool treats tasks still queued or running when it is destroyed. PUBLIC because the
        /// constructor below takes one: a private type in a public signature compiles through the
        /// default argument, but callers cannot name a non-default policy - which silently made
        /// `discard` unreachable from outside the class.
        enum class shutdown_policy : uint8_t {
            discard,
            wait,
        };

    private:
        using action_type = std::function<void()>;
        struct action_deleter {
            void operator()(action_type* action) const noexcept {
                std::destroy_at(action);
                deren::utility::memory::allocator<action_type>{}.deallocate(action, 1);
            }
        };
        using action_pointer = std::unique_ptr<action_type, action_deleter>;
        // Construct outside access_mutex: even std::function's move may destroy a user callable.
        static action_pointer make_action(action_type action) {
            auto* storage = deren::utility::memory::allocator<action_type>{}.allocate(1);
            std::construct_at(storage, std::move(action));
            return action_pointer(storage);
        }
        struct task {
            int32_t priority = 0;
            // The heap key is priority only. Transfer only a pointer under the mutex:
            // moving std::function itself may leave a small-buffer callable in the source.
            mutable action_pointer action;
            bool operator<(task const& other) const noexcept;
        };

        // NOT `tasks`: post_batch takes a parameter of that name, and a parameter would hide the
        // member - MSVC /W4 reports C4458, which /WX turns into an error (clang's -Wshadow is off).
        deren::utility::vessel::priority_queue<task> task_queue;
        std::condition_variable cv;
        std::condition_variable idle;
        mutable std::mutex access_mutex; // mutable: is_free() const reads task_queue under the lock
        std::atomic_int active_thread = 0;
        // per-priority pending count: tasks posted with a given priority that are still queued
        // or running. post/post_batch increment it under the lock, workers decrement it when a
        // task finishes (or is discarded at shutdown), and wait_until_priority_done() blocks on
        // it reaching zero - so a caller can wait for "its" priority group without waiting for
        // unrelated tasks posted by other users of a shared pool.
        deren::utility::vessel::unordered_map<int32_t, std::size_t> pending_by_priority;
        shutdown_policy policy = shutdown_policy::wait;
        bool accepting = true; // protected by access_mutex, shared by submission and shutdown
        // Declared last so the jthreads are destroyed (auto-joined) FIRST, before the mutex /
        // condition variables above: worker threads still exiting would otherwise touch
        // already-destroyed synchronization state.
        deren::utility::vessel::vector<std::jthread> threads;

        void worker_loop(std::stop_token const& token);
        // Bookkeeping helpers (caller holds access_mutex): bookkeeping for one posted/finished task
        void note_task_posted(int32_t priority);
        void note_task_finished(int32_t priority);

    public:
        /**
         * @brief thread_pool's constructor
         * @param threads thread number you want create
         * @param policy behavior when tasks remain at destruction
         * @note threads must be positive (otherwise panic); hardware_concurrency is only a sizing hint.
         */
        explicit thread_pool(int32_t threads, shutdown_policy policy = shutdown_policy::wait);
        ~thread_pool();
        /**
         * @brief post a task to thread_pool, signature must be void()
         * @param task callable object
         * @param priority larger values are claimed first; equal-priority order is unspecified
         * @return false for an empty callable or a shut-down pool; nothing is queued
         */
        bool post(std::function<void()> task, int32_t priority = 0);
        /**
         * @brief post a batch of tasks at one priority in a single lock acquisition
         * @param tasks callable objects, all posted with @p priority (any order)
         * @param priority larger values are claimed first; equal-priority order is unspecified
         * @return false if any callable is empty or the pool is shut down; none are queued.
         *         An empty batch is always a successful no-op.
         * @note pair with wait_until_priority_done(@p priority) to run a group synchronously:
         *       this drains the priority, including work from other callers at the same priority.
         *       The caller retains its own copies of batch callables and their captures.
         */
        bool post_batch(std::span<std::function<void()>> tasks, int32_t priority = 0);
        /**
         * @brief stop accepting work and request worker shutdown; does not join
         * @note idempotent. wait policy executes queued tasks; discard skips queued tasks.
         *       Work already claimed by a worker may finish. Destruction joins all workers.
         *       Public calls may race shutdown, but must not race destruction. Never destroy
         *       the pool or synchronously wait for it from its own task/capture destructor.
         */
        void shutdown();
        /**
         * @brief check whether thread_pool is free
         * @return if free returns true, otherwise returns false
         */
        bool is_free() const;
        /**
         * @brief block current thread until all the tasks completed
         */
        void wait_until_free();
        /**
         * @brief block until no queued or running task at @p priority remains
         * @param priority the priority to drain, not a batch identifier
         * @note live-drain semantics: later submissions at this priority can extend the wait.
         *       Work at other priorities does not block this wait. The result is an observation
         *       under the pool lock; another caller may submit new work immediately afterward.
         *       Worker-owned captures are released before completion is published; caller-owned
         *       copies are unaffected. Discarded tasks count as complete without being executed.
         * @note returns immediately when no task at @p priority is pending.
         */
        void wait_until_priority_done(int32_t priority);
        /**
         * @brief get count of worker threads
         * @return number of threads this pool runs
         */
        [[nodiscard]] int32_t thread_count() const noexcept;
        /**
         * @brief get count of active thread
         * @return count of active thread
         */
        int32_t get_active_thread() const;
    };
} // namespace deren::utility

namespace deren::utility {
    bool thread_pool::task::operator<(task const& other) const noexcept {
        return this->priority < other.priority;
    }

    // Bookkeeping helpers: the caller holds access_mutex
    void thread_pool::note_task_posted(int32_t const priority) {
        ++this->pending_by_priority[priority];
    }

    void thread_pool::note_task_finished(int32_t const priority) {
        auto const it = this->pending_by_priority.find(priority);
        if (it != this->pending_by_priority.end() && --it->second == 0) {
            this->pending_by_priority.erase(it); // a zero entry is indistinguishable from "never posted"
        }
    }

    void thread_pool::worker_loop(std::stop_token const& token) {
        static_cast<void>(deren::utility::diagnostics::prepare_crash_thread());
        this->active_thread.fetch_add(1);
        action_pointer current_task;
        while (true) {
            int32_t current_priority = 0;
            bool execute = false;
            {
                std::unique_lock lock(this->access_mutex);
                this->active_thread.fetch_sub(1);
                if (this->active_thread.load() < 1) {
                    // all workers idle: wake EVERY wait_until_free/priority waiter - notify_one
                    // would let two concurrent waiters consume each other's notification (both
                    // predicates satisfied, no further wake -> permanent hang). Events are
                    // rare, so the thundering-herd cost is negligible.
                    this->idle.notify_all();
                }
                cv.wait(lock, [this, &token] {
                    return !this->task_queue.empty() || token.stop_requested();
                });
                this->active_thread.fetch_add(1);

                if (this->task_queue.empty()) {
                    this->active_thread.fetch_sub(1);
                    return;
                }

                current_task = std::move(this->task_queue.top().action);
                current_priority = this->task_queue.top().priority;
                execute = !token.stop_requested() || this->policy == shutdown_policy::wait;
                this->task_queue.pop();
            }
            if (execute) {
                (*current_task)();
            }
            // Release captures outside the mutex, BEFORE publishing completion. A capture's
            // destructor may query or submit to the pool. Discard uses this same completion path.
            current_task = nullptr;
            {
                // finished (or dropped above): the task's priority group made progress.
                // notify_all, not notify_one: several threads may wait on idle concurrently
                // (run_tasks on the frame thread + another consumer of the shared pool), and a
                // single notification could wake one waiter whose predicate is already true
                // while another waiter's predicate just became true - the second would hang
                // forever. Events are per-task (rare relative to wake cost), so notify_all.
                std::lock_guard lock(this->access_mutex);
                this->note_task_finished(current_priority);
                this->idle.notify_all();
            }
        }
    }

    thread_pool::thread_pool(int32_t const threads, shutdown_policy const policy) {
        if (threads <= 0) {
            deren::utility::panic("thread_pool: worker count must be positive");
        }
        this->threads.resize(static_cast<std::size_t>(threads));
        this->policy = policy;

        for (auto& thread : this->threads) {
            thread = std::jthread([this](std::stop_token const& stop_token) {
                thread_pool::worker_loop(stop_token);
            });
        }
    }

    bool thread_pool::post(std::function<void()> task, int32_t priority) {
        if (!task) {
            return false;
        }
        auto action = make_action(std::move(task));
        std::unique_lock lock(this->access_mutex);
        // after shutdown() every worker is leaving: queuing would sit forever unexecuted
        if (!this->accepting) {
            return false;
        }
        this->note_task_posted(priority);
        this->task_queue.emplace(priority, std::move(action));
        this->cv.notify_one();
        return true;
    }

    bool thread_pool::post_batch(std::span<std::function<void()>> const tasks, int32_t const priority) {
        if (tasks.empty()) {
            return true;
        }
        for (auto const& task : tasks) {
            if (!task) {
                return false;
            }
        }
        deren::utility::vessel::vector<action_pointer> actions;
        actions.reserve(tasks.size());
        for (auto const& task : tasks) {
            actions.push_back(make_action(task));
        }
        std::unique_lock lock(this->access_mutex);
        if (!this->accepting) {
            return false;
        }
        for (auto& action : actions) {
            this->note_task_posted(priority);
            this->task_queue.emplace(priority, std::move(action));
        }
        this->cv.notify_all();
        return true;
    }

    void thread_pool::shutdown() {
        {
            std::lock_guard lock(this->access_mutex);
            this->accepting = false;
            for (auto& thread : this->threads) {
                thread.request_stop();
            }
        }
        this->cv.notify_all();
    }

    thread_pool::~thread_pool() {
        this->shutdown();
    }

    bool thread_pool::is_free() const {
        std::lock_guard lock(this->access_mutex);
        // same predicate as wait_until_free(): idle workers with queued tasks are not "free"
        return this->task_queue.empty() && this->active_thread.load() == 0;
    }

    void thread_pool::wait_until_free() {
        std::unique_lock lock(this->access_mutex);
        this->idle.wait(lock, [this] { return this->task_queue.empty() && this->active_thread.load() == 0; });
    }

    void thread_pool::wait_until_priority_done(int32_t const priority) {
        std::unique_lock lock(this->access_mutex);
        this->idle.wait(lock, [this, priority] {
            return !this->pending_by_priority.contains(priority);
        });
    }

    int32_t thread_pool::thread_count() const noexcept {
        return static_cast<int32_t>(this->threads.size());
    }

    int32_t thread_pool::get_active_thread() const {
        return this->active_thread.load();
    }
} // namespace deren::utility
