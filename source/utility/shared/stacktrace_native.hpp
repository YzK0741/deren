#pragma once
#include <cstddef>
#include <cstdint>

namespace deren::utility::detail::trace_native {
    inline constexpr std::size_t capacity = 64;
    struct trace {
        std::uintptr_t pc[capacity]{};
        std::uint32_t count = 0;
        bool truncated = false;
    };
    struct frame_info {
        std::uintptr_t base = 0;
        std::uintptr_t offset = 0;
        char module[2048]{};
    };
    enum class process_status { ok,
                                unavailable,
                                timeout,
                                output_limit,
                                failed };
    trace capture(std::uint32_t skip, std::uint32_t depth) noexcept;
    bool resolve(std::uintptr_t pc, frame_info& frame) noexcept;
    void write_trace(trace const& value) noexcept;
    void write_current() noexcept;
    // Emergency file only; signal contexts use the preopened variant and never open files.
    bool prepare_output() noexcept;
    void write_preopened_text(char const* value, std::size_t size) noexcept;
    void write_text(char const* value, std::size_t size) noexcept;
    void write_panic_header(char const* message, std::size_t size, char const* file,
                            char const* function, std::uint32_t line) noexcept;
    [[noreturn]] void fast_panic(char const* message, std::size_t size) noexcept;
    bool prepare_thread() noexcept;
    bool install_handler() noexcept;
    char const* default_symbolizer() noexcept;
    std::uint64_t monotonic_ms() noexcept;
    process_status symbolize_group(char const* executable, char const* module, std::uintptr_t const* offsets,
                                   std::uint32_t count, bool inlines, char* output, std::size_t capacity,
                                   std::size_t& written, std::uint32_t timeout_ms) noexcept;
} // namespace deren::utility::detail::trace_native
