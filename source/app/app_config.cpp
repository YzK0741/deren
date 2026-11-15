/**
 * @file source/app/app_config.cpp
 * @brief main()'s startup helpers' bodies: the config-file + argv merge, shader SPIR-V loading and
 *        the source/shaders/ and model-file lookups (see app_config.cppm for why they live here).
 */
module;

// NO GLOBAL-MODULE-FRAGMENT INCLUDE, deliberately: every type this unit names comes from the
// imported modules below (deren.vstd re-exports the STL), and the vendored std module plus
// deren.utility already give the allocator's instantiations here - measured with -fsyntax-only
// on this toolchain, so the ambiguous-operator-new trap that still requires a textual <glm/glm.hpp>
// in source/app/app_pipelines.cpp does not apply to this unit. Do not add one back "for symmetry".

module deren.app.config;

import deren.vstd;
import deren.utility;
import deren.application_configuration;
import deren.asset_library;

namespace deren::app {
    namespace {
        void require_existing_path(std::string_view const path, std::string_view label, bool const directory) {
            std::filesystem::path const fs_path(path);
            std::error_code ec;
            bool const exists = directory ? std::filesystem::is_directory(fs_path, ec) : std::filesystem::is_regular_file(fs_path, ec);
            if (exists) {
                return;
            }
            std::error_code cwd_error;
            std::string const cwd = std::filesystem::current_path(cwd_error).string();
            deren::utility::panic(std::source_location::current(),
                                  "{} '{}' does not exist (working directory '{}'). Check the config file or pass a valid path.",
                                  label, path, cwd);
        }

        void require_existing_directory(std::string_view const path, std::string_view label) {
            if (!path.empty()) {
                require_existing_path(path, label, true);
            }
        }
    } // namespace

    startup_config analyse_config(deren::app_config::app_settings const& settings) {
        require_existing_directory(settings.paths.asset_library, "[paths] asset_library");
        if (!settings.config_file.empty()) {
            deren::utility::log("app_config: loaded startup settings from '{}'", settings.config_file);
        } else {
            std::error_code cwd_error;
            std::string const cwd = std::filesystem::current_path(cwd_error).string();
            deren::utility::log("app_config: no config.toml found in '{}'; using defaults.", cwd);
        }

        startup_config config = {settings, {}};
        // The shaders directory (holds the compiled SPIR-V): explicit config path when given,
        // otherwise walk up from the working directory to find source/shaders/.
        if (!settings.paths.shaders_dir.empty()) {
            config.shaders_dir = settings.paths.shaders_dir;
            if (!std::filesystem::is_directory(config.shaders_dir)) {
                std::error_code cwd_error;
                std::string const cwd = std::filesystem::current_path(cwd_error).string();
                deren::utility::panic(std::source_location::current(),
                                      "cannot find configured shaders_dir '{}' (working directory '{}'). "
                                      "Fix [paths] shaders_dir in the config file; after renaming or moving the build "
                                      "directory, regenerate it with 'python scripts/config/make_config.py <build-dir>'.",
                                      settings.paths.shaders_dir, cwd);
            }
        } else if (std::optional<std::filesystem::path> const located = locate_shaders_dir()) {
            config.shaders_dir = *located;
        } else {
            std::error_code cwd_error;
            std::string const cwd = std::filesystem::current_path(cwd_error).string();
            deren::utility::panic(std::source_location::current(),
                                  "cannot find a shaders/ directory from '{}'. Run from the project root, set "
                                  "[paths] shaders_dir, or pass --config <build-dir>/config.toml; regenerate the build "
                                  "config with 'python scripts/config/make_config.py <build-dir>'.",
                                  cwd);
        }
        return config;
    }

    // Read a single shader SPIR-V file and print info; panic on failure
    void load_shader(std::filesystem::path const& dir, std::string_view const file_name, deren::utility::vessel::vector<std::uint8_t>& out) {
        std::filesystem::path const path = dir / file_name;
        constexpr std::uintmax_t max_shader_bytes = 64ull * 1024 * 1024;
        auto data = deren::utility::read_binary_to_vector(path, max_shader_bytes);
        if (!data) {
            deren::utility::panic(std::source_location::current(), "cannot read shader file '{}' ({}; cause {}: {})", path.string(),
                                  deren::utility::file_error_name(data.error().code), data.error().cause.category().name(),
                                  data.error().cause.value());
        }
        out = std::move(*data);
        deren::utility::log("loaded shader: {} ({} bytes)", path.string(), out.size());
    }

    // Walk up from the working directory to find the source/shaders/ directory,
    // so it works when run from the project root or a cmake-build-* directory
    std::optional<std::filesystem::path> locate_shaders_dir() {
        // The build's output first: a shaders/ directory beside the executable, written by the
        // build's shader step. Checking this before the cwd walk is what makes a build-tree run load
        // the SPIR-V its build just compiled instead of resolving `source/shaders/` upward into the source
        // tree - the stale-binary trap the build no longer commits a fallback for.
        std::filesystem::path const exe_dir = deren::utility::executable_directory();
        if (!exe_dir.empty()) {
            std::filesystem::path const candidate = exe_dir / "shaders";
            if (std::filesystem::is_regular_file(candidate / "pbr.mesh.spv")) {
                return candidate;
            }
        }
        std::filesystem::path current = std::filesystem::current_path();
        for (std::int32_t depth = 0; depth < 4; ++depth) {
            std::filesystem::path candidate = current / "shaders";
            // The SAME probe as the executable-relative check above, deliberately: a directory named
            // `shaders` that holds no compiled SPIR-V (the source tree's, since the build writes
            // the .spv into the build tree) is not the answer this function is looking for - returning
            // it only moves the failure into a much less clear "cannot open shader file" panic.
            if (std::filesystem::is_regular_file(candidate / "pbr.mesh.spv")) {
                return candidate;
            }
            std::filesystem::path const parent = current.parent_path();
            if (parent == current) {
                break;
            }
            current = parent;
        }
        return std::nullopt;
    }

} // namespace deren::app
