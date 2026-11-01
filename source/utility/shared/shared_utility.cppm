// ============================================================================
// module: deren.utility.shared
// module version: 1.3.0
//
// THE PROCESS-WIDE HALF OF utility (batch ④, the dynamic-backend flip's prerequisite).
//
// WHY A MODULE OF ITS OWN: this toolkit is STATIC and BOTH halves of the RHI boundary link it, so
// today one process holds one copy of everything - the log sink included. TURNING `deren_vulkan`
// INTO A SHARED LIBRARY BREAKS THAT: a DLL carries its own copy of whatever it links, so the
// process would hold TWO sinks - two `ofstream`s on the same deren-debug.log and two startup rotations,
// and `rotate_previous_log()` TRUNCATES the file the other copy is still writing. The boundary gate
// cannot see it either: that gate measures deren_vulkan ∩ deren_engine, and this is a third
// library defining no backend symbol. Hence a target of its own, which the flip makes SHARED.
//
// WHAT BELONGS HERE is exactly the state that must exist once per PROCESS:
//   - the log sink: the queue, its worker thread, and the file handle;
//   - the startup rotation of the previous session's deren-debug.log (and its once-per-process claim);
//   - panic convergence: one `panic` for the whole process, flushing that one sink;
//   - the mimalloc allocation/free service and allocation-failure convergence.
// What stays in `deren.utility` (static_utility) is everything per-object or pure, plus the loader:
// data_block / BVH / frame_clock / frame_stats / thread_pool / the platform_* TUs /
// deren.utility.dynamic_link / every template. `dynamic_link` in particular stays on the
// executable's side: it is the key that loads the backend, so putting it behind another library
// would build a bootstrap chain ("load shared_utility before you can load the backend").
//
// `deren.utility` RE-EXPORTS this module, so an existing `import deren.utility;` still names the
// sink, `panic`, `log`, `error` and memory service - the call sites do not know which half a name
// came from, and this batch changes no call site.
//
// THE EXPORT SURFACE IS C++ AND MAY CARRY STL (batch ④'s relaxed rules, set by the user after the first
// draft argued itself into a C shim): std::string, std::vector, std::function, std::span, std::pmr and a
// `std::format_string` template are all allowed across this module's interface. THE MEASURED BASIS is
// that the executable imports libc++.dll and uses the UCRT heap, so ONE C++ runtime and ONE heap are
// shared by both images - an allocation here and a free there is safe in this configuration, and
// scripts/checks/check_backend_boundary.py now machine-checks that premise (libc++.dll imported, no static
// libc++ linked anywhere).
//
// WHERE THIS BATCH STILL HANDS OVER A VIEW, it is a design choice rather than a rule: the sink's door is
// `log_text(std::string_view)` because the COPY the queue owns is made on the sink's side, and because
// the formatting stays in the caller's template instantiation (`deren.utility`'s `log`), so no template
// needs to be an exported symbol. Nothing above forbids the alternatives.
//
// THE REASON THIS MODULE EXISTS IS ONE PIECE OF STATE PER PROCESS, which is the whole point of the
// split: the sink. Two copies of it would mean two `ofstream`s on one deren-debug.log and two startup
// rotations, and `rotate_previous_log()` truncates the file the other copy still has open. That - not
// the heap, and not STL - is what the seam is for.
//
// The dependency direction is one-way and enforced by this file's contents: `deren.utility` may
// depend on this module, and this module depends on NOTHING of it - it does not even import
// `deren.promise.rhi`; RHI failure types and the policy for them stay out of here (see
// docs/error_handling.md).
// ============================================================================
module;

#include <cstdint>

export module deren.utility.shared;
export import deren.vstd;

namespace deren::utility {
    /// Process-local identity shared across plugins; never reuse zero or a retired identity.
    export [[nodiscard]] std::uint64_t allocate_identity() noexcept;

    /**
     * @ingroup utility
     * @brief THE sink's one door: queue a pre-formatted line for this process's log worker
     * @param text the line, without its newline (the worker adds the blank-line spacing)
     * @note
     *     - FORMATTING STAYS IN THE CALLER: `log(fmt, args...)` in `deren.utility` is an inline
     *       template whose instantiation formats into a temporary `std::string` on the caller's
     *       side and hands it here as a view. That is a design choice, not a restriction (STL is
     *       allowed across this interface - see the header): it keeps the template in the caller's
     *       instantiation and needs no exported symbol.
     *     - the copy this function queues is made on THIS side of the boundary
     *     - thread safe; a message that arrives while the sink is shutting down is dropped (see
     *       `deren.utility`'s `log()` doc for why that is the correct late-write behaviour)
     */
    export void log_text(std::string_view text) noexcept;

    /**
     * @ingroup utility
     * @brief queue `[ERROR] ...` to deren-debug.log in every build
     * @param text the pre-formatted error text
     * @note one asynchronous writer owns the ordinary log; failure falls back to deren-crash.log,
     *       never to stdout/stderr. Fatal reports use the independent emergency writer.
     */
    export void error_text(std::string_view text) noexcept;

    /**
     * @ingroup utility
     * @brief wait until the prefix queued at entry has been processed and write/flush attempted
     * @note newer submissions do not extend the wait. This normal-context operation can block;
     *       panic writes its own report directly and uses only a bounded existing-sink wait.
     */
    export void wait_log_all() noexcept;

    /**
     * @ingroup utility
     * @brief use when the program causes a terminating error
     * @param msg error message
     * @param source_location just use the default argument; it captures the caller's position
     * @note
     *     - TERMINATING: writes a bounded message/location, raw stack and best-effort symbols
     *       directly to deren-crash.log, then briefly waits for an existing ordinary log prefix.
     *       File writes are best effort; no console fallback, async logger initialization or lock
     *       acquisition occurs here. Kernel I/O and symbolization have separate limits.
     *     - a failure the backend only REPORTS (a lost device, exhausted host memory) is turned into
     *       a panic by the ENGINE, not by the backend: the backend answers an error code, and the
     *       engine decides that the process ends here (see docs/error_handling.md)
     */
    export [[noreturn]] void panic(std::string_view msg = "",
                                   std::source_location source_location = std::source_location::current()) noexcept;
} // namespace deren::utility

// ---- Memory service and STL aliases in the primary interface. ----
//
// IT WAS A PARTITION (`deren.utility.shared:better_pmr`) AND IT CANNOT BE ONE. Measured: a partition
// of a module that is ITSELF re-exported (`deren.utility` re-exports `deren.utility.shared`, which
// re-exported `:better_pmr`) makes clang 22.1.8 die with `clang frontend command failed due to
// signal` in a mere CONSUMER's translation unit - utility.cpp, while instantiating std::format in
// write_png, with `-fsyntax-only` (so it is Sema, not codegen). Replacing the partition re-export
// with a plain `import` makes it compile; the same file compiled against master's BMI compiles.
// One DLL owns mimalloc. Templates never link their own allocator instance, and
// no default-resource registration or application startup hook is required.
export namespace deren::utility::memory {
    struct allocation_statistics {
        std::uint64_t current_bytes = 0, peak_bytes = 0;
        std::uint64_t current_allocations = 0, peak_allocations = 0;
    };
    /// Shared project-allocator usable bytes (capacity/rounding included), not RSS or reserved pages.
    /// Individual fields are atomic samples; concurrent allocations can occur between field reads.
    [[nodiscard]] allocation_statistics statistics() noexcept;
    enum class allocation_error { invalid_alignment,
                                  size_overflow,
                                  out_of_memory };

    [[nodiscard]] std::expected<void*, allocation_error> try_allocate(
        std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) noexcept;
    [[nodiscard]] void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) noexcept;
    void deallocate(void* pointer) noexcept;
    [[noreturn]] void allocation_failure(allocation_error error, std::size_t bytes, std::size_t alignment) noexcept;

    /** STL adapter: success returns storage; failure terminates without allocating a diagnostic. */
    template <typename T>
    class allocator {
    public:
        using value_type = T;
        using is_always_equal = std::true_type;
        using propagate_on_container_move_assignment = std::true_type;
        using propagate_on_container_swap = std::true_type;

        allocator() noexcept = default;
        template <typename U>
        allocator(allocator<U> const&) noexcept {
        }

        [[nodiscard]] static constexpr std::size_t max_size() noexcept {
            return static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(T);
        }
        [[nodiscard]] T* allocate(std::size_t count) noexcept {
            if (count > max_size()) {
                constexpr auto limit = std::numeric_limits<std::size_t>::max();
                auto const bytes = count > limit / sizeof(T) ? limit : count * sizeof(T);
                allocation_failure(allocation_error::size_overflow, bytes, alignof(T));
            }
            return static_cast<T*>(memory::allocate(count * sizeof(T), alignof(T)));
        }
        void deallocate(T* pointer, [[maybe_unused]] std::size_t count) noexcept {
            memory::deallocate(pointer);
        }
        template <typename U>
        bool operator==([[maybe_unused]] allocator<U> const& other) const noexcept {
            return true;
        }
    };
} // namespace deren::utility::memory

/** STL semantics with an explicit project allocation policy; methods are not universally noexcept. */
export namespace deren::utility::vessel {
    [[nodiscard]] inline memory::allocation_statistics memory_statistics() noexcept {
        return memory::statistics();
    }
    template <typename T, std::size_t N>
    using array = std::array<T, N>;
    template <typename T>
    using vector = std::vector<T, memory::allocator<T>>;
    template <typename T>
    using deque = std::deque<T, memory::allocator<T>>;
    template <typename T>
    using list = std::list<T, memory::allocator<T>>;
    template <typename Char, typename Traits = std::char_traits<Char>>
    using basic_string = std::basic_string<Char, Traits, memory::allocator<Char>>;
    using string = basic_string<char>;
    using wstring = basic_string<wchar_t>;
    using u8string = basic_string<char8_t>;
    using u16string = basic_string<char16_t>;
    using u32string = basic_string<char32_t>;
    using istringstream = std::basic_istringstream<char, std::char_traits<char>, memory::allocator<char>>;
    using ostringstream = std::basic_ostringstream<char, std::char_traits<char>, memory::allocator<char>>;
    using stringstream = std::basic_stringstream<char, std::char_traits<char>, memory::allocator<char>>;

    // Build the result directly in vessel storage instead of formatting a default-allocator string first.
    template <typename... Args>
    [[nodiscard]] string format(std::format_string<Args...> pattern, Args&&... args) {
        string result;
        std::format_to(std::back_inserter(result), pattern, std::forward<Args>(args)...);
        return result;
    }
    template <typename Integer>
        requires std::is_integral_v<Integer>
    [[nodiscard]] string to_string(Integer value) {
        if constexpr (std::is_same_v<Integer, bool>) {
            return value ? "1" : "0";
        } else {
            char buffer[std::numeric_limits<Integer>::digits10 + 3];
            auto const converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
            if (converted.ec != std::errc{}) {
                memory::allocation_failure(memory::allocation_error::size_overflow, sizeof(buffer), alignof(char));
            }
            return string(buffer, converted.ptr);
        }
    }
    template <typename K, typename V, typename Compare = std::less<K>>
    using map = std::map<K, V, Compare, memory::allocator<std::pair<K const, V>>>;
    template <typename K, typename V, typename Compare = std::less<K>>
    using multimap = std::multimap<K, V, Compare, memory::allocator<std::pair<K const, V>>>;
    template <typename K, typename Compare = std::less<K>>
    using set = std::set<K, Compare, memory::allocator<K>>;
    template <typename K, typename Compare = std::less<K>>
    using multiset = std::multiset<K, Compare, memory::allocator<K>>;
    template <typename K, typename V, typename Hash = std::hash<K>, typename Equal = std::equal_to<K>>
    using unordered_map = std::unordered_map<K, V, Hash, Equal, memory::allocator<std::pair<K const, V>>>;
    template <typename K, typename V, typename Hash = std::hash<K>, typename Equal = std::equal_to<K>>
    using unordered_multimap = std::unordered_multimap<K, V, Hash, Equal, memory::allocator<std::pair<K const, V>>>;
    template <typename K, typename Hash = std::hash<K>, typename Equal = std::equal_to<K>>
    using unordered_set = std::unordered_set<K, Hash, Equal, memory::allocator<K>>;
    template <typename K, typename Hash = std::hash<K>, typename Equal = std::equal_to<K>>
    using unordered_multiset = std::unordered_multiset<K, Hash, Equal, memory::allocator<K>>;
    template <typename T, typename Container = deque<T>>
    using queue = std::queue<T, Container>;
    template <typename T, typename Container = deque<T>>
    using stack = std::stack<T, Container>;
    template <typename T, typename Container = vector<T>, typename Compare = std::less<T>>
    using priority_queue = std::priority_queue<T, Container, Compare>;
} // namespace deren::utility::vessel

export namespace deren::utility::diagnostics {
    inline constexpr std::uint32_t max_stack_depth = 64;
    struct stack_trace {
        vessel::array<std::uintptr_t, max_stack_depth> addresses{};
        std::uint32_t count = 0;
        bool truncated = false;
        // The leading addresses that are EXACT PCs rather than return addresses: a fault PC
        // taken from an exception context (the crash handler's first frame is the exception
        // address itself), or the matching raw record of a crash log. symbolize() decodes
        // these unchanged and every later frame at pc - 1; capture_stack() returns return
        // addresses only, so it leaves this at zero.
        std::uint32_t exact_address_count = 0;
    };
    // skip=0 starts at the caller; excessive skip returns an empty trace.
    [[nodiscard]] stack_trace capture_stack(std::uint32_t skip = 0, std::uint32_t depth = max_stack_depth) noexcept;
    // Append raw records to deren-crash.log; never writes stdout/stderr.
    void write_stack(stack_trace const& trace) noexcept;
    // Call at each external worker entry; the reserved overflow stack is per-thread.
    [[nodiscard]] bool prepare_crash_thread() noexcept;
    [[nodiscard]] bool install_crash_handler() noexcept;
    enum class symbolizer_status { ok,
                                   unavailable,
                                   timeout,
                                   output_limit,
                                   failed };
    struct symbolizer_options {
        std::string_view executable{};                                                 // Empty uses CMake's configured absolute tool path; "off" disables it.
        std::uint32_t timeout_ms = 1000;                                               // Total child-process budget, clamped to 10 seconds.
        std::size_t max_symbolizer_output_bytes = static_cast<std::size_t>(64) * 1024; // Clamped to 1 MiB in total.
        bool inlines = true;
    };
    struct symbolized_trace {
        vessel::string text;
        symbolizer_status status = symbolizer_status::ok;
    };
    // Healthy-context API: owns vessel storage and can start a bounded child process.
    // The first `exact_address_count` frames are symbolized at the recorded PC; the rest are
    // return addresses and are decoded at pc - 1. Raw records are written either way.
    [[nodiscard]] symbolized_trace symbolize(stack_trace const& trace, symbolizer_options const& options = {});
} // namespace deren::utility::diagnostics

export namespace deren::utility {
    // Fatal/OOM entry: bounded raw text then abort. No formatting, location,
    // stack capture, heap objects, log queue/flush, GPU wait or subprocess.
    [[noreturn]] void fast_panic(std::string_view message = {}) noexcept;
} // namespace deren::utility
