/**
 * @file source/app/main.cpp
 * @brief the executable's entry point
 *
 * The entry point is deliberately thin. Crash handling, argument parsing, window/runtime startup, the
 * frame loop and teardown all live behind deren::app::run() in the deren.app.application module. The
 * scene itself is owned by the managed C# host, which the application starts and ticks. This mirrors the
 * split the reference engine (and UE) uses: the platform entry only forwards, the application module runs
 * the lifecycle.
 */

#include "../audio/module_entry.hpp"

import deren.app.application;

int main(int argc, char** argv) {
    deren_audio_ensure_linked();
    return deren::app::run(argc, argv);
}
