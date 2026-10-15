// Test-only: a DLL whose DllMain BLOCKS, so the thread inside LoadLibrary keeps the loader lock
// for as long as the test wants. A crash on another thread then finds the lock held - the one
// context the crash filter must survive, because a fault really can arrive while a plugin, the
// Vulkan loader or a driver's DLL is being initialized (mimalloc's TLS callbacks and our own
// DLLs' static initialization run in that same window).
//
// The two events are looked up BY NAME: DllMain runs INSIDE LoadLibrary, so the loading thread
// cannot call an exported setter first. The owner (a test child process) creates both events with
// the same name and never signals the release one.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {
    constexpr wchar_t entered_prefix[] = L"deren-loader-fixture-entered-";
    constexpr wchar_t release_prefix[] = L"deren-loader-fixture-release-";
    constexpr wchar_t fault_prefix[] = L"deren-loader-fixture-fault-";

    /// `<prefix><pid>`, built without the CRT (this runs before our own static initialization, and
    /// a test fixture has no business calling into a runtime it is proving the filter does not need).
    void event_name(wchar_t const* prefix, wchar_t (&name)[128]) noexcept {
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

    HANDLE open_event(wchar_t const* prefix) noexcept {
        wchar_t name[128];
        event_name(prefix, name);
        return OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name);
    }
} // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }
    // Trigger one: fault HERE. This thread is inside LoadLibrary, so it holds the loader lock while
    // the unhandled-exception filter runs - the "same thread holds it" half of the question.
    HANDLE const fault = open_event(fault_prefix);
    if (fault != nullptr) {
        CloseHandle(fault);
        auto* nowhere = static_cast<unsigned volatile*>(nullptr);
        *nowhere = 1;
    }
    HANDLE const entered = open_event(entered_prefix);
    if (entered != nullptr) {
        SetEvent(entered);
        CloseHandle(entered);
    }
    HANDLE const release = open_event(release_prefix);
    if (release != nullptr) {
        // Deliberately pathological, and deliberately test-only: THE LOADER LOCK IS HELD HERE
        // until the owner terminates the process. (A real plugin never does this; what this
        // simulates is a holder that is stuck, which is one of the two ways a crash filter can
        // meet a held lock.)
        static_cast<void>(WaitForSingleObject(release, INFINITE));
        CloseHandle(release);
    }
    return TRUE;
}
