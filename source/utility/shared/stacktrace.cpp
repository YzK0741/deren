module;
#include "stacktrace_native.hpp"
module deren.utility.shared;

#if defined(_MSC_VER)
#define TRACE_NOINLINE __declspec(noinline)
#else
#define TRACE_NOINLINE __attribute__((noinline))
#endif

namespace deren::utility::diagnostics {
    namespace native = deren::utility::detail::trace_native;
    namespace {
        /// The address handed to the symbolizer for one frame.
        ///
        /// A CAPTURED frame holds a RETURN address - the instruction after the call - so it is
        /// decoded at pc - 1: the call it returns from is the frame that matters. A FAULT PC
        /// from an exception context is EXACT (the crash handler's first frame IS the exception
        /// address), and moving it one byte back decodes the instruction before the fault: for
        /// the access-violation probe that is stacktrace_fault.cpp:16 instead of :18, i.e. a
        /// different source line with no sign that anything was adjusted. Hence the split, and
        /// hence `exact_address_count` on the trace rather than a blanket -1.
        [[nodiscard]] std::uintptr_t symbolizer_address(stack_trace const& trace, std::uint32_t index) noexcept {
            auto const pc = trace.addresses[index];
            if (!pc) {
                return 0;
            }
            return index < trace.exact_address_count ? pc : pc - 1;
        }
    } // namespace
    TRACE_NOINLINE stack_trace capture_stack(std::uint32_t skip, std::uint32_t depth) noexcept {
        stack_trace result;
        if (skip > 1024) {
            return result;
        }
        auto raw = native::capture(skip + 1, depth);
        result.count = raw.count;
        result.truncated = raw.truncated;
        std::copy_n(raw.pc, raw.count, result.addresses.begin());
        return result;
    }
    void write_stack(stack_trace const& trace) noexcept {
        native::trace raw;
        raw.count = std::min(trace.count, max_stack_depth);
        raw.truncated = trace.truncated || trace.count > max_stack_depth;
        std::copy_n(trace.addresses.begin(), raw.count, raw.pc);
        native::write_trace(raw);
    }
    bool prepare_crash_thread() noexcept {
        return native::prepare_thread();
    }
    bool install_crash_handler() noexcept {
        return native::install_handler();
    }

    symbolized_trace symbolize(stack_trace const& trace, symbolizer_options const& options) {
        symbolized_trace result;
        auto n = std::min(trace.count, max_stack_depth);
        if (!n) {
            return result;
        }
        auto executable = options.executable.empty() ? std::string_view(native::default_symbolizer()) : options.executable;
        bool disabled = executable.empty() || executable == "off";
        bool invalid = executable.find('\0') != std::string_view::npos || executable.size() >= 2048;
        vessel::string tool(executable);
        auto timeout = std::min(options.timeout_ms, std::uint32_t{10000});
        auto deadline = native::monotonic_ms() + timeout;
        auto remaining_output = std::min(options.max_symbolizer_output_bytes, static_cast<std::size_t>(1024) * 1024);
        vessel::vector<char> output;
        if (!disabled && !invalid && remaining_output) {
            output.resize(remaining_output);
        }
        for (std::uint32_t i = 0; i < n;) {
            native::frame_info frame;
            // Exact PCs are kept as recorded; return PCs point after the call instruction.
            static_cast<void>(native::resolve(symbolizer_address(trace, i), frame));
            std::uintptr_t offsets[max_stack_depth]{};
            std::uint32_t j = i;
            for (; j < n; ++j) {
                native::frame_info next;
                static_cast<void>(native::resolve(symbolizer_address(trace, j), next));
                if (j != i && (next.base != frame.base || std::string_view(next.module) != frame.module)) {
                    break;
                }
                offsets[j - i] = next.offset;
                // Always retain raw evidence, including when symbols are missing or a child fails.
                result.text += vessel::format("frame pc=0x{:x} base=0x{:x} rva=0x{:x} module={}\n",
                                              trace.addresses[j], next.base, trace.addresses[j] - next.base, next.module);
            }
            auto status = symbolizer_status::ok;
            std::size_t written = 0;
            auto now = native::monotonic_ms();
            if (invalid) {
                status = symbolizer_status::failed;
            } else if (disabled || !frame.module[0]) {
                status = symbolizer_status::unavailable;
            } else if (!remaining_output) {
                status = symbolizer_status::output_limit;
            } else if (now >= deadline) {
                status = symbolizer_status::timeout;
            } else {
                auto raw_status = native::symbolize_group(tool.c_str(), frame.module, offsets, j - i, options.inlines,
                                                          output.data(), remaining_output, written, static_cast<std::uint32_t>(deadline - now));
                switch (raw_status) {
                case native::process_status::ok:
                    break;
                case native::process_status::unavailable:
                    status = symbolizer_status::unavailable;
                    break;
                case native::process_status::timeout:
                    status = symbolizer_status::timeout;
                    break;
                case native::process_status::output_limit:
                    status = symbolizer_status::output_limit;
                    break;
                case native::process_status::failed:
                    status = symbolizer_status::failed;
                    break;
                }
                if (status == symbolizer_status::ok && written) {
                    result.text += vessel::format("symbols frames={}..{}\n", i, j - 1);
                    result.text.append(output.data(), written);
                }
                // A tool that exited cleanly without writing anything decoded nothing: reporting
                // `ok` here would claim symbols that do not exist, and the caller cannot tell it
                // from a group that was decoded (the raw records are present either way).
                if (status == symbolizer_status::ok && !written) {
                    status = symbolizer_status::unavailable;
                }
                remaining_output -= written;
            }
            if (status != symbolizer_status::ok && result.status == symbolizer_status::ok) {
                result.status = status;
            }
            i = j;
        }
        return result;
    }
} // namespace deren::utility::diagnostics
namespace deren::utility {
    [[noreturn]] TRACE_NOINLINE void fast_panic(std::string_view message) noexcept {
        detail::trace_native::fast_panic(message.data(), message.size());
    }
} // namespace deren::utility
