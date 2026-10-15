#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
int main(int argc, char** argv) {
    // Test-only modes, selected through the CHILD's environment. The launcher must pass every
    // variable except the ones that rewrite the symbolizer's output protocol, so a mode that
    // arrives proves both that ordinary variables survive the filter and what a clean exit with
    // no output at all looks like (nothing decoded must not be reported as `ok`).
    if (char const* mode = std::getenv("STACKTRACE_FIXTURE_MODE")) {
        if (std::strcmp(mode, "quiet") == 0)
            return 0;
        // What THIS child actually received: the launcher must drop LLVM_SYMBOLIZER_OPTS while
        // ordinary variables (the one selecting this mode) survive.
        if (std::strcmp(mode, "env-probe") == 0) {
            char const* option = std::getenv("LLVM_SYMBOLIZER_OPTS");
            std::printf("mode=env-probe llvm_symbolizer_opts=%s\n", option ? option : "(unset)");
            return 0;
        }
    }
    bool inlines = false;
    for (int i = 1; i < argc; ++i)
        inlines = inlines || std::strcmp(argv[i], "--inlines") == 0;
    if (inlines) {
        char output[1024];
        std::memset(output, 'x', sizeof(output));
        for (int i = 0; i < 128; ++i)
            static_cast<void>(std::fwrite(output, 1, sizeof(output), stdout));
    } else {
#if defined(_WIN32)
        Sleep(10000);
#endif
    }
}
