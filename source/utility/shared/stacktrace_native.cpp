// Keep OS headers in a plain TU. The fatal path owns no C++ heap objects.
#include "stacktrace_native.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <chrono>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <unwind.h>
#if defined(__linux__)
#include <link.h>
#endif
#endif

#ifndef DEREN_LLVM_SYMBOLIZER
#define DEREN_LLVM_SYMBOLIZER ""
#endif
#if defined(_MSC_VER)
#define TRACE_NOINLINE __declspec(noinline)
#else
#define TRACE_NOINLINE __attribute__((noinline))
#endif

namespace deren::utility::detail::trace_native {
    namespace {
#if defined(_WIN32)
        // Process-lifetime emergency file. Fatal calls before startup may attempt one lazy open.
        std::atomic<HANDLE> emergency_output{nullptr};
        static_assert(std::atomic<HANDLE>::is_always_lock_free);
#else
        std::atomic<int> emergency_output{-1};
        static_assert(std::atomic<int>::is_always_lock_free);
#endif
        std::atomic<bool> emergency_open_attempted{false};
        static_assert(std::atomic<bool>::is_always_lock_free);
        struct line_buffer {
            char bytes[512];
            std::size_t used = 0;
            void text(char const* value, std::size_t max = 400) noexcept {
                if (!value) {
                    return;
                }
                while (*value && max-- && used < sizeof(bytes)) {
                    bytes[used++] = *value++;
                }
            }
            void number(std::uintptr_t value, unsigned radix = 16) noexcept {
                char digits[3 * sizeof(value)];
                unsigned n = 0;
                do {
                    digits[n++] = "0123456789abcdef"[value % radix];
                    value /= radix;
                } while (value);
                while (n && used < sizeof(bytes)) {
                    bytes[used++] = digits[--n];
                }
            }
            void send() noexcept {
                write_text(bytes, used);
                used = 0;
            }
        };
#if defined(_WIN32)
        /// The symbolizer child's environment, WITHOUT the variables that rewrite the tool's
        /// output protocol.
        ///
        /// Passed `nullptr`, CreateProcessW hands the child a COPY of the parent's environment -
        /// `LLVM_SYMBOLIZER_OPTS` included. MEASURED: with `LLVM_SYMBOLIZER_OPTS` set to an
        /// unknown option, llvm-symbolizer exits nonzero ("error: unknown argument"), which this
        /// service would report as `failed`; and an option that merely changes the output style
        /// (`--output-style=JSON`, `--print-address`) would mix foreign text into a block the
        /// caller reads as the fixed protocol. The offline decoder drops the same variable, so
        /// the two must agree. Names are case-insensitive on Windows, hence the fold.
        class filtered_environment {
        public:
            filtered_environment() noexcept {
                LPWCH const source = GetEnvironmentStringsW();
                if (!source) {
                    return;
                }
                std::size_t units = 1; // the terminating empty entry
                for (wchar_t const* entry = source; *entry; entry += std::wcslen(entry) + 1) {
                    if (!blocked(entry)) {
                        units += std::wcslen(entry) + 1;
                    }
                }
                // A pathological environment is inherited rather than failing symbolization: the
                // filter is a hardening measure, not a reason to lose the decoded frames.
                if (units <= maximum_units) {
                    this->block = static_cast<wchar_t*>(std::malloc(units * sizeof(wchar_t)));
                }
                if (this->block) {
                    std::size_t written = 0;
                    for (wchar_t const* entry = source; *entry; entry += std::wcslen(entry) + 1) {
                        if (blocked(entry)) {
                            continue;
                        }
                        auto const length = (std::wcslen(entry) + 1) * sizeof(wchar_t);
                        std::memcpy(this->block + written, entry, length);
                        written += length / sizeof(wchar_t);
                    }
                    this->block[written] = L'\0';
                }
                FreeEnvironmentStringsW(source);
            }
            ~filtered_environment() noexcept {
                if (this->block) {
                    std::free(this->block);
                }
            }
            filtered_environment(filtered_environment const&) = delete;
            filtered_environment& operator=(filtered_environment const&) = delete;
            [[nodiscard]] wchar_t* get() noexcept {
                return this->block;
            }

        private:
            static inline constexpr std::size_t maximum_units = static_cast<std::size_t>(512) * 1024; // 1 MiB of wchar_t
            static wchar_t lower(wchar_t value) noexcept {
                return value >= L'A' && value <= L'Z' ? static_cast<wchar_t>(value - L'A' + L'a') : value;
            }
            static bool blocked(wchar_t const* entry) noexcept {
                constexpr wchar_t name[] = L"llvm_symbolizer_opts=";
                // A terminator where the name still expects a character ends the comparison: the
                // entry is some other variable (the name itself is never a prefix of a shorter one).
                for (std::size_t i = 0; name[i]; ++i) {
                    if (lower(entry[i]) != name[i]) {
                        return false;
                    }
                }
                return true;
            }
            wchar_t* block = nullptr;
        };
#endif
    } // namespace

    bool prepare_output() noexcept {
#if defined(_WIN32)
        if (emergency_output.load(std::memory_order_acquire))
            return true;
#else
        if (emergency_output.load(std::memory_order_acquire) >= 0)
            return true;
#endif
        bool expected = false;
        if (!emergency_open_attempted.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return false; // never wait for another fatal reporter or retry forever
#if defined(_WIN32)
        HANDLE file = CreateFileW(L"deren-crash.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
#else
        int file = ::open("deren-crash.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (file < 0)
            return false;
        if (file < 3) {
            int const duplicate = fcntl(file, F_DUPFD_CLOEXEC, 3);
            close(file);
            file = duplicate;
            if (file < 0)
                return false;
        }
#endif
        emergency_output.store(file, std::memory_order_release);
        return true;
    }

    void write_preopened_text(char const* value, std::size_t size) noexcept {
        if (!value || !size)
            return;
#if defined(_WIN32)
        HANDLE const output = emergency_output.load(std::memory_order_acquire);
        if (output) {
            DWORD written = 0;
            static_cast<void>(WriteFile(output, value, static_cast<DWORD>(size), &written, nullptr));
        }
#else
        int const output = emergency_output.load(std::memory_order_acquire);
        if (output >= 0) {
            [[maybe_unused]] auto const written = ::write(output, value, size);
        }
#endif
    }
    void write_text(char const* value, std::size_t size) noexcept {
        static_cast<void>(prepare_output());
        write_preopened_text(value, size);
    }
    void write_panic_header(char const* message, std::size_t size, char const* file,
                            char const* function, std::uint32_t line) noexcept {
        write_text("deren panic: ", 13);
        write_text(message, std::min(size, std::size_t{4096}));
        write_text("\n", 1);
        line_buffer location;
        location.text("source: ");
        location.text(file);
        location.text(":");
        location.number(line, 10);
        location.text(" in ");
        location.send();
        line_buffer name;
        name.text(function);
        name.text("\n");
        name.send();
    }

    TRACE_NOINLINE trace capture(std::uint32_t skip, std::uint32_t depth) noexcept {
        trace result;
        depth = std::min(depth, static_cast<std::uint32_t>(capacity));
        if (!depth || skip > 1024) {
            return result;
        }
#if defined(_WIN32)
        void* addresses[capacity + 1]{};
        auto n = CaptureStackBackTrace(skip + 1, depth + 1, addresses, nullptr);
        result.truncated = n > depth;
        result.count = std::min(static_cast<std::uint32_t>(n), depth);
        for (std::uint32_t i = 0; i < result.count; ++i) {
            result.pc[i] = reinterpret_cast<std::uintptr_t>(addresses[i]);
        }
#else
        struct state {
            trace* output;
            std::uint32_t skip, depth;
        } state{&result, skip + 1, depth};
        _Unwind_Backtrace(+[](_Unwind_Context* context, void* opaque) -> _Unwind_Reason_Code {
            auto& s = *static_cast<struct state*>(opaque);
            if (s.skip) { --s.skip; return _URC_NO_REASON; }
            auto pc = static_cast<std::uintptr_t>(_Unwind_GetIP(context));
            if (!pc) return _URC_END_OF_STACK;
            if (s.output->count == s.depth) { s.output->truncated = true; return _URC_END_OF_STACK; }
            s.output->pc[s.output->count++] = pc;
            return _URC_NO_REASON; }, &state);
#endif
        return result;
    }

    bool resolve(std::uintptr_t pc, frame_info& frame) noexcept {
        frame = {};
        frame.offset = pc;
#if defined(_WIN32)
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(pc), &module)) {
            return false;
        }
        wchar_t path[1024];
        DWORD n = GetModuleFileNameW(module, path, 1024);
        if (!n || n >= 1024) {
            return false;
        }
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, static_cast<int>(n), frame.module,
                                 sizeof(frame.module) - 1, nullptr, nullptr)) {
            return false;
        }
        frame.base = reinterpret_cast<std::uintptr_t>(module);
#else
        Dl_info info{};
        if (!dladdr(reinterpret_cast<void*>(pc), &info) || !info.dli_fbase || !info.dli_fname)
            return false;
        auto length = std::strlen(info.dli_fname);
        if (length >= sizeof(frame.module))
            return false;
        std::memcpy(frame.module, info.dli_fname, length + 1);
        frame.base = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
#if defined(__linux__)
        // ELF symbol addresses use the load bias, which is zero for a non-PIE executable.
        struct lookup {
            std::uintptr_t pc;
            std::uintptr_t* base;
        } lookup{pc, &frame.base};
        dl_iterate_phdr(+[](dl_phdr_info* image, std::size_t, void* opaque) {
            auto& value = *static_cast<struct lookup*>(opaque);
            for (unsigned i = 0; i < image->dlpi_phnum; ++i) {
                auto const& segment = image->dlpi_phdr[i];
                auto const begin = image->dlpi_addr + segment.p_vaddr;
                if (segment.p_type == PT_LOAD && value.pc >= begin && value.pc - begin < segment.p_memsz) {
                    *value.base = image->dlpi_addr;
                    return 1;
                }
            }
            return 0; }, &lookup);
#endif
#endif
        frame.offset = pc - frame.base;
        return true;
    }

    void write_trace(trace const& value) noexcept {
        line_buffer b;
        b.text("deren raw stack v1 frames=");
        b.number(value.count, 10);
        b.text(" truncated=");
        b.number(value.truncated, 10);
        b.text("\n");
        b.send();
        for (std::uint32_t i = 0; i < std::min(value.count, static_cast<std::uint32_t>(capacity)); ++i) {
            frame_info frame;
            static_cast<void>(resolve(value.pc[i], frame));
            b.text("frame pc=0x");
            b.number(value.pc[i]);
            b.text(" base=0x");
            b.number(frame.base);
            b.text(" rva=0x");
            b.number(frame.offset);
            b.text(" module=");
            b.send();
            write_text(frame.module, std::strlen(frame.module));
            write_text("\n", 1);
        }
    }
    TRACE_NOINLINE void write_current() noexcept {
        write_trace(capture(1, capacity));
    }

    [[noreturn]] TRACE_NOINLINE void fast_panic(char const* message, std::size_t size) noexcept {
        static thread_local bool reporting = false;
        if (reporting) {
            std::_Exit(EXIT_FAILURE);
        }
        reporting = true;
        write_text("deren fast_panic: ", 18);
        write_text(message, std::min(size, std::size_t{4096}));
        write_text("\n", 1);
        std::abort();
    }

    char const* default_symbolizer() noexcept {
        return DEREN_LLVM_SYMBOLIZER;
    }
    std::uint64_t monotonic_ms() noexcept {
#if defined(_WIN32)
        return GetTickCount64();
#else
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
#endif
    }

#if defined(_WIN32)
    namespace {
        TRACE_NOINLINE LONG WINAPI crash_filter(EXCEPTION_POINTERS* exception) {
            static thread_local bool reporting = false;
            if (reporting || !exception || !exception->ExceptionRecord || !exception->ContextRecord) {
                return EXCEPTION_EXECUTE_HANDLER;
            }
            reporting = true;
            auto* record = exception->ExceptionRecord;
            auto fault = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
            line_buffer b;
            b.text("deren crash code=0x");
            b.number(record->ExceptionCode);
            b.text(" fault_pc=0x");
            b.number(fault);
            // An access violation's ExceptionInformation carries WHAT was touched, not just where
            // the code was: [0] is the access kind (0 read, 1 write, 8 execute) and [1] the data
            // address. That is the first question a corruption post-mortem asks, and the fault PC
            // alone cannot answer it.
            if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
                b.text(" access=");
                switch (record->ExceptionInformation[0]) {
                case 0:
                    b.text("read");
                    break;
                case 1:
                    b.text("write");
                    break;
                case 8:
                    b.text("execute");
                    break;
                default:
                    b.number(record->ExceptionInformation[0], 10);
                    break;
                }
                b.text(" data=0x");
                b.number(record->ExceptionInformation[1]);
            }
            b.text("\n");
            b.send();
            if (record->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
                b.text("stack overflow: context_pc=0x");
                b.number(fault);
                b.text(" (unwind skipped)\n");
                b.send();
                return EXCEPTION_EXECUTE_HANDLER;
            }
            trace captured;
            captured.pc[captured.count++] = fault;
#if defined(_M_X64) || defined(__x86_64__)
            CONTEXT context = *exception->ContextRecord;
            for (unsigned i = 1; i < capacity && context.Rip; ++i) {
                DWORD64 base = 0;
                auto previous_sp = context.Rsp;
                auto previous_pc = context.Rip;
                auto* function = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
                if (function) {
                    PVOID handler_data = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, function, &context,
                                     &handler_data, &establisher, nullptr);
                } else {
                    DWORD64 next = 0;
                    SIZE_T read = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(context.Rsp), &next, sizeof(next), &read) || read != sizeof(next)) {
                        break;
                    }
                    context.Rip = next;
                    context.Rsp += sizeof(next);
                }
                if (!context.Rip || context.Rsp <= previous_sp || context.Rsp - previous_sp > static_cast<std::uint64_t>(16) * 1024 * 1024 || context.Rip == previous_pc) {
                    break;
                }
                captured.pc[captured.count++] = context.Rip;
            }
#endif
            captured.truncated = captured.count == capacity;
            write_trace(captured);
            return EXCEPTION_EXECUTE_HANDLER;
        }
    } // namespace
#endif
#if defined(_WIN32)
    bool prepare_thread() noexcept {
        ULONG guarantee = 64 * 1024;
        return SetThreadStackGuarantee(&guarantee) != FALSE;
    }
    bool install_handler() noexcept {
        static_cast<void>(prepare_output());
        static_cast<void>(prepare_thread());
        SetUnhandledExceptionFilter(crash_filter);
        return true;
    }

    process_status symbolize_group(char const* executable, char const* module, std::uintptr_t const* offsets,
                                   std::uint32_t count, bool inlines, char* output, std::size_t cap,
                                   std::size_t& written, std::uint32_t timeout_ms) noexcept {
        written = 0;
        wchar_t application[2048], object[2048], command[8192];
        if (!count || count > capacity || !cap || !timeout_ms) {
            return process_status::failed;
        }
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, executable, -1, application, 2048) ||
            !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, module, -1, object, 2048)) {
            return process_status::failed;
        }
        std::size_t used = 0;
        bool valid = true;
        auto append = [&](wchar_t ch) { if (used + 1 < sizeof(command) / sizeof(command[0])) { command[used++] = ch; } else { valid = false; 
} };
        auto text = [&](wchar_t const* value) { while (*value) { append(*value++); 
} };
        auto quote = [&](wchar_t const* value) {
            append(L'"');
            while (*value) {
                unsigned slashes = 0;
                while (*value == L'\\') {
                    ++slashes;
                    ++value;
                }
                unsigned n = (*value == L'"' || !*value) ? 2 * slashes : slashes;
                while (n--) {
                    append(L'\\');
                }
                if (*value == L'"') {
                    append(L'\\');
                }
                if (*value) {
                    append(*value++);
                }
            }
            append(L'"');
        };
        quote(application);
        text(L" --obj=");
        quote(object);
        text(L" --relative-address --functions=linkage --demangle --no-debuginfod ");
        text(inlines ? L"--inlines" : L"--no-inlines");
        for (std::uint32_t i = 0; i < count; ++i) {
            text(L" 0x");
            wchar_t digits[2 * sizeof(std::uintptr_t)];
            unsigned n = 0;
            auto value = offsets[i];
            do {
                digits[n++] = L"0123456789abcdef"[value % 16];
                value /= 16;
            } while (value);
            while (n) {
                append(digits[--n]);
            }
        }
        command[used] = 0;
        if (!valid) {
            return process_status::failed;
        }

        HANDLE reader = nullptr, writer = nullptr, null_file = INVALID_HANDLE_VALUE, job = nullptr;
        PROCESS_INFORMATION process{};
        STARTUPINFOEXW startup{};
        bool attributes_ready = false;
        auto cleanup = [&] {
            if (job) {
                CloseHandle(job); // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE contains descendants too.
            }
            if (process.hThread) {
                CloseHandle(process.hThread);
            }
            if (process.hProcess) {
                CloseHandle(process.hProcess);
            }
            if (attributes_ready) {
                DeleteProcThreadAttributeList(startup.lpAttributeList);
            }
            if (reader) {
                CloseHandle(reader);
            }
            if (writer) {
                CloseHandle(writer);
            }
            if (null_file != INVALID_HANDLE_VALUE) {
                CloseHandle(null_file);
            }
        };
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        if (!CreatePipe(&reader, &writer, &security, 0)) {
            return process_status::failed;
        }
        if (!SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0)) {
            cleanup();
            return process_status::failed;
        }
        null_file = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &security, OPEN_EXISTING, 0, nullptr);
        if (null_file == INVALID_HANDLE_VALUE) {
            cleanup();
            return process_status::failed;
        }
        alignas(void*) unsigned char attributes[2048];
        SIZE_T attribute_bytes = sizeof(attributes);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes);
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_bytes)) {
            cleanup();
            return process_status::failed;
        }
        attributes_ready = true;
        HANDLE inherited[]{writer, null_file};
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       static_cast<void*>(inherited), sizeof(inherited), nullptr, nullptr)) {
            cleanup();
            return process_status::failed;
        }
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = writer;
        startup.StartupInfo.hStdError = null_file;
        startup.StartupInfo.hStdInput = null_file;
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            cleanup();
            return process_status::failed;
        }
        // A non-null environment block must be Unicode, hence CREATE_UNICODE_ENVIRONMENT; the
        // nullptr fallback keeps the old "inherit" behaviour for a pathological environment.
        filtered_environment environment;
        DWORD const creation_flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT |
                                     (environment.get() ? CREATE_UNICODE_ENVIRONMENT : 0);
        if (!CreateProcessW(application, command, nullptr, nullptr, TRUE, creation_flags, environment.get(), nullptr,
                            &startup.StartupInfo, &process)) {
            cleanup();
            return process_status::unavailable;
        }
        if (!AssignProcessToJobObject(job, process.hProcess)) {
            TerminateProcess(process.hProcess, 99);
            cleanup();
            return process_status::failed;
        }
        CloseHandle(writer);
        writer = nullptr;
        if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
            cleanup();
            return process_status::failed;
        }
        auto deadline = monotonic_ms() + timeout_ms;
        process_status status = process_status::ok;
        for (;;) {
            DWORD available = 0;
            if (PeekNamedPipe(reader, nullptr, 0, nullptr, &available, nullptr) && available) {
                if (written == cap) {
                    status = process_status::output_limit;
                    break;
                }
                DWORD got = 0;
                auto request = static_cast<DWORD>(std::min(std::size_t{available}, cap - written));
                if (!ReadFile(reader, output + written, request, &got, nullptr) || !got) {
                    status = process_status::failed;
                    break;
                }
                written += got;
            } else if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
                break;
            }
            if (monotonic_ms() >= deadline) {
                status = process_status::timeout;
                break;
            }
            // The loop already broke when the child exited with nothing left to read, and the
            // job's kill-on-close cleanup ends any descendant still holding the write end. This
            // wait only paces the next PeekNamedPipe; it is NOT a wait for descendants' output.
            static_cast<void>(WaitForSingleObject(process.hProcess, 1));
        }
        DWORD code = 0;
        if (status == process_status::ok && (!GetExitCodeProcess(process.hProcess, &code) || code != 0)) {
            status = process_status::failed;
        }
        cleanup();
        return status;
    }
#endif // Windows process and exception handling; POSIX lives in stacktrace_posix.cpp.
} // namespace deren::utility::detail::trace_native
