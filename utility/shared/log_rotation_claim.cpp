/**
 * @file utility/shared/log_rotation_claim.cpp
 * @brief The process-wide log-rotation claim (see log_rotation_claim.hpp for why it exists).
 * @ingroup utility
 */

#include "log_rotation_claim.hpp"

#include <cstdint>

#if defined(_WIN32)

// THE `#ifndef` IS LOAD-BEARING, not decoration: libstdc++'s `c++config.h` already defines NOMINMAX
// when it pulls in a Windows header - so an unguarded `#define` here is a REDEFINITION, which the
// project's `-Werror` turns into a build failure (measured with GCC on MinGW; libc++ does not).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <cwchar>
#include <windows.h>

extern "C" int32_t utility_shared_claim_log_rotation() {
    // One named mutex per PROCESS, created on first use and NEVER released: the name exists while
    // any handle to it is open, which is what lets a second copy of this code see
    // ERROR_ALREADY_EXISTS. Releasing it would let a later-arriving copy claim the name again - i.e.
    // rotate a second time - which is the one thing this function is here to prevent.
    static int32_t const claimed = [] {
        // `Local\` is per SESSION; the process id is what makes it per process (two runs of the
        // executable must each rotate their own debug.log; two images of one process must not).
        wchar_t name[64] = {};
        int32_t const written = std::swprintf(name, 64, L"Local\\deren.debug.log.rotation.%lu",
                                              static_cast<unsigned long>(GetCurrentProcessId()));
        if (written <= 0) {
            return int32_t{1}; // cannot name the object: fall back to the single-image behaviour
        }
        HANDLE const handle = CreateMutexW(nullptr, TRUE, name);
        if (handle == nullptr) {
            return int32_t{1}; // cannot arbitrate: rotate, exactly as a single-copy process does
        }
        return (GetLastError() == ERROR_ALREADY_EXISTS) ? int32_t{0} : int32_t{1};
    }();
    return claimed;
}

#else

#include <atomic>

// POSIX (written, NOT measured - CI is windows-latest for both jobs, like dynamic_link's POSIX
// branch): there is no leak-free OS-named object that is per PROCESS rather than per session or per
// machine here, so the claim degrades to per-IMAGE arbitration and the cross-image half is
// deliberately not pretended. Recorded rather than hidden: if a POSIX `shared_utility` flip ever
// lands, the claim needs a real process-wide object there too.
extern "C" int32_t utility_shared_claim_log_rotation() {
    static std::atomic<bool> taken = false;
    return taken.exchange(true) ? int32_t{0} : int32_t{1};
}

#endif
