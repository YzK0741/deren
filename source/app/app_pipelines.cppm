module;

export module deren.app.pipelines;
export import deren.vstd;
import deren.engine.runtime;

/**
 * @file source/app/app_pipelines.cppm
 * @defgroup app_pipelines Application Pipeline Setup
 * @brief the demo pipelines' creation, kept out of main.cpp so the entry point reads first: the
 *        named forward pipelines every pass and job draws with, and the shader modules those
 *        passes build their pipelines from. Demo/application layer only - this module knows
 *        about deren.engine.runtime and the app's shader files (loaded through
 *        deren.app.config), never about glTF (the loader's diagnostics live in gltf_loader).
 * @note interface only - the implementations live in app_pipelines.cpp (the module follows the
 *       export import std pattern of the other split modules)
 */
export namespace deren::app {
    /**
     * @ingroup app_pipelines
     * @brief load a fragment/mesh SPIR-V pair for a named pipeline and create it via the runtime;
     *        panic on load or creation failure. THE VERTEX FILE IS GONE (docs/mesh_shaders.md step
     *        4): a named geometry pipeline is built from a MESH stage and the meshlet entry beside
     *        it, and the runtime refuses a name with no mesh module rather than falling back.
     * @param meshlet_file the meshlet entry beside @p mesh_file; an empty name creates the
     *        fragment + mesh form only
     */
    void load_and_create_pipeline(deren::engine::runtime& runtime,
                                  std::filesystem::path const& shaders_dir,
                                  std::string_view const pipeline_name,
                                  std::string_view const fragment_file,
                                  std::string_view const mesh_file,
                                  std::string_view const meshlet_file = {});

    /**
     * @ingroup app_pipelines
     * @brief create the demo pipelines up front: the standard PBR pipeline plus the skybox
     *        background (fullscreen environment pass) and the directional shadow-map pass
     *        (depth-only). Panics when PBR/skybox creation fails; a shadow failure only logs
     *        "shadow pipeline disabled" (the scene still renders without shadows).
     * @note the old triangle demo pipeline is deliberately NOT created here: nothing draws it
     *       anymore (the skybox background and the imported scene cover the screen)
     */
    void setup_pipeline(deren::engine::runtime& runtime, std::filesystem::path const& shaders_dir);
} // namespace deren::app
