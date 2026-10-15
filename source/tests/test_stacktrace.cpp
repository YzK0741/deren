#include "../utility/shared/memory_test_hooks.hpp"
#include "vessel_child.hpp"
#include "vk_test.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <thread>
#include <type_traits>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#define TRACE_ASAN 1
#else
#define TRACE_ASAN 0
#endif
import deren.utility.shared;
import deren.utility.dynamic_link;
namespace diag = deren::utility::diagnostics;
namespace vessel = deren::utility::vessel;

#if defined(_MSC_VER)
#define TRACE_NOINLINE __declspec(noinline)
#else
#define TRACE_NOINLINE __attribute__((noinline))
#endif

// A pipe to a child, for the offline-decoder cross-check below.
#if defined(_WIN32)
#define TRACE_POPEN _popen
#define TRACE_PCLOSE _pclose

/// The loader-lock fixture's DllMain is entered INSIDE LoadLibrary, so it cannot be handed the
/// event names: both sides derive them from the process id alone.
void loader_lock_event_name(wchar_t const* prefix, wchar_t (&name)[128]) noexcept {
    unsigned used = 0;
    while (prefix[used] && used + 1 < 128) {
        name[used] = prefix[used];
        ++used;
    }
    wchar_t digits[24];
    unsigned count = 0;
    DWORD pid = GetCurrentProcessId();
    do {
        digits[count++] = static_cast<wchar_t>(L'0' + pid % 10);
        pid /= 10;
    } while (pid != 0);
    while (count != 0 && used + 1 < 128) {
        name[used++] = digits[--count];
    }
    name[used] = L'\0';
}
#else
#define TRACE_POPEN popen
#define TRACE_PCLOSE pclose
#endif

TRACE_NOINLINE diag::stack_trace capture_site() {
    return diag::capture_stack(0, 8);
}
TRACE_NOINLINE void capture_callback(void* state) {
    *static_cast<diag::stack_trace*>(state) = diag::capture_stack();
}
extern "C" void trace_fault();
extern "C" void trace_stack_overflow();
extern "C" void trace_detach_stderr();

namespace {
    /// Constructed during static initialization, i.e. BEFORE main forces the log sink into
    /// existence, so its destructor runs AFTER the sink would have been destroyed: the late-write
    /// window that must drop the message instead of locking a dead mutex, and must not hang.
    struct late_writer {
        late_writer() = default;
        ~late_writer() {
            deren::utility::log_text("late write after shutdown");
            deren::utility::wait_log_all();
        }
    };
    late_writer late_writer_instance;
} // namespace
vessel::string read_log(char const* file) {
    std::ifstream log(file);
    return {std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
}
std::uintptr_t field(vessel::string const& text, std::string_view name) {
    auto at = text.find(name);
    if (at == vessel::string::npos)
        return 0;
    at += name.size();
    std::uintptr_t result = 0;
    static_cast<void>(std::from_chars(text.data() + at, text.data() + text.size(), result, 16));
    return result;
}
static_assert(noexcept(deren::utility::fast_panic(std::declval<std::string_view>())));
static_assert(!std::is_invocable_v<decltype(&deren::utility::fast_panic), std::string_view, int>);
int main(int argc, char** argv) {
    if (argc == 2) {
        vessel_prepare_child();
        if (std::string_view(argv[1]) == "--loader-lock-fault") {
            // The fault happens INSIDE the fixture's DllMain, so it is raised on THIS thread while
            // that thread holds the loader lock - the second way a filter can meet a held lock.
            static_cast<void>(diag::install_crash_handler());
            wchar_t fault_name[128];
            loader_lock_event_name(L"deren-loader-fixture-fault-", fault_name);
            HANDLE const fault_event = CreateEventW(nullptr, TRUE, FALSE, fault_name);
            static_cast<void>(fault_event);
            static_cast<void>(LoadLibraryA(STACKTRACE_LOADER_FIXTURE));
            return 1;
        }
        if (std::string_view(argv[1]) == "--loader-lock-crash") {
            // ONE THREAD BLOCKS INSIDE LoadLibrary (the fixture's DllMain holds the loader lock),
            // and THIS thread faults while that lock is held. The filter has to write its record
            // and terminate from the snapshot alone: asking the loader for a module would wait on
            // the lock the blocked thread never releases.
            static_cast<void>(diag::install_crash_handler());
            wchar_t entered_name[128];
            wchar_t release_name[128];
            loader_lock_event_name(L"deren-loader-fixture-entered-", entered_name);
            loader_lock_event_name(L"deren-loader-fixture-release-", release_name);
            HANDLE const entered = CreateEventW(nullptr, TRUE, FALSE, entered_name);
            HANDLE const release = CreateEventW(nullptr, TRUE, FALSE, release_name);
            std::thread loader([] { static_cast<void>(LoadLibraryA(STACKTRACE_LOADER_FIXTURE)); });
            if (entered && WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0) {
                loader.detach(); // it is inside the fixture's DllMain, holding the loader lock
                trace_fault();   // and this thread faults right there
            }
            static_cast<void>(release);
            return 1;
        }
        if (std::string_view(argv[1]) == "--no-stderr") {
            trace_detach_stderr();
            static_cast<void>(diag::install_crash_handler());
            utility_memory_test_fail_after(0);
            deren::utility::fast_panic("preopened fallback output");
        }
        if (std::string_view(argv[1]) == "--no-stderr-no-install") {
            // The window between process start and main's install call: no handler, no preopened
            // handle, no stderr. The fatal path has to leave its evidence anyway.
            trace_detach_stderr();
            deren::utility::fast_panic("lazy fallback output");
        }
        if (std::string_view(argv[1]) == "--late-log") {
            // Force the sink into existence, then return: late_writer_instance's destructor runs
            // after the sink's shutdown and must return instead of hanging or crashing.
            deren::utility::log_text("forcing the log sink into existence");
            deren::utility::wait_log_all();
            return 0;
        }
        if (std::string_view(argv[1]) == "--exit-log") {
            // NO explicit wait: the drain registered at first use must still write this line when
            // the process ends. That drain is what the never-destroyed sink has to preserve.
            deren::utility::log_text("exit drain marker");
            return 0;
        }
        if (std::string_view(argv[1]) == "--crash" || std::string_view(argv[1]) == "--overflow") {
            static_cast<void>(diag::install_crash_handler());
            if (std::string_view(argv[1]) == "--overflow")
                trace_stack_overflow();
            else
                trace_fault();
            return 1;
        }
        if (std::string_view(argv[1]) == "--worker-overflow") {
            static_cast<void>(diag::install_crash_handler());
            std::thread worker([] {
                static_cast<void>(diag::prepare_crash_thread());
                trace_stack_overflow();
            });
            worker.join();
            return 1;
        }
        if (std::string_view(argv[1]) == "--panic")
            deren::utility::panic("ordinary panic diagnostics");
        if (std::string_view(argv[1]) == "--long-fast") {
            vessel::string reason(8192, 'x');
            utility_memory_test_fail_after(0);
            deren::utility::fast_panic(reason);
        }
        utility_memory_test_fail_after(0);
        deren::utility::fast_panic("fast failure under injected OOM");
    }
    auto trace = capture_site();
    CHECK(trace.count > 0 && trace.count <= 8);
    CHECK(diag::capture_stack(0, 0).count == 0);
    CHECK(diag::capture_stack(1000000, 8).count == 0);
    CHECK(diag::capture_stack(0, 1).count == 1);
    utility_memory_test_fail_after(0);
    auto no_heap = diag::capture_stack();
    CHECK(no_heap.count > 0);
    CHECK(!deren::utility::memory::try_allocate(8));
    diag::symbolizer_options options;
    options.executable = "off";
    auto raw = diag::symbolize(trace, options);
    CHECK(raw.status == diag::symbolizer_status::unavailable);
    CHECK(raw.text.find("rva=0x") != vessel::string::npos);
    auto child = vessel_run_child(argv[0], "--fast", "fast.log");
    CHECK(child.started && child.aborted);
    std::ifstream log("fast.log");
    vessel::string text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    CHECK(text.find("fast failure under injected OOM") != vessel::string::npos);
    CHECK(text.find("origin ") == vessel::string::npos);
    CHECK(text.find("frame pc=0x") == vessel::string::npos);
    CHECK(text.find("abort signal received") != vessel::string::npos);
    auto long_child = vessel_run_child(argv[0], "--long-fast", "long-fast.log");
    CHECK(long_child.started && long_child.aborted);
    auto long_text = read_log("long-fast.log");
    CHECK(std::count(long_text.begin(), long_text.end(), 'x') == 4096);
#if defined(_WIN32)
    options.executable = "C:/deren-no-such-symbolizer.exe";
    CHECK(diag::symbolize(trace, options).status == diag::symbolizer_status::unavailable);
    options.executable = std::string_view("bad\0tool", 8);
    CHECK(diag::symbolize(trace, options).status == diag::symbolizer_status::failed);
    options.executable = STACKTRACE_FIXTURE;
    options.max_symbolizer_output_bytes = 1024;
    auto oversized = diag::symbolize(trace, options);
    CHECK(oversized.status == diag::symbolizer_status::output_limit);
    CHECK(oversized.text.find("rva=0x") != vessel::string::npos);
    options.inlines = false;
    options.timeout_ms = 40;
    auto started = std::chrono::steady_clock::now();
    auto timed = diag::symbolize(trace, options);
    CHECK(timed.status == diag::symbolizer_status::timeout);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
    CHECK(timed.text.find("rva=0x") != vessel::string::npos);
#if STACKTRACE_DIAGNOSTICS && STACKTRACE_HAVE_SYMBOLIZER
    auto symbols = diag::symbolize(trace);
    CHECK(symbols.text.find("capture_site") != vessel::string::npos);
    CHECK(symbols.text.find("test_stacktrace.cpp:") != vessel::string::npos);
    auto library = deren::utility::dynamic_link::load(STACKTRACE_PROBE);
    CHECK(library.has_value());
    if (library) {
        auto entry = library->symbol("vessel_probe_trace");
        CHECK(entry.has_value());
        if (entry) {
            diag::stack_trace across;
            reinterpret_cast<void (*)(void (*)(void*), void*)>(const_cast<void*>(*entry))(capture_callback, &across);
            auto decoded = diag::symbolize(across);
            CHECK(decoded.text.find("capture_callback") != vessel::string::npos);
            CHECK(decoded.text.find("vessel_probe_trace") != vessel::string::npos);
            CHECK(decoded.text.find("vessel_probe.cpp:") != vessel::string::npos);
        }
    }
#endif
    // A tool that exits cleanly WITHOUT writing anything decoded nothing: `ok` would claim
    // symbols that do not exist. The mode is read from the child's environment, so the same
    // check also pins that ordinary variables still reach it (an over-eager filter would leave
    // the fixture sleeping and the status `timeout`, failing the second assertion).
    _putenv_s("STACKTRACE_FIXTURE_MODE", "quiet");
    options.executable = STACKTRACE_FIXTURE;
    options.inlines = true;
    options.timeout_ms = 1000;
    options.max_symbolizer_output_bytes = 64 * 1024;
    CHECK(diag::symbolize(trace, options).status != diag::symbolizer_status::ok);
    _putenv_s("STACKTRACE_FIXTURE_MODE", "");
#if STACKTRACE_DIAGNOSTICS && STACKTRACE_HAVE_SYMBOLIZER
    // LLVM_SYMBOLIZER_OPTS rewrites the tool's OUTPUT PROTOCOL, so the child must not inherit it.
    // The probe tool reports what IT received: the marker must be gone, while the ordinary
    // variable that selected the probe must survive the same filter.
    _putenv_s("STACKTRACE_FIXTURE_MODE", "env-probe");
    _putenv_s("LLVM_SYMBOLIZER_OPTS", "deren-env-marker");
    options.executable = STACKTRACE_FIXTURE;
    options.inlines = true;
    options.timeout_ms = 1000;
    auto const probed = diag::symbolize(trace, options);
    CHECK(probed.status == diag::symbolizer_status::ok);
    CHECK(probed.text.find("mode=env-probe llvm_symbolizer_opts=(unset)") != vessel::string::npos);
    _putenv_s("STACKTRACE_FIXTURE_MODE", "");
    _putenv_s("LLVM_SYMBOLIZER_OPTS", "");
    // And the REAL tool with the same stray variable set. The assertion is the DECODED EXE FRAME,
    // not the status: this trace also reaches KERNEL32/ntdll, whose symbols are absent and whose
    // group reports `failed` either way (measured: "kernel32.pdb: No such file or directory"). A
    // variable that reached the child would fail EVERY group, so `capture_site` would be gone.
    _putenv_s("LLVM_SYMBOLIZER_OPTS", "--deren-not-a-real-option");
    auto const stray_environment = diag::symbolize(trace);
    CHECK(stray_environment.text.find("capture_site") != vessel::string::npos);
    CHECK(stray_environment.text.find("test_stacktrace.cpp:") != vessel::string::npos);
    _putenv_s("LLVM_SYMBOLIZER_OPTS", "");
#endif
    // ASAN owns native fault handling; exercise our OS filter in the Release suite.
#if !TRACE_ASAN
    auto crashed = vessel_run_child(argv[0], "--crash", "crash.log");
    CHECK(crashed.started && crashed.code == 0xc0000005u);
    auto crash_text = read_log("crash.log");
    CHECK(crash_text.find("deren crash code=0xc0000005") != vessel::string::npos);
    CHECK(field(crash_text, "fault_pc=0x") != 0);
    CHECK(field(crash_text, "fault_pc=0x") == field(crash_text, "frame pc=0x"));
    // WHAT the violation touched, not only where the code was: the probe writes to a
    // PAGE_NOACCESS page, so the access kind and the data address are both recorded.
    CHECK(crash_text.find(" access=write data=0x") != vessel::string::npos);
#if STACKTRACE_DIAGNOSTICS && STACKTRACE_HAVE_SYMBOLIZER
    // THE FAULT PC IS EXACT, NOT A RETURN ADDRESS. The crash log carries the fault frame's RVA,
    // which does not move with ASLR, so this rebuilds that frame against THIS image and decodes
    // it twice: as the exact PC a crash record holds, and as a return address. MEASURED: exact
    // lands on stacktrace_fault.cpp:18 (`*page = 42;`), one byte lower on :16 - which is why a
    // blanket `pc - 1` misreports the faulting line.
    diag::symbolizer_options raw_only;
    raw_only.executable = "off";
    auto const own_base = field(diag::symbolize(capture_site(), raw_only).text, "base=0x");
    auto const fault_rva = field(crash_text, "rva=0x");
    CHECK(own_base != 0 && fault_rva != 0);
    if (own_base && fault_rva) {
        diag::stack_trace fault;
        fault.count = 1;
        fault.addresses[0] = own_base + fault_rva;
        fault.exact_address_count = 1;
        auto const exact = diag::symbolize(fault);
        fault.exact_address_count = 0;
        auto const adjusted = diag::symbolize(fault);
        CHECK(exact.text.find("stacktrace_fault.cpp:18") != vessel::string::npos);
        CHECK(adjusted.text.find("stacktrace_fault.cpp:18") == vessel::string::npos);
    }
#if defined(STACKTRACE_DECODER) && defined(STACKTRACE_PYTHON)
    // THE CROSS-CHECK, and why it is here: the fault-frame rule lived in TWO decoders with no test
    // comparing them, which is exactly how a blanket `pc - 1` survived in this one while the
    // offline tool did it right. Both now read the SAME raw record the child just wrote, and the
    // offline tool must print the fault line the in-process decoder was asserted on above.
    {
        auto const exe_directory = std::filesystem::path(argv[0]).parent_path();
        auto const module_dir = exe_directory.empty() ? std::string{"."} : exe_directory.string();
        vessel::string const inner = vessel::format("\"{}\" \"{}\" \"crash.log\" --module-dir \"{}\"",
                                                    STACKTRACE_PYTHON, STACKTRACE_DECODER, module_dir);
        // cmd.exe strips the FIRST and LAST quote of a `cmd /c <command>` that begins with a quote,
        // and this command both begins and ends with one - so it needs a second pair around the
        // whole thing (measured: without it the child dies with "the filename ... is incorrect").
        vessel::string const command = vessel::format("\"{}\"", inner.c_str());
        auto* const pipe = TRACE_POPEN(command.c_str(), "r");
        CHECK(pipe != nullptr);
        vessel::string decoded;
        if (pipe) {
            char buffer[4096];
            while (std::fgets(buffer, sizeof(buffer), pipe)) {
                decoded += buffer;
            }
            static_cast<void>(TRACE_PCLOSE(pipe));
        }
        CHECK(decoded.find("stacktrace_fault.cpp:18") != vessel::string::npos);
    }
#endif
#endif
    // A fault while ANOTHER thread holds the loader lock (the fixture's DllMain blocks inside
    // LoadLibrary). The crash line and the frames have to come out, and the process has to
    // terminate: a filter that asks the loader for a module waits on a lock the blocked thread
    // never releases, so the harness ends up killing the child and `started` comes back false.
    // It does not need the symbolizer - the record is raw text - so it stays outside that guard.
    auto loader_lock = vessel_run_child(argv[0], "--loader-lock-crash", "loader-lock.log");
    CHECK(loader_lock.started);
    auto const loader_lock_text = read_log("loader-lock.log");
    CHECK(loader_lock_text.find("deren crash code=0xc0000005") != vessel::string::npos);
    CHECK(loader_lock_text.find("frame pc=0x") != vessel::string::npos);
    // And the same-thread half: the fault is raised inside the fixture's DllMain, so if it reached
    // the filter the filter would run on the thread holding the lock. MEASURED: it never arrives -
    // the loader turns an exception in DllMain into a FAILED LOAD (LoadLibrary returns null), not
    // an unhandled exception - so there is nothing to record. That is why this half cannot hang the
    // filter: it does not get there. The blocked-holder case above is the one that does, and it
    // completes, so the fatal path's module lookup is empirically NOT hostage to the loader lock.
    auto loader_lock_fault = vessel_run_child(argv[0], "--loader-lock-fault", "loader-lock-fault.log");
    CHECK(loader_lock_fault.started);
    CHECK(read_log("loader-lock-fault.log").find("deren crash") == vessel::string::npos);
    auto overflow = vessel_run_child(argv[0], "--overflow", "overflow.log");
    CHECK(overflow.started && overflow.code == 0xc00000fdu);
    CHECK(read_log("overflow.log").find("unwind skipped") != vessel::string::npos);
    auto worker_overflow = vessel_run_child(argv[0], "--worker-overflow", "worker-overflow.log");
    CHECK(worker_overflow.started && worker_overflow.code == 0xc00000fdu);
    CHECK(read_log("worker-overflow.log").find("unwind skipped") != vessel::string::npos);
#endif
    static_cast<void>(std::filesystem::remove("deren-crash.log"));
    auto no_stderr = vessel_run_child(argv[0], "--no-stderr", "no-stderr.log");
    CHECK(no_stderr.started && no_stderr.aborted);
    CHECK(read_log("deren-crash.log").find("preopened fallback output") != vessel::string::npos);
    // The install-order window: no handler ran, so nothing preopened the fallback - and the fatal
    // path still has to leave evidence instead of dropping its only message.
    static_cast<void>(std::filesystem::remove("deren-crash.log"));
    auto lazy_fallback = vessel_run_child(argv[0], "--no-stderr-no-install", "lazy-fallback.log");
    CHECK(lazy_fallback.started && lazy_fallback.aborted);
    CHECK(read_log("deren-crash.log").find("lazy fallback output") != vessel::string::npos);
    // The late-write window: a static destructor ordered AFTER the sink's shutdown still calls
    // log() + wait_log_all(). It must return (exit 0) rather than lock a destroyed mutex; a hang
    // would show up here as the harness killing the child instead of an exit code.
    auto late = vessel_run_child(argv[0], "--late-log", "late.log");
    CHECK(late.started && late.code == 0);
    // The drain at exit still has to happen even though the sink is never destroyed: the child
    // queues one line and returns WITHOUT waiting. Release writes debug.log in the working
    // directory, Debug writes stdout, which the harness captures - either one proves the line was
    // not lost when the process ended.
    auto exit_drain = vessel_run_child(argv[0], "--exit-log", "exit-drain.log");
    CHECK(exit_drain.started && exit_drain.code == 0);
    CHECK(read_log("debug.log").find("exit drain marker") != vessel::string::npos ||
          read_log("exit-drain.log").find("exit drain marker") != vessel::string::npos);
#endif
    auto panic = vessel_run_child(argv[0], "--panic", "panic.log");
    CHECK(panic.started && panic.aborted);
    // The raw records still go to stderr first - that is what the offline decoder reads.
    CHECK(read_log("panic.log").find("frame pc=0x") != vessel::string::npos);
    // And the SAME panic prints a readable stack through its log path: Release queues it to
    // debug.log in the working directory, Debug writes it straight to stderr, so either file proves
    // it. The header alone is asserted here because it is present even when no symbols could be
    // produced; the guarded check requires the symbols themselves.
    CHECK(read_log("debug.log").find("call stack:") != vessel::string::npos ||
          read_log("panic.log").find("call stack:") != vessel::string::npos);
#if STACKTRACE_DIAGNOSTICS && STACKTRACE_HAVE_SYMBOLIZER
    CHECK(read_log("debug.log").find("symbols frames=") != vessel::string::npos ||
          read_log("panic.log").find("symbols frames=") != vessel::string::npos);
#endif
    return deren::vk_test::finish("test_stacktrace");
}
