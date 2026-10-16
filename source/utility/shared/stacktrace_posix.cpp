// Signal reporting is deliberately smaller than healthy-context stack diagnostics.
#if !defined(_WIN32)
#include "stacktrace_native.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

extern char** environ;
namespace deren::utility::detail::trace_native {
    namespace {
        std::uintptr_t fault_pc(void* raw) noexcept {
            if (!raw)
                return 0;
            auto const& context = *static_cast<ucontext_t const*>(raw);
#if defined(__linux__) && defined(__x86_64__)
            return static_cast<std::uintptr_t>(context.uc_mcontext.gregs[REG_RIP]);
#elif defined(__linux__) && defined(__aarch64__)
            return context.uc_mcontext.pc;
#else
            static_cast<void>(context);
            return 0;
#endif
        }
        void signal_handler(int signal, siginfo_t* info, void* context) noexcept {
            // No unwind, dladdr, stdio, allocation or child process on a signal stack.
            char line[256];
            std::size_t used = 0;
            auto text = [&](char const* p) { while (*p && used < sizeof(line)) line[used++] = *p++; };
            auto number = [&](std::uintptr_t value, unsigned radix) {
                char digits[3 * sizeof(value)];
                unsigned count = 0;
                do {
                    digits[count++] = "0123456789abcdef"[value % radix];
                    value /= radix;
                } while (value);
                while (count && used < sizeof(line))
                    line[used++] = digits[--count];
            };
            text("deren crash signal=");
            number(static_cast<unsigned>(signal), 10);
            text(" fault_pc=0x");
            number(fault_pc(context), 16);
            if (info && (signal == SIGSEGV || signal == SIGBUS)) {
                text(" data=0x");
                number(reinterpret_cast<std::uintptr_t>(info->si_addr), 16);
            }
            text(" (unwind skipped: signal context)\n");
            [[maybe_unused]] auto const written = ::write(STDERR_FILENO, line, used);
            // SA_RESETHAND already restored the default action. Preserve signal/core semantics.
            sigset_t unblocked;
            sigemptyset(&unblocked);
            sigaddset(&unblocked, signal);
            sigprocmask(SIG_UNBLOCK, &unblocked, nullptr);
            raise(signal);
            _exit(128 + signal);
        }
        int above_stdio(int fd) noexcept {
            if (fd < 0 || fd >= 3)
                return fd;
            int const copy = fcntl(fd, F_DUPFD_CLOEXEC, 3);
            close(fd);
            return copy;
        }
    } // namespace

    bool prepare_thread() noexcept {
        stack_t current{};
        if (sigaltstack(nullptr, &current))
            return false;
        if (!(current.ss_flags & SS_DISABLE))
            return true; // Respect another library's stack.
        alignas(16) static thread_local unsigned char alternate[64 * 1024];
        stack_t stack{};
        stack.ss_sp = alternate;
        stack.ss_size = sizeof(alternate);
        return sigaltstack(&stack, nullptr) == 0;
    }
    bool install_handler() noexcept {
        if (!prepare_thread())
            return false;
        constexpr int signals[]{SIGSEGV, SIGBUS, SIGILL, SIGFPE};
        struct sigaction previous[4]{};
        struct sigaction action{};
        action.sa_sigaction = signal_handler;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
        sigemptyset(&action.sa_mask);
        for (unsigned i = 0; i < 4; ++i) {
            if (sigaction(signals[i], &action, &previous[i])) {
                while (i) {
                    --i;
                    sigaction(signals[i], &previous[i], nullptr);
                }
                return false;
            }
        }
        // SIGABRT is intentionally untouched: fast_panic/OOM does not acquire crash machinery.
        return true;
    }

    process_status symbolize_group(char const* executable, char const* module, std::uintptr_t const* offsets,
                                   std::uint32_t count, bool inlines, char* output, std::size_t cap,
                                   std::size_t& written, std::uint32_t timeout_ms) noexcept {
        written = 0;
        if (!executable || !module || !offsets || !output || !count || count > capacity || !cap || !timeout_ms)
            return process_status::failed;
        char addresses[capacity][2 * sizeof(std::uintptr_t) + 3];
        char* arguments[capacity + 10];
        unsigned argc = 0;
        arguments[argc++] = const_cast<char*>(executable);
        arguments[argc++] = const_cast<char*>("--obj");
        arguments[argc++] = const_cast<char*>(module);
        arguments[argc++] = const_cast<char*>("--relative-address");
        arguments[argc++] = const_cast<char*>("--functions=linkage");
        arguments[argc++] = const_cast<char*>("--demangle");
        arguments[argc++] = const_cast<char*>("--no-debuginfod");
        arguments[argc++] = const_cast<char*>(inlines ? "--inlines" : "--no-inlines");
        for (std::uint32_t i = 0; i < count; ++i) {
            std::snprintf(addresses[i], sizeof(addresses[i]), "0x%llx", static_cast<unsigned long long>(offsets[i]));
            arguments[argc++] = addresses[i];
        }
        arguments[argc] = nullptr;
        std::size_t variables = 0;
        while (environ[variables] && variables < 65536)
            ++variables;
        if (variables == 65536)
            return process_status::failed;
        auto** environment = static_cast<char**>(std::malloc((variables + 1) * sizeof(char*)));
        if (!environment)
            return process_status::failed;
        std::size_t kept = 0;
        for (std::size_t i = 0; i < variables; ++i)
            if (std::strncmp(environ[i], "LLVM_SYMBOLIZER_OPTS=", 21) != 0)
                environment[kept++] = environ[i];
        environment[kept] = nullptr;

        int pipes[2]{-1, -1};
        int null_fd = -1;
        pid_t child = -1;
        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attributes;
        bool actions_ready = false, attributes_ready = false;
        auto cleanup = [&] {
            if (child > 0) {
                kill(-child, SIGKILL); // A separate process group contains ordinary descendants.
                int status;
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
                }
            }
            if (pipes[0] >= 0)
                close(pipes[0]);
            if (pipes[1] >= 0)
                close(pipes[1]);
            if (null_fd >= 0)
                close(null_fd);
            if (actions_ready)
                posix_spawn_file_actions_destroy(&actions);
            if (attributes_ready)
                posix_spawnattr_destroy(&attributes);
            std::free(environment);
        };
        if (pipe(pipes)) {
            cleanup();
            return process_status::failed;
        }
        pipes[0] = above_stdio(pipes[0]);
        pipes[1] = above_stdio(pipes[1]);
        null_fd = above_stdio(open("/dev/null", O_RDWR | O_CLOEXEC));
        if (pipes[0] < 0 || pipes[1] < 0 || null_fd < 0 ||
            fcntl(pipes[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(pipes[1], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(pipes[0], F_SETFL, O_NONBLOCK) < 0) {
            cleanup();
            return process_status::failed;
        }
        actions_ready = posix_spawn_file_actions_init(&actions) == 0;
        attributes_ready = posix_spawnattr_init(&attributes) == 0;
        sigset_t mask;
        sigemptyset(&mask);
        if (!actions_ready || !attributes_ready ||
            posix_spawn_file_actions_adddup2(&actions, null_fd, STDIN_FILENO) ||
            posix_spawn_file_actions_adddup2(&actions, null_fd, STDERR_FILENO) ||
            posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO) ||
            posix_spawn_file_actions_addclose(&actions, pipes[0]) ||
            posix_spawn_file_actions_addclose(&actions, pipes[1]) ||
            posix_spawn_file_actions_addclose(&actions, null_fd) ||
            posix_spawnattr_setpgroup(&attributes, 0) || posix_spawnattr_setsigmask(&attributes, &mask) ||
            posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK)) {
            cleanup();
            return process_status::failed;
        }
        int const error = posix_spawn(&child, executable, &actions, &attributes, arguments, environment);
        if (error) {
            child = -1;
            cleanup();
            return error == ENOENT || error == EACCES ? process_status::unavailable : process_status::failed;
        }
        close(pipes[1]);
        pipes[1] = -1;
        auto const deadline = monotonic_ms() + timeout_ms;
        auto result = process_status::ok;
        bool exited = false;
        for (;;) {
            char extra;
            auto const got = read(pipes[0], written < cap ? output + written : &extra, written < cap ? cap - written : 1);
            if (got > 0) {
                if (written == cap) {
                    result = process_status::output_limit;
                    break;
                }
                written += static_cast<std::size_t>(got);
            } else if (got < 0 && errno != EAGAIN && errno != EINTR) {
                result = process_status::failed;
                break;
            } else if (exited && (got == 0 || errno == EAGAIN))
                break;
            siginfo_t status{};
            if (waitid(P_PID, static_cast<id_t>(child), &status, WEXITED | WNOHANG | WNOWAIT)) {
                if (errno != EINTR) {
                    result = process_status::failed;
                    break;
                }
            } else if (status.si_pid) {
                exited = true; // Leave it unreaped until group cleanup, preventing PID reuse.
                if (status.si_code != CLD_EXITED || status.si_status)
                    result = process_status::failed;
                continue; // Drain anything queued before the child exited.
            }
            auto const now = monotonic_ms();
            if (now >= deadline) {
                result = process_status::timeout;
                break;
            }
            pollfd readable{pipes[0], POLLIN, 0};
            poll(&readable, 1, static_cast<int>(std::min<std::uint64_t>(deadline - now, 10)));
        }
        cleanup();
        return result;
    }
} // namespace deren::utility::detail::trace_native
#endif
