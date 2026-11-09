module;

export module deren.app.config;
export import deren.utility.shared;
export import deren.vstd;
import deren.application_configuration;

/**
 * @file source/app/app_config.cppm
 * @defgroup app_startup Application Startup Helpers
 * @brief main()'s startup configuration helpers, kept out of main.cpp so the entry point reads
 *        first: resolving the startup config (config file + argv merge, source/shaders/ and
 *        default-model dirs) and loading shader SPIR-V files. Demo/application layer only - this
 *        module knows about deren::app_config (settings) and the utility log/panic, never about
 *        glTF (the loader's diagnostics live in gltf_loader).
 * @note interface only - the implementations live in app_config.cpp (the module follows the
 *       export import std pattern of the other split modules)
 */
export namespace deren::app {
    /**
     * @ingroup app_startup
     * @brief the resolved startup environment: merged settings + located shaders dir + model path
     * @note everything main needs before it touches the runtime; produced by analyse_config()
     */
    struct startup_config {
        deren::app_config::app_settings settings = {};  // merged config-file + argv settings
        std::filesystem::path shaders_dir = {};         // shader SPIR-V directory (configured or located)
        deren::utility::vessel::string model_path = {}; // default model (configured, model_dir, or located)
        // One resolved model path per `[[scene.objects]]` entry, in the same order. Empty when the
        // scene list is empty. Entries that name no model/asset inherit `model_path`.
        std::vector<deren::utility::vessel::string> object_model_paths = {};
    };

    /**
     * @ingroup app_startup
     * @brief resolve the startup config in one step: merge the config file + argv into the
     *        app settings, then locate the source/shaders/ directory and pick the model file
     *        (configured path, model_dir default, or auto-located gltf_model/). Panics when a
     *        configured or located resource is missing.
     * @param argc argv argument count (as received by main)
     * @param argv argv argument vector (as received by main)
     */
    startup_config analyse_config(std::int32_t argc, char** argv);

    /**
     * @ingroup app_startup
     * @brief read one shader SPIR-V file into @p out; panic on failure (prints the path)
     */
    void load_shader(std::filesystem::path const& dir, std::string_view const file_name, deren::utility::vessel::vector<std::uint8_t>& out);

    /**
     * @ingroup app_startup
     * @brief walk up from the working directory to find the source/shaders/ directory (works from the
     *        project root or a cmake-build-* directory); nullopt when not found within 4 levels
     */
    std::optional<std::filesystem::path> locate_shaders_dir();

    /**
     * @ingroup app_startup
     * @brief walk up from the working directory to find the default model under gltf_model/
     *        (gltf_model/DamagedHelmet.gltf); nullopt when not found within 4 levels
     */
    std::optional<std::filesystem::path> locate_model_file();
} // namespace deren::app
