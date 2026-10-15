#include <cstddef>
import deren.utility.shared;
#if defined(_WIN32)
#define VESSEL_PROBE_EXPORT __declspec(dllexport)
#else
#define VESSEL_PROBE_EXPORT __attribute__((visibility("default")))
#endif
extern "C" VESSEL_PROBE_EXPORT void* vessel_probe_allocate(std::size_t bytes, std::size_t alignment) noexcept {
    auto result = deren::utility::memory::try_allocate(bytes, alignment);
    return result ? *result : nullptr;
}
extern "C" VESSEL_PROBE_EXPORT void vessel_probe_free(void* pointer) noexcept {
    deren::utility::memory::deallocate(pointer);
}
#if defined(_MSC_VER)
#define PROBE_NOINLINE __declspec(noinline)
#else
#define PROBE_NOINLINE __attribute__((noinline))
#endif
extern "C" VESSEL_PROBE_EXPORT PROBE_NOINLINE void vessel_probe_trace(void (*callback)(void*), void* state) {
    callback(state);
    void* volatile retained = state;
    static_cast<void>(retained);
}
