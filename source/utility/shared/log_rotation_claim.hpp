#pragma once
/**
 * @file source/utility/shared/log_rotation_claim.hpp
 * @brief The one declaration of the process-wide log-rotation claim (shared_utility).
 * @ingroup utility
 *
 * WHY THIS IS A PLAIN (NON-MODULE) TU, exactly like source/utility/platform_*.cpp: the Windows half needs
 * <windows.h>, and a module's global module fragment that includes it makes libc++'s operator new
 * ambiguous in every TU importing that module (see platform_functions.hpp for the measured note).
 * The module side therefore only sees the C entry point declared here, and the definition includes
 * this same header so a signature change cannot drift.
 *
 * WHY THE CLAIM EXISTS AT ALL. `rotate_previous_log()` reads debug.log, moves it aside to
 * debug.log.old, and then TRUNCATES debug.log. If two copies of the sink's code ever live in one
 * process, the second rotation truncates the file the first copy still has open and is appending
 * to: interleaved lines, holes, and two "sessions" in one debug.log.old. Today that cannot happen
 * because the toolkit is STATIC and the linker keeps one copy - but the day `deren_vulkan` becomes
 * SHARED, the DLL carries its own copy unless `shared_utility` is the shared image both halves
 * import, and THAT is precisely the invariant this claim watches.
 *
 * A function-local `static` cannot see the other copy of the code (each image has its own), so the
 * claim is an OS-named object keyed by the PROCESS ID. `Local\` alone would be per SESSION, which
 * is the wrong scope in both directions: two runs of the executable must each rotate their own
 * log, while two images of one process must not.
 *
 * @return 1 when THIS image is the first in this process to claim the rotation (the caller
 *         rotates), 0 when another copy already claimed it (the caller skips the rotation).
 *         Failing to arbitrate at all answers 1, which is exactly the single-image behaviour.
 */

#include <cstdint>

extern "C" int32_t utility_shared_claim_log_rotation();
