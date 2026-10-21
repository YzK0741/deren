#pragma once
#include "document.hpp"

#include <variant>
import deren.asset.subtilis.builder;
namespace deren::tools::subtilis {
    struct build_command {
        path input, output;
        std::optional<path> report;
        asset::build_options options;
        asset::execution_options execution;
        bool strict = false;
    };
    struct inspect_command {
        path input;
    };
    struct validate_command {
        path input;
        bool full = false;
    };
    struct export_cut_command {
        path input, output;
        float error = 0;
    };
    using command = std::variant<build_command, inspect_command, validate_command, export_cut_command>;
    result<command> parse_arguments(std::span<std::string_view const>);
    int run(command const&);
} // namespace deren::tools::subtilis
