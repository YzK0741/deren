/**
 * @file source/app/main.cpp
 * @brief the executable's entry point
 *
 * The entry point is deliberately thin. Crash handling, argument parsing, window/runtime startup, the
 * frame loop and teardown all live behind deren::app::run() in app_application.cpp; the toon material
 * lookup lives in deren.app.toon. This mirrors the split the reference engine (and UE) uses: the
 * platform entry only forwards, the application module owns the lifecycle.
 */

#include "app_application.hpp"

int main(int argc, char** argv) {
    return deren::app::run(argc, argv);
}
