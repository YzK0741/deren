#pragma once
#include "document.hpp"

#include <variant>
import deren.asset.subtilis.builder;
namespace deren::tools::subtilis {
    struct conversion_options {
        asset::build_options build;
        asset::execution_options execution;
        write_options write;
        bool strict = false;
    };
    struct conversion_report {
        uint64_t converted = 0, reused = 0, skipped = 0, lod0_only = 0;
        string json;
    };
    result<conversion_report> convert_file(path const& input, path const& output, conversion_options const&);
    struct build_command {
        path input, output;
        std::optional<path> report;
        asset::build_options options;
        asset::execution_options execution;
        output_format format = output_format::glb;
        bool strict = false;
        bool overwrite = false;
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
        output_format format = output_format::glb;
        bool overwrite = false;
    };
    using command = std::variant<build_command, inspect_command, validate_command, export_cut_command>;
    result<command> parse_arguments(std::span<std::string_view const>);
    int run(command const&);
} // namespace deren::tools::subtilis
