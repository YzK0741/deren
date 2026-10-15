#include <cstdint>
#include <cstdlib>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#if defined(_MSC_VER)
#define FAULT_NOINLINE __declspec(noinline)
#else
#define FAULT_NOINLINE __attribute__((noinline, no_sanitize("address", "undefined")))
#endif
extern "C" FAULT_NOINLINE void trace_fault() {
#if defined(_WIN32)
    auto* page = static_cast<unsigned char volatile*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
    if (!page)
        std::abort();
    *page = 42;
#else
    std::abort();
#endif
}
static FAULT_NOINLINE void exhaust_stack(unsigned volatile* condition) {
    unsigned char volatile pad[4096];
    pad[0] = static_cast<unsigned char>(*condition);
    if (*condition)
        exhaust_stack(condition);
    pad[4095] = pad[0];
}
extern "C" void trace_stack_overflow() {
    unsigned volatile condition = 1;
    exhaust_stack(&condition);
}
extern "C" void trace_detach_stderr() {
#if defined(_WIN32)
    SetStdHandle(STD_ERROR_HANDLE, nullptr);
#endif
}
