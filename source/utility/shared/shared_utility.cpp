module;

#include "stacktrace_native.hpp"

#include <cstdint>
#include <cstdio> // std::snprintf: format the log prefix without allocating on the worker
#include <ctime>  // std::tm, localtime_s / localtime_r
#if defined(DEREN_LOG_TESTING)
#include "log_test_hooks.hpp"
#endif
#include <cstdlib> // std::atexit, for the sink's drain at exit (see instance())
// The process-wide rotation claim is declared in a header rather than here: a global module fragment
// may only carry preprocessing directives, and MSVC enforces that (C5202 at /W4, fatal under /WX).
#include "log_rotation_claim.hpp"

module deren.utility.shared;
import deren.vstd;

// ============================================================================
// THE PROCESS-WIDE STATE, AND NOTHING ELSE (see shared_utility.cppm for the whole rationale).
//
// The sink is a file-local class here - it is not declared in the interface at all, because the
// interface's users reach it through `log_text` / `error_text` / `wait_log_all`. That is a design
// choice, not a restriction: this module's interface MAY carry STL (batch ④'s relaxed rules - see
// shared_utility.cppm), and keeping the class private is simply the smallest surface that serves the
// three doors.
// ============================================================================
namespace deren::utility {
    std::uint64_t allocate_identity() noexcept {
        static std::atomic<std::uint64_t> next{1};
        auto value = next.fetch_add(1, std::memory_order_relaxed);
        if (value == 0 || value == UINT64_MAX) {
            fast_panic("process identity space exhausted");
        }
        return value;
    }

    namespace {
        enum class log_level : std::uint8_t { info,
                                              warning,
                                              error,
                                              verbose };

        constexpr char const* log_level_name(log_level const level) noexcept {
            switch (level) {
            case log_level::warning:
                return "WARNING";
            case log_level::error:
                return "ERROR";
            case log_level::verbose:
                return "VERBOSE";
            case log_level::info:
            default:
                return "INFO";
            }
        }

        struct log_entry {
            std::chrono::system_clock::time_point timestamp = {};
            std::uint64_t thread = 0;
            log_level level = log_level::info;
            std::string_view function = {};
            deren::utility::vessel::string text = {};
        };

        // Local time, because a log is read by the person sitting at the machine. localtime_s and
        // localtime_r are the thread-safe spellings; gmtime is the fallback for an out-of-range time.
        void format_timestamp(char* const buffer, std::size_t const size,
                              std::chrono::system_clock::time_point const timestamp) noexcept {
            auto const whole = std::chrono::floor<std::chrono::seconds>(timestamp);
            auto const millis =
                std::chrono::duration_cast<std::chrono::milliseconds>(timestamp - whole).count();
            std::time_t const raw = std::chrono::system_clock::to_time_t(whole);
            std::tm tm = {};
            bool have_time = false;
#if defined(_WIN32)
            have_time = ::localtime_s(&tm, &raw) == 0;
            if (!have_time) {
                have_time = ::gmtime_s(&tm, &raw) == 0;
            }
#else
            have_time = ::localtime_r(&raw, &tm) != nullptr;
            if (!have_time) {
                have_time = ::gmtime_r(&raw, &tm) != nullptr;
            }
#endif
            if (!have_time) {
                std::snprintf(buffer, size, "0000-00-00 00:00:00.000");
                return;
            }
            std::snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%03lld", tm.tm_year + 1900,
                          tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                          static_cast<long long>(millis));
        }

        void format_log_prefix(char* const buffer, std::size_t const size, log_entry const& entry) noexcept {
            char timestamp[32];
            format_timestamp(timestamp, sizeof timestamp, entry.timestamp);
            // Padding sits OUTSIDE the level's brackets: the bracket itself is never stretched, so
            // "[INFO]", "[WARNING]" and "[ERROR]" keep their written form while "[tid ...]" starts in
            // the same column. Width 7 is the widest level name; the extra space is the separator.
            auto const* const level = log_level_name(entry.level);
            int const padding = 7 - static_cast<int>(std::string_view(level).size()) + 1;
            std::snprintf(buffer, size, "[%s][%s]%*s[tid %016llx]", timestamp, level, padding, "",
                          static_cast<unsigned long long>(entry.thread));
        }

        // Some call sites wrote their level into the message before the sink owned one. Absorb those
        // markers so the line carries one level, not two; the payload after the marker is unchanged.
        void apply_manual_level(std::string_view& text, log_level& level) noexcept {
            struct marker {
                std::string_view prefix;
                log_level level;
            };
            constexpr marker markers[] = {
                {"[ERROR] ", log_level::error},
                {"[WARNING] ", log_level::warning},
                {"[INFO] ", log_level::info},
                {"[VERBOSE] ", log_level::verbose},
                {"WARNING: ", log_level::warning},
            };
            for (auto const& marker : markers) {
                if (text.starts_with(marker.prefix)) {
                    level = marker.level;
                    text.remove_prefix(marker.prefix.size());
                    return;
                }
            }
        }

        std::uint64_t current_thread_token() noexcept {
            return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        }

        // Startup rotation for the log file: move the previous session's deren-debug.log content aside to
        // deren-debug.log.old (with a session-end timestamp when the content carries none), then truncate
        // deren-debug.log so the new session starts fresh. Runs for every build type.
        //
        // ONCE PER PROCESS, AND THAT IS A GUARD RATHER THAN A COMMENT: the claim below is an
        // OS-named object keyed by the process id (log_rotation_claim.cpp), so if two copies of this
        // module's code ever live in one process - exactly what linking the toolkit statically into
        // the executable AND a backend DLL would do the day deren_vulkan becomes SHARED - the second
        // copy does NOT rotate. Without the guard that second rotation TRUNCATES the file the first
        // copy still has open and appends to: interleaved lines, holes, and two sessions in one
        // deren-debug.log.old. The claim costs one already-created handle per process and nothing per run.
        [[maybe_unused]] void rotate_previous_log() {
            if (utility_shared_claim_log_rotation() == 0) {
                return; // another copy of the sink in this process already rotated
            }
            // Text mode on both sides: the read translates CRLF to LF, the text-mode write
            // translates LF back to CRLF, so line endings stay consistent with deren-debug.log
            std::ifstream current_log("deren-debug.log");
            if (!current_log) {
                return; // no previous log yet
            }
            current_log.seekg(0, std::ios::end);
            if (current_log.tellg() <= 0) {
                return; // empty, nothing to rotate
            }
            current_log.seekg(0, std::ios::beg);

            deren::utility::vessel::string const content((std::istreambuf_iterator<char>(current_log)), std::istreambuf_iterator<char>());
            current_log.close();

            // Cap deren-debug.log.old: once it exceeds the cap, start it fresh (truncate) instead of
            // appending forever, so the archive stays bounded across many sessions.
            constexpr uintmax_t old_log_cap = 8ull * 1024ull * 1024ull; // 8 MiB
            std::ios::openmode const old_mode = [&] {
                std::error_code ec;
                uintmax_t const size = std::filesystem::file_size("deren-debug.log.old", ec);
                return (!ec && size >= old_log_cap) ? (std::ios::out | std::ios::trunc) : (std::ios::out | std::ios::app);
            }();
            std::ofstream old_log("deren-debug.log.old", old_mode);
            if (!old_log) {
                return;
            }

            // Timestamp the rotated block so sessions are distinguishable in deren-debug.log.old
            if (!content.contains("===== session")) {
                char timestamp[32];
                format_timestamp(timestamp, sizeof timestamp, std::chrono::system_clock::now());
                old_log << "===== session ended at " << timestamp << " =====\n";
            }

            // Normalize to one newline after every physical line, matching the worker's
            // deren-debug.log shape (each entry is a metadata line followed immediately by its body).
            deren::utility::vessel::istringstream lines(content);
            deren::utility::vessel::string line;
            while (std::getline(lines, line)) {
                if (!line.empty()) {
                    old_log << line << '\n';
                }
            }
            old_log.close();

            // Start the new session with an empty deren-debug.log
            std::ofstream fresh_log("deren-debug.log", std::ios::out | std::ios::trunc);
            fresh_log.close();
        }
    } // namespace

    // --------------------------------------------------------------------------------------------
    // WHY `log_sink` IS NOT IN THE ANONYMOUS NAMESPACE ABOVE although it is just as private: Doxygen
    // does NOT extract the members of an anonymous namespace (Doxyfile: EXTRACT_ANON_NSPACES = NO, the
    // doxygen default), so every doc block in that class was a DANGLING documentation block and the
    // manual build reported five warnings - "documented symbol 'deren::utility::log_sink::log_sink' was not
    // declared or defined" for the constructor, the destructor, `worker_loop`, `write` and `wait_all`.
    // The class is still NOT exported (it is not in the module interface and nothing outside this image
    // can name it); what changed is only that it has a name Doxygen can see, so the sink stays DOCUMENTED
    // instead of the comments being deleted to silence a warning. The helpers that need no documentation
    // stay in the anonymous namespace above, and the two that follow (instance / write_text) keep theirs.
    // --------------------------------------------------------------------------------------------

    /**
     * @brief the process's one log sink: a queue, its worker, and the file handle
     *
     * ASYNCHRONOUS BY CONSTRUCTION, and the two condition variables are load-bearing:
     * `queue_cv` wakes the worker, `drained_cv` wakes `wait_all()`. `panic()` does NOT call
     * wait_all(): it writes the emergency report first and polls the lock-free prefix for at most
     * 250 ms, so a busy ordinary sink cannot hang the fatal path.
     */
    class log_sink { // NOLINT
        std::mutex queue_mutex = {};
        std::condition_variable queue_cv = {};
        std::condition_variable drained_cv = {}; // notifies when the queue has been drained
        deren::utility::vessel::queue<log_entry> messages = {};
        std::atomic<std::uint64_t> submitted{0}, completed{0};
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
        std::thread worker = {};
        std::atomic<bool> running = true;
        // Whether write() still enqueues. Cleared by the destructor BEFORE it signals the worker
        // to drain and exit, and read under queue_mutex - so a write that arrives during (or
        // after) teardown is dropped rather than queued for a worker that is already gone. That
        // matters because wait_all() would then block forever on a pending count nothing will
        // ever decrement. panic() does not wait on that count - it uses the bounded lock-free prefix
        // poll - so teardown cannot hang the fatal path.
        bool accepting = true;
        std::ofstream file = {}; // every build writes to deren-debug.log

    public:
        log_sink();
        ~log_sink();
        void worker_loop() noexcept;

        log_sink(log_sink const&) = delete ("log_sink is non-copyable: pass it by reference or construct a new instance");
        log_sink& operator=(log_sink const&) = delete ("log_sink is non-copyable: modify the existing instance instead");

        /// Queue one entry. TAKES A VIEW, and the copy the queue stores is made on THIS side - which
        /// is what lets a caller hand over its own temporary string without either side owning the
        /// other's allocation. That is a design choice rather than a rule (this module's interface
        /// may carry STL: see shared_utility.cppm), and it is the shape the three doors settled on.
        /// `timestamp`, the caller's thread and the caller's function name are captured before the
        /// caller's formatted string reaches this function, so the entry identifies when and where
        /// the event happened.
        void write(std::string_view text, std::chrono::system_clock::time_point timestamp, bool error,
                   std::string_view function);
        void wait_all();
        void wait_prefix_for(std::chrono::milliseconds budget) noexcept;
        /// Drain the queue, stop the worker and close the file. Idempotent; called from the
        /// atexit handler instance() registers, because the sink itself is never destroyed.
        void shutdown() noexcept;

    private:
        // The worker's own thread id, published when it starts: wait_all() must recognise it (see
        // there). A default-constructed id belongs to no thread, so this reads "not the worker"
        // until the thread is running.
        std::atomic<std::thread::id> worker_id = {};
    };

    namespace {
        std::atomic<log_sink*> active_sink{nullptr};
        log_sink& instance() noexcept {
            // NEVER DESTROYED, ON PURPOSE. This is the process's one sink and panic() reaches it -
            // the path that must not hang. A function-local static IS destroyed at exit, and a
            // log()/error()/panic() arriving after that destruction (a static destructor ordered
            // after this one, or any atexit handler registered later) would lock a destroyed mutex
            // and signal a destroyed condition variable: undefined behaviour, not the "message
            // dropped during teardown" the accepting gate promises. The object outlives the
            // process instead, and shutdown() is registered here: atexit runs in the same reverse
            // order a static destructor would, so the queue is still drained before the process
            // ends, and every write after it is dropped by the accepting gate.
            static log_sink* const sink = [] {
                auto* value = new log_sink;
                active_sink.store(value, std::memory_order_release);
                return value;
            }();
            // The comma operator registers the drain and keeps the result as a plain bool; the
            // registration is deliberately the LAST thing done, so nothing here can re-enter
            // instance() while its own guard is still being initialised.
            [[maybe_unused]] static bool const drains_at_exit = (std::atexit(+[] { instance().shutdown(); }), true);
            return *sink;
        }

    } // namespace

    log_sink::log_sink() {
        rotate_previous_log();
        this->file.open("deren-debug.log", std::ios::out | std::ios::app);
        this->worker = std::thread([this] { this->worker_loop(); });
    }

    // Never runs: instance() does not destroy the sink. It is kept so the drain has exactly one
    // implementation, and so a sink constructed on the stack (nothing does today) still behaves.
    log_sink::~log_sink() {
        this->shutdown();
    }

    void log_sink::shutdown() noexcept {
        {
            // Stop accepting BEFORE waking the worker: a message enqueued after the worker has drained
            // and exited would leave `pending` above zero forever, and wait_all() (called by panic())
            // would then block the shutdown it is supposed to complete.
            std::lock_guard lock(this->queue_mutex);
            this->accepting = false;
            this->running = false;
        }
        this->queue_cv.notify_all();
        if (this->worker.joinable()) {
            this->worker.join(); // wait for the worker to drain the queue before exiting
        }
        if (this->file.is_open()) {
            this->file.close();
        }
    }

    void log_sink::worker_loop() noexcept {
        static_cast<void>(diagnostics::prepare_crash_thread());
        this->worker_id.store(std::this_thread::get_id(), std::memory_order_relaxed);
        while (true) {
            log_entry message;
            {
                std::unique_lock lock(this->queue_mutex);
                // Keep waiting for messages; drain the queue before exiting
                this->queue_cv.wait(lock, [this] { return !this->running || !this->messages.empty(); });
                if (this->messages.empty()) {
                    if (!this->running) {
                        break;
                    }
                    continue;
                }
                message = std::move(this->messages.front());
                this->messages.pop();
            }
            // Write/flush outside the queue lock. Failure falls back only to the emergency file.
#if defined(DEREN_LOG_TESTING)
            utility_log_test_before_write();
#endif
            char prefix[96];
            format_log_prefix(prefix, sizeof prefix, message);
            auto const prefix_size = std::string_view(prefix).size();
            if (this->file) {
                this->file << prefix;
                if (!message.function.empty()) {
                    this->file << ' ' << message.function;
                }
                this->file << '\n'
                           << message.text << '\n'
                           << std::flush;
            }
            if (!this->file) {
                detail::trace_native::write_text(prefix, prefix_size);
                if (!message.function.empty()) {
                    detail::trace_native::write_text(" ", 1);
                    detail::trace_native::write_text(message.function.data(), message.function.size());
                }
                detail::trace_native::write_text("\n", 1);
                detail::trace_native::write_text(message.text.data(), message.text.size());
                detail::trace_native::write_text("\n", 1);
            }
            {
                std::lock_guard lock(this->queue_mutex);
                this->completed.fetch_add(1, std::memory_order_release);
                this->drained_cv.notify_all(); // prefix waiters may finish while newer work remains
            }
        }
    }

    void log_sink::write(std::string_view text, std::chrono::system_clock::time_point const timestamp,
                         bool const error, std::string_view const function) {
        log_level level = error ? log_level::error : log_level::info;
        if (error) {
            // Keep exactly one [ERROR] marker even if the caller already wrote one.
            if (text.starts_with("[ERROR] ")) {
                text.remove_prefix(8);
            }
        } else {
            apply_manual_level(text, level);
        }
        log_entry message{timestamp, current_thread_token(), level, function,
                          deren::utility::vessel::string(text)}; // the copy the queue owns is made on THIS side
        {
            std::lock_guard lock(this->queue_mutex);
            if (!this->accepting) {
                // The sink is shutting down and its worker will not drain anything else: drop the
                // message instead of queueing it for nobody. Late writes are the normal case, not an
                // error - static destructors ordered after this singleton still call log()/error().
                return;
            }
            if (this->submitted.load(std::memory_order_relaxed) == UINT64_MAX) {
                fast_panic("log sequence space exhausted");
            }
            this->messages.push(std::move(message));
            this->submitted.fetch_add(1, std::memory_order_release);
        }
        this->queue_cv.notify_one();
    }

    void log_sink::wait_all() {
        // The worker must never wait for the queue to drain: it IS the drain. panic() reaches this
        // from whatever thread hit the fatal condition, and a panic ON the log worker would
        // otherwise block on its own pending count forever - in the one path that must not hang.
        // The write it queued is still written: it is this thread's own loop that will write it.
        if (this->worker_id.load(std::memory_order_relaxed) == std::this_thread::get_id()) {
            return;
        }
        std::unique_lock lock(this->queue_mutex);
        auto const target = this->submitted.load(std::memory_order_acquire);
#if defined(DEREN_LOG_TESTING)
        utility_log_test_did_snapshot();
#endif
        this->drained_cv.wait(lock, [this, target] {
            return this->completed.load(std::memory_order_acquire) >= target;
        });
    }
    void log_sink::wait_prefix_for(std::chrono::milliseconds budget) noexcept {
        if (this->worker_id.load(std::memory_order_relaxed) == std::this_thread::get_id()) {
            return;
        }
        auto const target = this->submitted.load(std::memory_order_acquire);
        auto const deadline = std::chrono::steady_clock::now() + budget;
        while (this->completed.load(std::memory_order_acquire) < target &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield(); // no logger mutex, condition variable or lazy initialization
        }
    }

    void log_text(std::string_view const text, std::source_location const location) noexcept {
        instance().write(text, std::chrono::system_clock::now(), false, location.function_name());
    }

    void log_text(std::string_view const text, std::chrono::system_clock::time_point const timestamp,
                  std::source_location const location) noexcept {
        instance().write(text, timestamp, false, location.function_name());
    }

    void error_text(std::string_view const text, std::source_location const location) noexcept {
        instance().write(text, std::chrono::system_clock::now(), true, location.function_name());
    }

    void error_text(std::string_view const text, std::chrono::system_clock::time_point const timestamp,
                    std::source_location const location) noexcept {
        instance().write(text, timestamp, true, location.function_name());
    }

    void wait_log_all() noexcept {
        instance().wait_all();
    }

    [[noreturn]] void panic(std::string_view const msg, std::source_location const source_location) noexcept {
        static thread_local bool reporting = false;
        if (reporting) {
            fast_panic("recursive panic");
        }
        reporting = true;
        detail::trace_native::write_panic_header(msg.data(), msg.size(), source_location.file_name(),
                                                 source_location.function_name(), source_location.line());
        auto const trace = diagnostics::capture_stack();
        diagnostics::write_stack(trace);
        auto const readable = diagnostics::symbolize(trace);
        if (!readable.text.empty()) {
            detail::trace_native::write_text("call stack:\n", 12);
            detail::trace_native::write_text(readable.text.data(), readable.text.size());
        }
        // The emergency report is already written. Give only an EXISTING sink a bounded chance
        // to flush its prefix, without acquiring its locks or initializing a logger on this path.
        if (auto* sink = active_sink.load(std::memory_order_acquire)) {
            sink->wait_prefix_for(std::chrono::milliseconds(250));
        }

        std::terminate();
    }
} // namespace deren::utility