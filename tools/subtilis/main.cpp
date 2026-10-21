#include "commands.hpp"

#include <cstdio>
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::puts("deren-subtilis build INPUT -o OUTPUT.glb [--threads 1] [--report JSON] [--strict] [--overwrite]\n               inspect INPUT\n               validate INPUT [--full]\n               export-cut INPUT --error MODEL_ERROR -o OUTPUT.glb [--overwrite]");
        return 0;
    }
    deren::tools::subtilis::vector<std::string_view> arguments;
    for (int i = 1; i < argc; ++i)
        arguments.push_back(argv[i]);
    auto parsed = deren::tools::subtilis::parse_arguments(arguments);
    if (!parsed) {
        std::fprintf(stderr, "deren-subtilis: %s\n", parsed.error().context.c_str());
        return 2;
    }
    return deren::tools::subtilis::run(*parsed);
}
