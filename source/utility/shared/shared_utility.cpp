module;

#include <cstdint>
#include <cstdio>  // stderr/stdout below need the macros (they cannot cross a module boundary - see deren.utility::standard_output)
#include <cstdlib> // std::atexit, for the sink's drain at exit (see instance())
// The process-wide rotation claim is declared in a header rather than here: a global module fragment
// may only carry preprocessing directives, and MSVC enforces that (C5202 at /W4, fatal under /WX).
#include "log_rotation_claim.hpp"

module deren.utility.shared;

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
        // Startup rotation for the Release log file: move the previous session's debug.log content
        // aside to debug.log.old (with a session-end timestamp when the content carries none), then
        // truncate debug.log so the new session starts fresh. Only called in Release builds (NDEBUG).
        //
        // ONCE PER PROCESS, AND THAT IS A GUARD RATHER THAN A COMMENT: the claim below is an
        // OS-named object keyed by the process id (log_rotation_claim.cpp), so if two copies of this
        // module's code ever live in one process - exactly what linking the toolkit statically into
        // the executable AND a backend DLL would do the day deren_vulkan becomes SHARED - the second
        // copy does NOT rotate. Without the guard that second rotation TRUNCATES the file the first
        // copy still has open and appends to: interleaved lines, holes, and two sessions in one
        // debug.log.old. The claim costs one already-created handle per process and nothing per run.
        [[maybe_unused]] void rotate_previous_log() {
            if (utility_shared_claim_log_rotation() == 0) {
                return; // another copy of the sink in this process already rotated
            }
            // Text mode on both sides: the read translates CRLF to LF, the text-mode write
            // translates LF back to CRLF, so line endings stay consistent with debug.log
            std::ifstream current_log("debug.log");
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

            // Cap debug.log.old: once it exceeds the cap, start it fresh (truncate) instead of
            // appending forever, so the archive stays bounded across many sessions.
            constexpr uintmax_t old_log_cap = 8ull * 1024ull * 1024ull; // 8 MiB
            std::ios::openmode const old_mode = [&] {
                std::error_code ec;
                uintmax_t const size = std::filesystem::file_size("debug.log.old", ec);
                return (!ec && size >= old_log_cap) ? (std::ios::out | std::ios::trunc) : (std::ios::out | std::ios::app);
            }();
            std::ofstream old_log("debug.log.old", old_mode);
            if (!old_log) {
                return;
            }

            // Timestamp the rotated block so sessions are distinguishable in debug.log.old
            if (!content.contains("===== session")) {
                old_log << deren::utility::vessel::format("===== session ended at {:%Y-%m-%d %H:%M:%S} =====\n",
                                                          std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
            }

            // Normalize to one blank line after every log line, matching the worker's debug.log
            // format (idempotent: already double-spaced content stays unchanged)
            deren::utility::vessel::istringstream lines(content);
            deren::utility::vessel::string line;
            while (std::getline(lines, line)) {
                if (!line.empty()) {
                    old_log << line << '\n'
                            << '\n';
                }
            }
            old_log.close();

            // Start the new session with an empty debug.log
            std::ofstream fresh_log("debug.log", std::ios::out | std::ios::trunc);
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
     * `queue_cv` wakes the worker, `drained_cv` wakes `wait_all()` - which `panic()` calls
     * before `std::terminate()`, so the message that must never be lost is the one that is
     * flushed here rather than at static-destruction time.
     */
    class log_sink { // NOLINT
        std::mutex queue_mutex = {};
        std::condition_variable queue_cv = {};
        std::condition_variable drained_cv = {}; // notifies when the queue has been drained
        deren::utility::vessel::queue<deren::utility::vessel::string> messages = {};
        std::size_t pending = 0; // messages pending write (queued + currently being written)
        std::thread worker = {};
        std::atomic<bool> running = true;
        // Whether write() still enqueues. Cleared by the destructor BEFORE it signals the worker
        // to drain and exit, and read under queue_mutex - so a write that arrives during (or
        // after) teardown is dropped rather than queued for a worker that is already gone. That
        // matters because wait_all() would then block forever on a pending count nothing will
        // ever decrement, and panic() calls wait_all() - i.e. the one path that must not hang is
        // the one that would.
        bool accepting = true;
        std::ofstream file = {}; // Release builds write to debug.log

    public:
        log_sink();
        ~log_sink();
        void worker_loop() noexcept;

        log_sink(log_sink const&) = delete;
        log_sink& operator=(log_sink const&) = delete;

        /// Queue one line. TAKES A VIEW, and the copy the queue stores is made on THIS side - which
        /// is what lets a caller hand over its own temporary string without either side owning the
        /// other's allocation. That is a design choice rather than a rule (this module's interface
        /// may carry STL: see shared_utility.cppm), and it is the shape the three doors settled on.
        void write(std::string_view text);
        void wait_all();
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
            static log_sink* const sink = new log_sink;
            // The comma operator registers the drain and keeps the result as a plain bool; the
            // registration is deliberately the LAST thing done, so nothing here can re-enter
            // instance() while its own guard is still being initialised.
            [[maybe_unused]] static bool const drains_at_exit = (std::atexit(+[] { instance().shutdown(); }), true);
            return *sink;
        }

        /// one write and no flush: the stream's own buffering decides, which is what `std::print`
        /// does too (and `stderr` is unbuffered, which is why the error path needs nothing more)
        void write_text(std::FILE* const stream, std::string_view const text) {
            if (!text.empty()) {
                static_cast<void>(std::fwrite(text.data(), 1, text.size(), stream));
            }
        }
    } // namespace

    log_sink::log_sink() {
#ifdef NDEBUG
        // Release builds: rotate the previous session's log aside, then append the new session
        rotate_previous_log();
        this->file.open("debug.log", std::ios::out | std::ios::app);
#endif
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
        }
        this->running = false;
        this->queue_cv.notify_all();
        if (this->worker.joinable()) {
            this->worker.join(); // wait for the worker to drain the queue before exiting
        }
#ifdef NDEBUG
        if (this->file.is_open()) {
            this->file.close();
        }
#endif
    }

    void log_sink::worker_loop() noexcept {
        static_cast<void>(diagnostics::prepare_crash_thread());
        this->worker_id.store(std::this_thread::get_id(), std::memory_order_relaxed);
        while (true) {
            deren::utility::vessel::string message;
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
            // Write outside the lock to avoid blocking producers (a blank line follows every
            // message for readability; messages carry no \n)
#ifdef NDEBUG
            if (this->file.is_open()) {
                this->file << message << '\n'
                           << '\n'
                           << std::flush;
            } else {
                // the file could not be opened: fall back to the terminal. This is written HERE
                // rather than through deren.utility's println(), because the dependency direction is
                // one-way - the shared half cannot call into the static one.
                deren::utility::vessel::string const line = message + '\n';
                write_text(stdout, line);
            }
#else
            deren::utility::vessel::string const line = message + '\n';
            write_text(stdout, line);
#endif
            // Decrement pending only after the write finishes so wait_log_all also covers the message being written
            {
                std::lock_guard lock(this->queue_mutex);
                --this->pending;
                if (this->pending == 0) {
                    this->drained_cv.notify_all();
                }
            }
        }
    }

    void log_sink::write(std::string_view const text) {
        deren::utility::vessel::string message(text); // the copy the queue owns is made on THIS side of the boundary
        {
            std::lock_guard lock(this->queue_mutex);
            if (!this->accepting) {
                // The sink is shutting down and its worker will not drain anything else: drop the
                // message instead of queueing it for nobody. Late writes are the normal case, not an
                // error - static destructors ordered after this singleton still call log()/error().
                return;
            }
            ++this->pending;
            this->messages.push(std::move(message));
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
        // Return as soon as the sink stopped accepting: any message still counted in `pending` at that
        // point belongs to a worker that is on its way out, and waiting for it would hang forever.
        this->drained_cv.wait(lock, [this] { return this->pending == 0 || !this->accepting; });
    }

    void log_text(std::string_view const text) noexcept {
        instance().write(text);
    }

    void error_text(std::string_view const text) noexcept {
#ifdef NDEBUG
        // Release: hand to the log thread (writes to debug.log)
        instance().write(deren::utility::vessel::format("[ERROR] {}", text));
#else
        // Debug: print directly to stderr in red, no queueing (error is usually followed by terminate)
        deren::utility::vessel::string const line = deren::utility::vessel::format("\x1b[31m[ERROR] {}\x1b[0m\n", text);
        write_text(stderr, line);
#endif
    }

    void wait_log_all() noexcept {
        instance().wait_all();
    }

    [[noreturn]] void panic(std::string_view const msg, std::source_location const source_location) noexcept {
        // Raw evidence survives even if the log queue or formatting cannot complete, and it is what
        // the offline decoder reads. Captured ONCE: the readable pass below decodes the same frames.
        auto const trace = diagnostics::capture_stack();
        diagnostics::write_stack(trace);
        // Every line goes through the ONE sink (or, in Debug, straight to stderr). The formatting is
        // done here because this side owns the sink; a caller's `panic(fmt, args...)` has already
        // formatted its own text and lands on the non-template overload below.
        error_text("program panic!");

        if (!msg.empty()) {
            error_text(deren::utility::vessel::format("error info: {}", msg));
        }

        error_text(deren::utility::vessel::format("occurred at function [{}] line {}", source_location.function_name(), source_location.line()));
        error_text(deren::utility::vessel::format("time point: {:%Y-%m-%d %H:%M:%S}", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())));

        // A PANIC IS READ BY A HUMAN, so it prints the stack as symbols too - the raw frames above
        // are for the offline decoder, not for whoever is staring at debug.log. Best effort by
        // construction: symbolization runs the same bounded child process as any healthy-context
        // call (deadline, output cap, no tool = raw records only), the records above are already
        // written, and a missing tool, missing symbols or a timeout simply leaves this out. The
        // text it produces is itself a raw record plus `symbols frames=` blocks, so it stays
        // decodable offline as well. An OOM or a broken heap does NOT come through here - that path
        // is `fast_panic`, which writes bounded raw text and never symbolizes.
        auto const readable = diagnostics::symbolize(trace);
        if (!readable.text.empty()) {
            // "call stack", not "symbolized stack": the block always carries the raw frames, and the
            // symbols are the part that may be missing (no tool, no debug data, a lean build).
            error_text(deren::utility::vessel::format("call stack:\n{}", readable.text));
        }

        // Release writes logs through the async log thread; flush before terminating,
        // otherwise the panic messages above may be lost (std::terminate skips static destructors).
        wait_log_all();

        std::terminate();
    }
} // namespace deren::utility
