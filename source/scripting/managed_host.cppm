export module deren.scripting.managed_host;
export import deren.vstd;

export namespace deren::scripting {

    inline constexpr std::uint32_t managed_abi_version = 2;

    struct entity_handle {
        std::uint64_t scene;
        std::uint32_t index;
        std::uint32_t generation;
    };

    struct vector_3 {
        float x, y, z;
    };

    struct transform_value {
        vector_3 position;
        vector_3 rotation_degrees;
        vector_3 scale;
    };

    struct utf8_view {
        char const* data;
        std::uint32_t length;
    };

    struct script_binding {
        entity_handle entity;
        utf8_view type;
    };

    enum class script_status : std::int32_t {
        success = 0,
        invalid_argument = 1,
        invalid_entity = 2,
        scene_exists = 3,
        scene_not_found = 4,
        script_failure = 5
    };

    // Keep layouts and calling conventions in sync with managed_bootstrap.cs.
    struct native_api {
        std::uint32_t size = sizeof(native_api);
        std::uint32_t version = managed_abi_version;
        void* context = nullptr;
        void(__cdecl* log)(void* context, char const* utf8, std::uint32_t length) = nullptr;
        script_status(__cdecl* get_transform)(void* context, entity_handle entity, transform_value* value) = nullptr;
        script_status(__cdecl* set_transform)(void* context, entity_handle entity, transform_value const* value) = nullptr;
    };

    struct managed_api {
        std::uint32_t size = sizeof(managed_api);
        std::uint32_t version = managed_abi_version;
        // Call on the scene thread. Use a fresh nonzero scene ID for each scene
        // lifetime. The callbacks are copied, and their context must outlive stop_scene.
        script_status(__cdecl* start_scene)(native_api const*, std::uint64_t scene, utf8_view assembly,
                                            script_binding const* bindings, std::uint32_t count) = nullptr;
        script_status(__cdecl* update_scene)(std::uint64_t scene, float delta_time) = nullptr;
        script_status(__cdecl* stop_scene)(std::uint64_t scene) = nullptr;
    };

    enum class host_status : std::int32_t {
        success = 0,
        abi_mismatch = 1,
        invalid_api = 2,
        assembly_missing,
        runtime_config_missing,
        nethost_load_failed,
        runtime_discovery_failed,
        hostfxr_load_failed,
        runtime_initialization_failed,
        delegate_lookup_failed,
        bridge_load_failed,
        bridge_initialization_failed
    };

    struct host_paths {
        std::filesystem::path assembly;
        std::filesystem::path runtime_config;
        std::filesystem::path nethost;
        std::filesystem::path dotnet_root{};
    };

    struct host_result {
        host_status status;
        std::int32_t detail = 0;
    };

    [[nodiscard]] host_result initialize_managed_host(host_paths const& paths, native_api const& api, managed_api& scripts);

} // namespace deren::scripting
