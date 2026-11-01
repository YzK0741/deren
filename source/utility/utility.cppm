// ============================================================================
// module: utility
// module version: 3.0.0  (independent of the app version in CMakeLists project(VERSION))
//
// Pure-CPU toolkit: data_block, BVH, thread_pool, frame_clock / frame_stats,
// content hashing, the platform entry points, and the loader. Standalone - no Vulkan
// or app dependency, link it into any host.
//
// THE STATIC (PER-HALF) HALF OF THE SPLIT (batch ④). This module owns everything that is
// per-object or pure, plus deren.utility.dynamic_link; the PROCESS-WIDE state (the log sink, its
// rotation and file handle, panic convergence, the allocator hook) lives in deren.utility.shared,
// which this module RE-EXPORTS. TWO THINGS WORTH KNOWING WHILE READING IT:
//   - THE SPLIT IS ABOUT STATE, NOT MEMORY. Its one reason is that the log sink must exist once per
//     process: two copies mean two `ofstream`s on one deren-debug.log and two startup rotations, and
//     `rotate_previous_log()` truncates the file the other copy is still writing. STL and the heap
//     are NOT the reason and are not restricted here - the user's batch-④ ruling allows containers /
//     std::function and format templates across the seam, on the measured
//     basis that the executable imports libc++.dll and uses the UCRT heap (one runtime, one heap; see
//     the C++-runtime check in scripts/checks/check_backend_boundary.py).
//   - `log` / `error` / `panic` stay HERE as inline templates: the formatting happens in the
//     caller's instantiation and the result is handed over as a std::string_view to the shared sink,
//     so no template needs exporting. `panic` itself, `log_text` / `error_text` / `wait_log_all` and
//     the memory allocation service come from deren.utility.shared - one per process.
// 3.0.0: binary file reads require explicit bounds and return expected/file_error.
// 2.0.0: RHI outcome policy moved out of utility (the engine module that carried it was later deleted; see docs/error_handling.md).
// 1.0.0: project containers use utility::vessel and PMR startup hooks are removed.
// All vessel allocation/deallocation goes through the same shared_utility DLL.
//
// evolve: bump MAJOR on breaking interface changes, MINOR on additive features,
//         PATCH on internal fixes - independently of the rest of the project.
// ============================================================================
module;

#include <cstdint>

export module deren.utility;
export import deren.vstd;
// The process-wide half (sink + rotation + panic + allocator hook) is re-exported, so a consumer
// that imports this module names them exactly as before the split and does not have to know where
// the seam is. The direction is one-way: deren.utility.shared never imports this module.
export import deren.utility.shared;
// Forward-export every per-half submodule so consumers only need `import utility;`
// (frame_clock / frame_stats are the frame-loop time + fps helpers; data_block /
// bvh / thread_pool cover the rest). Submodules stay individually importable for
// callers that want only one of them.
export import :data_block;
export import :bvh;
export import :frame_clock;
export import :frame_stats;
export import :thread_pool;

/**
 * @file utility.cppm
 * @defgroup utility utility functions, classes sets
 */
namespace deren::utility {
    /**
     * @ingroup utility
     * @brief a mixin-class to enable derived class distribute unique handles
     *
     * @note
     *     - uniqueness is only guaranteed in the class instance
     *     - thread-safe (by using std::mutex)
     *     - consider use it in private as a class feature
     *
     * @code {.cpp}
     * class derived : enable_handle_distribute {
     *     uint64_t derived::mem() {
     *         uint64_t handle = 0
     *         auto handle_opt = this->distribute();
     *         if (!handle_opt){
     *             //error process...
     *         }
     *         handle = handle_opt.value();
     *         //do sth...
     *         return handle;
     *     }
     *     //...
     * }
     * @endcode
     */
    export class enable_handle_distribute {

        deren::utility::vessel::set<uint64_t> recycled_handles = {};
        std::mutex access_mutex = {};
        uint64_t handle_upper_bound = 1;

    public:
        std::optional<uint64_t> distribute() noexcept;

        void recycle(uint64_t handle) noexcept;
    };

    /**
     * @ingroup utility
     * @brief a mixin class which enables derived class a stack-style destruct ability
     * @note
     *     - LIFO
     *     - consider use it in private as a class feature
     *     - thread safe
     *
     * @code {.cpp}
     * class sth : enable_stack_destruct{
     *     void mem(){
     *         //...
     *         this->register_cleanup(
     *             [this]{
     *                 // sth cleanup...
     *             });
     *     }
     *     ~sth(){
     *         this->do_cleanup();
     *         //sth cleanup without stack-style...
     *     }
     *     //...
     * }
     * @endcode
     */
    export class enable_stack_destruct {
    public:
        using destruct_type = std::function<void()>;

    private:
        deren::utility::vessel::stack<destruct_type> destruct_stack = {};
        std::mutex access_mutex = {};

    public:
        /**
         * @param destructor callable objects  wants to push in the destruct stack
         */
        void register_cleanup(std::function<void()> const& destructor) noexcept;
        /**
         * @note pop and invoke all destructor in the stack
         */
        void do_cleanup() noexcept;
    };

    // [[noreturn]] void panic(std::string_view, std::source_location) is NOT declared here: it is
    // deren.utility.shared's, the process's one panic (re-exported above), because the report has to
    // go through the one sink. The template below stays here so the FORMATTING is the caller's - the
    // formatted text crosses as a view.

    /**
     * @ingroup utility
     * @brief panic with a compile-time-checked format string (like std::format)
     * @tparam Args argument types
     * @param source_location the caller's location, pass std::source_location::current()
     * @param fmt the format string (compile-time checked)
     * @param args arguments to format
     * @note thread safe
     * @note the location is an explicit parameter because clang does not deduce a parameter
     *       pack that is followed by another parameter
     */
    export template <typename... Args>
    [[noreturn]] void panic(std::source_location source_location, std::format_string<Args...> fmt, Args&&... args) noexcept {
        panic(deren::utility::vessel::format(fmt, std::forward<Args>(args)...), source_location);
    }

    /**
     * @ingroup utility
     * @brief assert the caller's own precondition, in every build
     * @param condition the state the calling code requires to make sense
     * @param description what did not hold, for the panic message (may be empty)
     * @param source_location just use the default argument: it captures the CALLER's position
     * @note
     *     - false panics (see panic()) - there is no mode where this compiles out, because the
     *       conditions it guards are the ones that make the following code meaningless, not
     *       "expensive checks": an assert that vanishes in release turns the bug it names into
     *       silent corruption
     *     - this is the engine-side sibling of the backend's named-panic startup rule: a violated
     *       invariant is a BUG in the code holding it, and terminating with the location is the
     *       honest report; a failure the code EXPECTS travels as a value instead
     *     - thread safe (panic is)
     */
    export void ensure(bool condition, std::string_view description = "",
                       std::source_location source_location = std::source_location::current()) noexcept;

    namespace detail {
        /// one write and no flush: the stream's own buffering decides, which is what `std::print` does too (and
        /// `stderr` is unbuffered, which is why the error path needs nothing more)
        inline void write_text(std::FILE* const stream, std::string_view const text) {
            if (!text.empty()) {
                static_cast<void>(std::fwrite(text.data(), 1, text.size(), stream));
            }
        }
    } // namespace detail

    /**
     * @brief `stdout`, which a module cannot export
     *
     * THE C STANDARD STREAMS ARE MACROS, not objects: exporting one from a module is not something the language
     * can do. The two ways out are a textual `<cstdio>` include in this INTERFACE - which declares libc++'s
     * entities twice, once in the module's global fragment and once through `export import vstd`, and which
     * clang 22.1.8 responds to by CRASHING in code generation (measured: `EmitBuiltinNewDeleteCall` on
     * `std::__libcpp_allocate`, while emitting the deferred definitions of a consumer that instantiates the
     * print family below) - or an accessor, which is this. `std::FILE` and `std::fwrite` come from `vstd`
     * already; only the macro needed a home, and it is the implementation unit's.
     */
    export [[nodiscard]] std::FILE* standard_output() noexcept;

    /**
     * @ingroup utility
     * @brief THE PROJECT'S OWN `std::print`: formatted text to a stream
     *
     * WHY THIS EXISTS INSTEAD OF `std::print`, and it is a MEASURED LINK FAILURE rather than taste. MinGW's
     * libstdc++ 16.2 ships `<print>`'s declarations without the terminal-writing half of its implementation: a
     * call site fails at link time with
     *
     *     undefined reference to `std::__open_terminal(_iobuf*)'
     *     undefined reference to `std::__write_to_terminal(void*, std::span<char, ...>)'
     *
     * (`nm --defined-only libstdc++.a` finds no definition of either, and both are referenced from
     * `bits/print.h`'s `vprint_unicode`.) `deren::utility::log` is the only thing in the engine that prints and the
     * tests are the only thing that prints besides it, so owning these functions here removed the tree's last
     * use of `<print>` - which, together with one `std::unique_ptr` held over an incomplete type, was the whole
     * of what stopped the project linking with libstdc++ (see the compiler-tolerance audit).
     *
     * THE SEMANTICS ARE `std::print`'s: the format string is a `std::format_string`, so it is checked at COMPILE
     * time exactly as `std::format`'s is, and the result is written with ONE `std::fwrite`. There is no flush.
     * `std::format` itself comes from `vstd` (which exports it) - the one standard facility this family needs
     * and does not implement.
     *
     * @param stream the stream to write to (`standard_output()`, `stderr` from a TU that includes `<cstdio>`, or
     *        any other)
     * @param fmt the format string, checked at compile time
     * @param args the arguments it formats
     * @note the project builds with -fno-exceptions, so a formatting or allocation failure terminates instead of
     *       propagating: the first is a bug the compile-time check already rules out, the second is a machine out
     *       of memory
     * @note thread safe to the extent `std::fwrite` is - the C library locks the stream
     */
    export template <typename... Args>
    void print(std::FILE* stream, std::format_string<Args...> fmt, Args&&... args) {
        detail::write_text(stream, deren::utility::vessel::format(fmt, std::forward<Args>(args)...));
    }

    /// @brief the same as `print` above, written to `standard_output()`
    /// @param fmt the format string, checked at compile time
    /// @param args the arguments it formats
    export template <typename... Args>
    void print(std::format_string<Args...> fmt, Args&&... args) {
        // QUALIFIED, and that is not decoration: an unqualified call here is AMBIGUOUS against
        // `std::print(std::FILE*, format_string<Args...>, Args&&...)`, which libc++'s <print> brings into an
        // unqualified lookup inside this namespace (measured: clang reports the two as candidates and calls it
        // ambiguous at this line).
        deren::utility::print(standard_output(), fmt, std::forward<Args>(args)...);
    }

    /// @brief `print` with the newline `std::println` adds
    export template <typename... Args>
    void println(std::FILE* stream, std::format_string<Args...> fmt, Args&&... args) {
        deren::utility::vessel::string text = deren::utility::vessel::format(fmt, std::forward<Args>(args)...);
        text.push_back('\n');
        detail::write_text(stream, text);
    }

    /// @brief the same as `println` above, written to `standard_output()`
    /// @param fmt the format string, checked at compile time
    /// @param args the arguments it formats
    export template <typename... Args>
    void println(std::format_string<Args...> fmt, Args&&... args) {
        // Qualified for the same measured reason as `print` above: libc++'s <print> has a `std::println(FILE*,
        // ...)` overload that an unqualified call finds, and the two are then ambiguous.
        deren::utility::println(standard_output(), fmt, std::forward<Args>(args)...);
    }

    /**
     * @ingroup utility
     * @brief a simple time test function
     * @param test callable objects wants to get the invoke time cost
     * @return used time in invoking the argument
     */
    /**
     * @ingroup utility
     * @brief byte order of a scalar written through write_binary
     * @note binary formats are endian-defined (PNG stores its chunk lengths and CRCs big-endian,
     *       Vulkan/glTF-style data is little-endian), so a plain memcpy of a scalar is only correct
     *       on a matching host. Tag scalars with be()/le() to say which bytes you mean; a bare
     *       scalar is written little-endian (and a raw POD struct keeps the host's order).
     */
    export enum class endian { little,
                               big,
    };

    /**
     * @ingroup utility
     * @brief a scalar tagged with the byte order it should be written in (see be() / le())
     * @tparam T the scalar type (integral or floating point; bool has no byte order)
     * @tparam Order the byte order write_single() writes it with
     * @note prefer the deducing factories be(value) / le(value) over spelling this type out; it is
     *       a type (not a namespace or an enum argument) so it can also be a template parameter,
     *       which a future reader or a generic block-conversion utility will want.
     */
    export template <typename T, endian Order = endian::little>
        requires(std::integral<T> || std::floating_point<T>) && (!std::same_as<T, bool>)
    struct ordered {
        T value = {};
    };

    /** @brief tag @p value to be written big-endian (network byte order, PNG chunk fields) */
    export template <std::integral T>
    [[nodiscard]] constexpr ordered<T, endian::big> be(T const value) noexcept {
        return {value};
    }
    /** @brief tag @p value to be written little-endian (Vulkan / most file formats) */
    export template <std::integral T>
    [[nodiscard]] constexpr ordered<T, endian::little> le(T const value) noexcept {
        return {value};
    }
    /** @brief tag a floating point @p value to be written big-endian (IEEE-754 bit pattern) */
    export template <std::floating_point T>
    [[nodiscard]] constexpr ordered<T, endian::big> be(T const value) noexcept {
        return {value};
    }
    /** @brief tag a floating point @p value to be written little-endian (IEEE-754 bit pattern) */
    export template <std::floating_point T>
    [[nodiscard]] constexpr ordered<T, endian::little> le(T const value) noexcept {
        return {value};
    }

    /**
     * @ingroup utility
     * @brief anything write_single() can append to: a type with write(char const*, size_t)
     * @note std::ostream / std::ofstream satisfy this, and so does a tiny test sink - which is why
     *       the writer is not tied to files: bytes can be checked in-memory.
     */
    export template <typename S>
    concept byte_sink = requires(S& sink, char const* data, std::size_t size) {
        sink.write(data, size);
    };

    namespace detail {
        // the concepts below all normalize their argument first: write_binary deduces its pack as
        // forwarding references, so a concept that only worked on a plain value type would reject
        // every lvalue (an array argument arrives as `T (&)[N]`, a span as `span<...> const&`).
        template <typename T>
        using plain = std::remove_cvref_t<T>;

        template <typename T>
        struct is_ordered : std::false_type {};
        template <typename T, endian Order>
        struct is_ordered<ordered<T, Order>> : std::true_type {};

        template <typename T>
        concept ordered_value = is_ordered<plain<T>>::value;

        // the byte order an ordered<T, Order> carries
        template <typename T>
        struct order_of;
        template <typename T, endian Order>
        struct order_of<ordered<T, Order>> {
            static constexpr endian value = Order;
        };

        // contiguous and one byte per element: spans/arrays/vectors/strings of bytes and characters
        template <typename T>
        concept byte_range = std::ranges::contiguous_range<plain<T>> && (sizeof(std::ranges::range_value_t<plain<T>>) == 1);

        // a byte range, or a contiguous range of trivially copyable elements written as one block
        // (deren::utility::vessel::array<float, 3>, std::span<glm::vec3>, deren::utility::vessel::vector<uint32_t>, ...)
        template <typename T>
        concept blittable_range = std::ranges::contiguous_range<plain<T>> && std::is_trivially_copyable_v<std::ranges::range_value_t<plain<T>>>;

        template <typename T>
        concept pod_value = std::is_trivially_copyable_v<plain<T>> && std::is_standard_layout_v<plain<T>> && (!std::is_pointer_v<plain<T>>) && (!std::is_enum_v<plain<T>>);

        template <typename>
        inline constexpr bool always_false = false;

        /**
         * @brief the bytes of a scalar in the requested byte order, correct on either host order
         * @note integers are assembled byte by byte (host independent); a float goes through its
         *       IEEE-754 bit pattern, which std::bit_cast hands over in HOST order, so it is
         *       reversed when the host and the requested order disagree
         */
        template <typename T, endian Order>
        [[nodiscard]] deren::utility::vessel::array<uint8_t, sizeof(T)> scalar_bytes(T const value) noexcept {
            deren::utility::vessel::array<uint8_t, sizeof(T)> bytes = {};
            if constexpr (std::is_floating_point_v<T>) {
                bytes = std::bit_cast<deren::utility::vessel::array<uint8_t, sizeof(T)>>(value);
                constexpr bool host_is_big = std::endian::native == std::endian::big;
                if constexpr ((Order == endian::big) != host_is_big) {
                    std::ranges::reverse(bytes);
                }
            } else {
                using unsigned_type = std::make_unsigned_t<T>;
                unsigned_type const bits = static_cast<unsigned_type>(value);
                for (std::size_t i = 0; i < bytes.size(); ++i) {
                    std::size_t const index = Order == endian::little ? i : bytes.size() - 1u - i;
                    bytes[index] = static_cast<uint8_t>((bits >> (8u * i)) & 0xFFu);
                }
            }
            return bytes;
        }
    } // namespace detail

    /**
     * @ingroup utility
     * @brief types write_single()/write_binary() can write
     * @note the set is deliberately closed and predictable:
     *       - ordered<T, Order> - one scalar with an explicit byte order (be()/le())
     *       - a contiguous one-byte range - written as-is, exactly size() bytes, empty writes
     *         nothing (a string LITERAL is char const[N] and therefore includes its terminator;
     *         write std::string_view{"IHDR"}, not "IHDR", for a fixed character sequence)
     *       - a contiguous range of trivially copyable values - one block write, no length prefix
     *         (add the length yourself when the format wants one)
     *       - a trivially copyable, standard-layout scalar/struct - native layout, which INCLUDES
     *         any padding and uses the HOST byte order (use be()/le() for the fields of a real file
     *         format instead)
     *       Anything else (a range of non-trivial elements, a pointer, an enum, a class with
     *       invariants) is rejected at compile time rather than written ambiguously.
     */
    export template <typename T>
    concept binary_writable = detail::ordered_value<T> || detail::byte_range<T> || detail::blittable_range<T> || detail::pod_value<T>;

    /**
     * @ingroup utility
     * @brief append one value to a byte sink (see binary_writable for what that can be)
     * @param sink the destination
     * @param value the value to append; for scalars use be()/le() to control the byte order
     * @return empty expected on success, an error message on a sink failure
     */
    export template <byte_sink S, typename T>
        requires binary_writable<T>
    std::expected<void, deren::utility::vessel::string> write_single(S& sink, T const& value) {
        // a sink that exposes its state (std::ostream and friends) is checked after every write, so
        // a full disk / a broken pipe is reported instead of silently dropping the tail. It takes the
        // sink as a parameter rather than capturing it: the check is compiled out for a sink without
        // operator bool, and a named capture that only the discarded branch uses is a warning.
        auto const check = [](S& out) -> std::expected<void, deren::utility::vessel::string> {
            if constexpr (requires { static_cast<bool>(out); }) {
                if (!out) {
                    return std::unexpected(deren::utility::vessel::string("write_single: sink is in a failed state"));
                }
            }
            return {};
        };
        auto const append = [&](void const* data, std::size_t const size) -> std::expected<void, deren::utility::vessel::string> {
            if (size == 0) {
                return {}; // never hand a null pointer to the sink
            }
            sink.write(static_cast<char const*>(data), size);
            return check(sink);
        };

        std::expected<void, deren::utility::vessel::string> result = {};
        if constexpr (detail::ordered_value<T>) {
            // one scalar, written in the order its tag asks for
            using scalar_type = std::remove_cvref_t<decltype(value.value)>;
            auto const bytes = detail::scalar_bytes<scalar_type, detail::order_of<std::remove_cvref_t<T>>::value>(value.value);
            result = append(bytes.data(), bytes.size());
        } else if constexpr (detail::byte_range<T>) {
            // one byte per element, written exactly as it lies: no length prefix, no conversion
            result = append(std::ranges::data(value), std::ranges::size(value));
        } else if constexpr (detail::blittable_range<T>) {
            // contiguous trivially copyable elements: one block write (padding inside an element is
            // written too, the same bytes an element-wise loop would produce)
            result = append(std::ranges::data(value), std::ranges::size(value) * sizeof(std::ranges::range_value_t<T>));
        } else if constexpr (detail::pod_value<T>) {
            // native layout: padding bytes and the host byte order are written as they are
            result = append(&value, sizeof(T));
        } else {
            static_assert(detail::always_false<T>, "write_single: unsupported type - see deren::utility::binary_writable");
        }
        return result;
    }

    /**
     * @ingroup utility
     * @brief append several values to a byte sink, stopping at the first failure
     * @param sink the destination (std::ostream / std::ofstream / any byte_sink)
     * @param args the values to append in order (see binary_writable; wrap scalars in be()/le())
     * @return empty expected on success, the first error message otherwise
     * @note append-only: there is no seek, so a format that has to patch a size afterwards must
     *       compute it up front (or buffer that part). Use write_binary_file() to open a file.
     */
    export template <byte_sink S, typename... Args>
        requires(binary_writable<Args> && ...)
    std::expected<void, deren::utility::vessel::string> write_binary(S& sink, Args&&... args) {
        std::expected<void, deren::utility::vessel::string> result = {};
        auto const write_one = [&sink, &result](auto const& value) {
            if (result) { // comma fold below is left-to-right and stops writing once it failed
                result = write_single(sink, value);
            }
        };
        (write_one(args), ...);
        return result;
    }

    /**
     * @ingroup utility
     * @brief write several values to @p path, overwriting it (opens in binary mode)
     * @param path output file, created or truncated
     * @param args the values to append in order (see binary_writable)
     * @return empty expected on success, an error message on failure
     * @note the explicit _file suffix keeps the file side effect visible at the call site; the
     *       stream overload is the one to use when the file is already open or nothing is written
     *       to disk (tests).
     */
    export template <typename... Args>
        requires(binary_writable<Args> && ...)
    std::expected<void, deren::utility::vessel::string> write_binary_file(std::filesystem::path const& path, Args&&... args) {
        // std::ios::binary matters on Windows: text mode would translate '\n' to "\r\n" and
        // corrupt every binary format
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) {
            return std::unexpected(deren::utility::vessel::format("write_binary_file: cannot open '{}'", path.string()));
        }
        std::expected<void, deren::utility::vessel::string> const written = write_binary(file, std::forward<Args>(args)...);
        if (!written) {
            return written;
        }
        file.flush(); // a failed flush loses the tail: report it rather than returning success
        if (!file) {
            return std::unexpected(deren::utility::vessel::format("write_binary_file: write failed for '{}'", path.string()));
        }
        return {};
    }

    /**
     * @ingroup utility
     * @brief write an 8-bit RGBA image to a PNG file
     * @param path output file (overwritten)
     * @param width image width in pixels
     * @param height image height in pixels
     * @param rgba tightly packed RGBA rows (width * height * 4 bytes)
     * @return empty expected on success, an error message otherwise
     * @note no external dependency: a minimal PNG writer (CRC32 + zlib stream of uncompressed
     *       deflate blocks + adler32), so captures work without pulling in an image library
     */
    export std::expected<void, deren::utility::vessel::string> write_png(std::filesystem::path const& path, uint32_t width, uint32_t height, std::span<uint8_t const> rgba);

    /**
     * @ingroup utility
     * @brief elapsed milliseconds between two GPU timestamp counter readings
     * @param begin_ticks counter value of the earlier mark
     * @param end_ticks counter value of the later mark
     * @param valid_bits counter width of the queue family
     *        (VkQueueFamilyProperties::timestampValidBits)
     * @param nanoseconds_per_tick duration of one tick
     *        (VkPhysicalDeviceLimits::timestampPeriod, ns/tick)
     * @return elapsed milliseconds, or 0 when the family cannot timestamp (@p valid_bits == 0)
     *         or the tick duration is not positive
     * @note the GPU counter is a modulo-2^@p valid_bits ring: queues commonly report 32 or 36
     *       valid bits, and the driver leaves the bits above that range undefined, so both
     *       readings are masked into that width and the difference is taken modulo it - a wrap
     *       inside the measured span comes out right instead of underflowing to a huge value.
     *       A span longer than one full wrap (2^32 ticks = 4.3 s at 1 ns/tick) is indistinguishable
     *       from a short one and would alias; no single pass comes close.
     */
    export double timestamp_delta_milliseconds(uint64_t begin_ticks, uint64_t end_ticks, uint32_t valid_bits, float nanoseconds_per_tick) noexcept;

    /// The failing stage of a bounded binary read; independent of renderer contracts.
    export enum class file_error_code : uint8_t {
        status_failed,
        not_regular_file,
        open_failed,
        size_unavailable,
        too_large,
        read_failed,
        file_changed,
    };
    export struct file_error {
        file_error_code code;
        // Metadata errors retain their system cause. Stream errors may expose only io_errc::stream.
        // Logical rejections (type, capacity or observed length change) leave cause empty.
        std::error_code cause{};
    };
    export [[nodiscard]] std::string_view file_error_name(file_error_code code) noexcept;

    /**
     * @ingroup utility
     * @brief read a regular file into a byte vector with an explicit allocation bound
     * @param path the file path; symlinks resolving to regular files are accepted
     * @param max_bytes largest accepted payload, checked before allocation and narrowing
     * @return complete contents or the failing stage/cause; never partial data
     * @note zero bound accepts only empty files. Observed short EOF or extra data is file_changed.
     *       This does not lock the file or detect same-size edits; concurrent path replacement is
     *       outside the contract. Ordinary I/O errors are values; allocation failure remains fatal.
     */
    export [[nodiscard]] std::expected<deren::utility::vessel::vector<uint8_t>, file_error>
    read_binary_to_vector(std::filesystem::path const& path, std::uintmax_t max_bytes);

    /**
     * @ingroup utility
     * @brief read binary bytes into a string, preserving embedded NUL/high bytes without text decoding
     * @param path the regular file path
     * @param max_bytes largest accepted payload; zero accepts only empty files
     * @return complete contents or file_error, with the same contract as read_binary_to_vector
     */
    export [[nodiscard]] std::expected<deren::utility::vessel::string, file_error>
    read_binary_to_string(std::filesystem::path const& path, std::uintmax_t max_bytes);

    // class log_sink is NOT declared here any more: the sink (queue, worker thread, file handle) is
    // deren.utility.shared's, so that ONE of it exists per process - the whole point of the split.
    // The templates below are this module's, and they reach it through the exported non-template
    // `log_text` / `error_text` cores.

    /**
     * @ingroup utility
     * @brief asynchronous log: formats the message like std::format and pushes it to the log singleton
     * @tparam Args argument types
     * @param fmt the format string (compile-time checked)
     * @param args arguments to format
     * @note output goes to the asynchronous deren-debug.log file in every build; there is no console fallback
     * @note THE FORMATTING HAPPENS HERE, IN THE CALLER'S INSTANTIATION: the formatted line is a
     *       temporary deren::utility::vessel::string on this side and crosses to the sink as a std::string_view, which
     *       the sink copies. A design choice rather than a restriction (STL is allowed across the
     *       seam - see the header): it keeps the template in the caller's instantiation and needs no
     *       exported symbol.
     */
    export template <typename... Args>
    void log(std::format_string<Args...> fmt, Args&&... args) {
        log_text(deren::utility::vessel::format(fmt, std::forward<Args>(args)...));
    }

    /**
     * @ingroup utility
     * @brief asynchronous log: writes a single pre-formatted string as-is
     * @param message the message (string literal, const char*, deren::utility::vessel::string or std::string_view)
     * @note
     *      - for runtime strings, which cannot construct the consteval std::format_string
     *      - for format-string usage prefer the template overload
     */
    export void log(std::string_view message) {
        log_text(message);
    }

    /**
     * @ingroup utility
     * @brief error log: queues the message with an [ERROR] prefix to deren-debug.log in every build
     * @tparam Args argument types
     * @param fmt the format string (compile-time checked)
     * @param args arguments to format
     * @note the formatting and its lifetime rule are `log`'s above; `error_text` is the shared core
     */
    export template <typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) {
        error_text(deren::utility::vessel::format(fmt, std::forward<Args>(args)...));
    }

    /**
     * @ingroup utility
     * @brief error log: queues the message with an [ERROR] prefix to deren-debug.log in every build
     * @param message the message (string literal, const char*, deren::utility::vessel::string or std::string_view)
     */
    export void error(std::string_view message) {
        error_text(message);
    }

    // wait_log_all() is NOT declared here: it is deren.utility.shared's (re-exported above), since
    // what it waits on is the one process-wide queue.

    /**
     * @defgroup hash Content Hashing
     * @ingroup utility
     * @brief xxHash-based 128-bit content hash (XXH3_128bits), returned as a data_block<16>
     * @note
     *     - non-cryptographic, extremely fast (used for content dedup)
     *     - 128-bit digest: two independent 64-bit lanes, so an accidental collision is
     *       negligible for content-addressed GPU-resource dedup (a wrong share would silently
     *       render the wrong texture / image)
     *     - digest supports operator==/!=/<=> and hex formatting (.to_hex_string())
     *     - cannot fail (no allocation / error state)
     */

    /**
     * @typedef xxh3_digest
     * @relates data_block
     * @ingroup hash
     */
    export using xxh3_digest = data_block<16>;

    /**
     * @brief xxh3_64bits hash function
     * @param data_view bytes to fingerprint
     * @return the 64-bit fingerprint of @p data_view
     * @ingroup hash
     *
     * The narrow variant is for per-frame change detection (the shadow pass's geometry signature),
     * where a collision costs one stale frame rather than a silently wrong resource - use
     * xxh3_128bits() when a collision would be wrong without anyone noticing, as it would be for
     * content-addressed dedup. It is also the faster of the two on small inputs.
     */
    export uint64_t xxh3_64bits(std::span<uint8_t const> data_view);

    /**
     * @brief sleep for a relative number of nanoseconds, with sub-millisecond precision
     * @param nanoseconds how long to wait; zero or a negative value returns immediately
     * @ingroup utility
     *
     * Why this is not std::this_thread::sleep_for: on Windows the default timer granularity is 15.6 ms,
     * so a sleep of a few milliseconds is routinely served late - which is the difference between
     * holding 60 fps and sagging towards 30. The Windows implementation waits on a
     * CREATE_WAITABLE_TIMER_HIGH_RESOLUTION timer (Windows 10 1803+) rather than calling
     * timeBeginPeriod, which would raise the timer resolution for every process on the machine; the
     * POSIX one restarts a relative nanosleep on EINTR.
     * The wait is still only good to microseconds, so a caller that wants to land exactly on a deadline
     * sleeps to a safe margin early and spins the remainder (the frame limiter in deren.engine.runtime sleeps
     * to one millisecond before its deadline, then yields until the deadline arrives).
     */
    export void sleep_for_nanoseconds(int64_t nanoseconds);

    /**
     * @brief directory of the running executable, or an empty path when the platform cannot report it
     * @return the directory with a trailing separator, or an empty path
     * @ingroup utility
     *
     * The build emits the compiled shaders next to the executable, so this is how the runtime prefers
     * the shaders its own build produced over any directory found by walking up from the working
     * directory - the difference between running the shaders you just edited and a stale copy.
     * @note deliberately narrow: it answers "where am I running from", a question only the loader can
     *       answer, and it answers it from the OS rather than from argv[0] (which may be a bare name
     *       resolved through PATH).
     */
    export std::filesystem::path executable_directory();

    /**
     * @brief ask the user for an existing file with the platform's own open dialog
     * @param title window/prompt title; a backend that builds a command line refuses a shell-hostile one
     * @param filter_patterns `;`-separated glob patterns, e.g. "*.glb;*.gltf"; empty means all files
     * @return the chosen path, or std::nullopt when the user cancelled OR no backend could ask
     * @ingroup utility
     *
     * This exists for the config's `model = "ask"`. The model is resolved before the window is created,
     * which rules out an in-app picker (that would need the overlay and a frame loop, and the scene is
     * imported before either), so the question goes to the platform: GetOpenFileNameW on Windows, zenity
     * or kdialog on Linux, osascript on macOS, and "nobody could ask" everywhere else.
     * @note the two nullopt cases are logged apart, because they are different news: a cancelled dialog is
     *       a decision and the caller quietly falls back to its default model, while an unavailable backend
     *       is a limitation the user should hear about rather than guess at.
     */
    export std::optional<std::filesystem::path> ask_open_file(std::string_view title, std::string_view filter_patterns = {});

    /**
     * @brief xxh3_128bits hash function
     * @param data_view bytes to fingerprint
     * @return 16-byte digest of @p data_view (the raw 128-bit fingerprint)
     * @ingroup hash
     */
    export xxh3_digest xxh3_128bits(std::span<uint8_t const> data_view);
} // namespace deren::utility
