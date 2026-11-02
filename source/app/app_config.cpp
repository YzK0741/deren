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
    // Resolve the startup config in one step: merge config file + argv into the app settings,
    // then locate the source/shaders/ dir and pick the model file. Panics when a resource is missing.
    startup_config analyse_config(std::int32_t argc, char** argv) {
        // 1. Resolve startup settings first: config file (config.toml by default, --config <path>
        //    to override) merged with positional argv overrides. argv[1] = model, argv[2] = grid
        //    side (numeric).
        deren::app_config::app_settings const settings = deren::app_config::resolve_from_argv(argc, argv);
        if (!settings.config_file.empty()) {
            deren::utility::log("app_config: loaded startup settings from '{}'", settings.config_file);
        }

        startup_config config = {settings, {}, {}, {}};
        // The library is scanned at most once: the default model and every per-object `asset` id
        // resolve through the SAME catalogue, and the warnings are reported once.
        std::unique_ptr<deren::asset_library::asset_library> library;
        auto const ensure_library = [&]() -> deren::asset_library::asset_library const& {
            if (library != nullptr) {
                return *library;
            }
            if (settings.paths.asset_library.empty()) {
                deren::utility::panic(std::source_location::current(),
                                      "an asset reference needs [paths] asset_library (pass --asset-library <dir>).");
            }
            library = std::make_unique<deren::asset_library::asset_library>(
                deren::asset_library::asset_library::scan(settings.paths.asset_library));
            if (!library->valid()) {
                deren::utility::panic(std::source_location::current(), "cannot use asset library '{}': {}",
                                      settings.paths.asset_library, library->error());
            }
            for (std::string const& warning : library->warnings()) {
                deren::utility::log("asset library: {}", warning);
            }
            return *library;
        };

        // 2. Shaders directory (holds GLSL sources and compiled SPIR-V): explicit config path when
        //    given, otherwise walk up from the working directory to find source/shaders/.
        if (!settings.paths.shaders_dir.empty()) {
            config.shaders_dir = settings.paths.shaders_dir;
            if (!std::filesystem::is_directory(config.shaders_dir)) {
                deren::utility::panic(std::source_location::current(), "cannot find configured shaders_dir '{}'.", settings.paths.shaders_dir);
            }
        } else if (std::optional<std::filesystem::path> const located = locate_shaders_dir()) {
            config.shaders_dir = *located;
        } else {
            deren::utility::panic("cannot find shaders/ directory. run the program from the project root, pass shaders_dir in config.toml, or use a cmake-build-* directory.");
        }

        // A scene whose placements all name their own asset does not need a separate default model: the
        // first placement becomes the default load below. Keep the historic default lookup only when at
        // least one placement inherits the top-level model (or there is no object list at all).
        bool every_object_owns_model = !settings.scene.objects.empty();
        for (deren::app_config::scene_object_settings const& object : settings.scene.objects) {
            if (object.model.empty() && object.asset.empty()) {
                every_object_owns_model = false;
                break;
            }
        }

        // 3. Pick the model file: `model = "ask"` opens the platform's own file dialog first (the config's
        //    way of saying "let me choose at startup"); otherwise settings.model when configured/argv-given;
        //    else the default model under settings.paths.model_dir (or the auto-located asset/).
        //    WHICH LOADER READS IT IS THE FILE'S OWN EXTENSION (see app_config::model_format_of), so the dialog
        //    offers both families and neither the title nor the filter promises glTF alone.
        if (deren::app_config::wants_model_dialog(settings)) {
            // A CANCELLED DIALOG IS NOT AN ERROR, and neither is a build with no way to ask: both fall
            // through to the same default chain an empty model takes, so the app still starts with
            // something on screen. The two are logged apart inside ask_open_file, because "I said no" and
            // "nobody could ask me" are different answers and only one of them is the user's decision.
            if (std::optional<std::filesystem::path> const chosen = deren::utility::ask_open_file("choose a model to render (.gltf/.glb scene, .ply gaussian splat cloud)", "*.glb;*.gltf;*.ply")) {
                config.model_path = chosen->string();
            } else {
                deren::utility::log("model: 'ask' produced no file, using the default model");
            }
        } else if (!settings.model.empty()) {
            config.model_path = settings.model;
        }
        // 3b. Asset library: only consulted when no explicit model won above, so the documented
        //     precedence holds (--model / configured model > --asset > configured asset > library
        //     default > the old model_dir/DamagedHelmet fallback). A library is never a silent
        //     opt-in for the experimental toon path: the manifest's [toon] game_toon flag is
        //     metadata here, not a render-mode switch.
        if (config.model_path.empty() && (!settings.asset.empty() || (!every_object_owns_model && !settings.paths.asset_library.empty()))) {
            deren::asset_library::asset_library const& assets = ensure_library();
            deren::asset_library::asset_entry const* selected = nullptr;
            if (!settings.asset.empty()) {
                selected = assets.find(settings.asset);
                if (selected == nullptr) {
                    deren::utility::panic(std::source_location::current(),
                                          "asset '{}' is not in the library at '{}'.", settings.asset,
                                          settings.paths.asset_library);
                }
            } else {
                selected = assets.default_asset();
                if (selected == nullptr) {
                    deren::utility::panic(std::source_location::current(), "asset library '{}' has no default asset.",
                                          settings.paths.asset_library);
                }
            }
            config.model_path = selected->model.string();
            deren::utility::log("asset library: selected '{}' ({})", selected->id, config.model_path);
        }
        if (config.model_path.empty() && !every_object_owns_model) {
            if (!settings.paths.model_dir.empty()) {
                config.model_path = (std::filesystem::path(settings.paths.model_dir) / "DamagedHelmet" / "DamagedHelmet.gltf").string();
                if (!std::filesystem::is_regular_file(config.model_path)) {
                    deren::utility::panic(std::source_location::current(), "cannot find model '{}' under configured model_dir '{}'.", "DamagedHelmet/DamagedHelmet.gltf", settings.paths.model_dir);
                }
            } else if (std::optional<std::filesystem::path> const located = locate_model_file()) {
                config.model_path = located->string();
            } else {
                deren::utility::panic("cannot find asset/DamagedHelmet/DamagedHelmet.gltf. run the program from the project root or pass a model path as argv[1]");
            }
        }

        // 4. Resolve every per-object asset reference. `model` and `asset` are mutually exclusive;
        //    an entry with neither reuses the default model above. Paths are kept in the config's own
        //    spelling (the loader resolves relative paths from the working directory), while the
        //    DISTINCT cap compares normalized absolute paths so two spellings of one file share a load.
        std::vector<std::string> distinct_models;
        for (std::size_t index = 0; index < settings.scene.objects.size(); ++index) {
            deren::app_config::scene_object_settings const& object = settings.scene.objects[index];
            if (!object.model.empty() && !object.asset.empty()) {
                deren::utility::panic(std::source_location::current(),
                                      "[[scene.objects]] entry {} '{}' sets both model and asset; name one.",
                                      index, object.name);
            }
            deren::utility::vessel::string model;
            if (!object.model.empty()) {
                model = object.model;
            } else if (!object.asset.empty()) {
                deren::asset_library::asset_entry const* const selected = ensure_library().find(object.asset);
                if (selected == nullptr) {
                    deren::utility::panic(std::source_location::current(),
                                          "[[scene.objects]] entry {} '{}' names asset '{}', which is not in the library at '{}'.",
                                          index, object.name, object.asset, settings.paths.asset_library);
                }
                model = selected->model.string();
            } else {
                model = config.model_path;
            }
            deren::app_config::model_format const format = deren::app_config::model_format_of(model);
            if (format == deren::app_config::model_format::unsupported) {
                deren::utility::panic(std::source_location::current(),
                                      "[[scene.objects]] entry {} '{}' names a model with no extension this build reads: name a .gltf/.glb scene or a .ply gaussian splat cloud.",
                                      index, object.name);
            }
            // THE PIPELINE AND THE LOADER MUST AGREE. A Gaussian cloud has no meshes, so a mesh
            // pipeline cannot draw it; conversely a .glb has no splats for the gaussian pipeline.
            bool const object_is_gaussian = format == deren::app_config::model_format::gaussian_ply;
            if (object_is_gaussian != (object.pipeline == deren::app_config::scene_object_pipeline_gaussian)) {
                deren::utility::panic(std::source_location::current(),
                                      "[[scene.objects]] entry {} '{}': a .ply cloud requires pipeline = \"gaussian\" and a .gltf/.glb requires any mesh pipeline",
                                      index, object.name);
            }
            std::filesystem::path const absolute = std::filesystem::absolute(model).lexically_normal();
            std::string const key = absolute.string();
            if (std::find(distinct_models.begin(), distinct_models.end(), key) == distinct_models.end()) {
                if (distinct_models.size() >= deren::app_config::max_scene_assets) {
                    deren::utility::panic(std::source_location::current(),
                                          "[[scene.objects]] needs more than {} distinct assets; share an asset between placements or use fewer model files.",
                                          deren::app_config::max_scene_assets);
                }
                distinct_models.push_back(key);
            }
            config.object_model_paths.push_back(std::move(model));
        }
        if (!config.object_model_paths.empty()) {
            // A scene whose placements all name their own asset needs no separate top-level model:
            // the first placement's asset becomes the default load, and slot dedupe points object 0
            // at that same load.
            if (config.model_path.empty()) {
                config.model_path = config.object_model_paths.front();
            }
            deren::utility::log("scene: {} object asset reference(s), {} distinct model file(s)", config.object_model_paths.size(), distinct_models.size());
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
        // The build's own output first: a shaders/ directory beside the executable, written by the
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
            // `shaders` that holds no compiled SPIR-V (the source tree's own, since the build writes
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

    // Walk up from the working directory to find the default model under asset/
    std::optional<std::filesystem::path> locate_model_file() {
        std::filesystem::path current = std::filesystem::current_path();
        for (std::int32_t depth = 0; depth < 4; ++depth) {
            std::filesystem::path candidate = current / "asset" / "DamagedHelmet" / "DamagedHelmet.gltf";
            if (std::filesystem::is_regular_file(candidate)) {
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
