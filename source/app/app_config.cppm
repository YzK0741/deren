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
     * @brief the resolved startup environment: merged settings + located shaders dir
     * @note everything main needs before it touches the runtime; produced by analyse_config()
     */
    struct startup_config {
        deren::app_config::app_settings settings = {}; // merged config-file + argv settings
        std::filesystem::path shaders_dir = {};        // shader SPIR-V directory (configured or located)
    };

    /**
     * @ingroup app_startup
     * @brief locate the shaders directory from the resolved project settings (configured path or
     *        auto-located); panics when it is missing. The C# game owns every scene object, so no
     *        native model is selected, validated, or required here.
     */
    startup_config analyse_config(deren::app_config::app_settings const& settings);

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
} // namespace deren::app
