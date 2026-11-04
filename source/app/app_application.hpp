#pragma once

/**
 * @file source/app/app_application.hpp
 * @brief the application's lifecycle entry, called by main.cpp after the platform entry
 */

namespace deren::app {

    /// Run the application: parse arguments, start the runtime, run the frame loop and tear down.
    /// Returns the process exit code (0 on success, 2 for invalid arguments, 3 for a failed capture).
    [[nodiscard]] int run(int argc, char** argv);

} // namespace deren::app
