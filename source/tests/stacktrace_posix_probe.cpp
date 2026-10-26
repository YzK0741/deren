// Plain native probe: can also run without the renderer's C++ module toolchain.
#include "../utility/shared/stacktrace_native.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
namespace native = deren::utility::detail::trace_native;
__attribute__((noinline)) native::trace posix_capture_site() {
    return native::capture(0, 8);
}
__attribute__((noinline)) void overflow(unsigned volatile* condition) {
    char volatile pad[4096];
    pad[0] = static_cast<char>(*condition);
    if (*condition)
        overflow(condition);
    pad[4095] = pad[0];
}
void* worker_overflow(void*) {
    if (!native::prepare_thread())
        std::_Exit(90);
    unsigned volatile condition = 1;
    overflow(&condition);
    return nullptr;
}
int main(int argc, char** argv) {
    if (argc < 2)
        return 2;
    if (std::strcmp(argv[1], "--fast") == 0) {
        native::install_handler();
        native::fast_panic("minimal POSIX fatal", 19);
    }
    if (std::strcmp(argv[1], "--crash") == 0) {
        if (!native::install_handler())
            return 90;
        auto* page = static_cast<char volatile*>(mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        if (page == MAP_FAILED)
            return 91;
        *page = 42;
        return 92;
    }
    if (std::strcmp(argv[1], "--overflow") == 0) {
        if (!native::install_handler())
            return 90;
        pthread_t worker;
        if (pthread_create(&worker, nullptr, worker_overflow, nullptr))
            return 91;
        pthread_join(worker, nullptr);
        return 92;
    }
    if (std::strcmp(argv[1], "--capture") == 0) {
        auto trace = posix_capture_site();
        if (!trace.count || trace.count > 8 || native::capture(0, 0).count || native::capture(1025, 8).count)
            return 1;
        native::write_trace(trace);
        return 0;
    }
    if (argc < 3)
        return 2;
    native::frame_info frame;
    if (!native::resolve(reinterpret_cast<std::uintptr_t>(&posix_capture_site), frame))
        return 3;
    char output[4096];
    std::size_t written = 0;
    bool const limited = std::strcmp(argv[1], "--limit") == 0;
    auto status = native::symbolize_group(argv[2], frame.module, &frame.offset, 1, limited,
                                          output, limited ? 128 : sizeof(output), written, 300);
    std::printf("status=%u bytes=%zu\n", static_cast<unsigned>(status), written);
    std::fwrite(output, 1, written, stdout);
    return 0;
}
