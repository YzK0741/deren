export module deren.scripting.managed_host;
export import deren.vstd;

export namespace deren::scripting {

    inline constexpr std::uint32_t managed_abi_version = 16;

    struct node_handle {
        std::uint64_t value = 0;
    };

    struct asset_handle {
        std::uint64_t value = 0;
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

    enum class script_status : std::int32_t {
        success = 0,
        invalid_argument = 1,
        invalid_node = 2,
        scene_exists = 3,
        scene_not_found = 4,
        script_failure = 5,
        spawn_unavailable = 6,
        module_unavailable = 7,
        module_invalid = 8,
        asset_not_found = 9,
        asset_invalid = 10
    };

    /**
     * @brief resolved scene module table
     *
     * The native side owns this table for the host lifetime. Callbacks are static functions;
     * node_handle values are process-wide node ids. Spawn and despawn take the scene id because
     * they need that scene's spawn slots, but the ABI never passes an opaque context.
     */
    struct scene_api {
        script_status(__cdecl* get_transform)(node_handle node, transform_value* value) = nullptr;
        script_status(__cdecl* set_transform)(node_handle node, transform_value const* value) = nullptr;
        script_status(__cdecl* try_spawn)(std::uint64_t scene, utf8_view asset, transform_value const* value,
                                          node_handle* node) = nullptr;
        script_status(__cdecl* try_despawn)(std::uint64_t scene, node_handle node) = nullptr;
        // Creates an empty managed scene tree. The returned id is independent of script scene ids.
        script_status(__cdecl* create_scene_tree)(std::uint64_t* tree) = nullptr;
        // Hands a managed tree to the renderer for the current scene startup. The tree becomes
        // the runtime scene and must not be destroyed while active.
        script_status(__cdecl* activate_scene)(std::uint64_t tree) = nullptr;
        script_status(__cdecl* get_active_scene)(std::uint64_t* tree) = nullptr;
        // Detaches the active tree from the current script scene before it is destroyed. The
        // native renderer must not have begun a frame with this tree after the call returns.
        script_status(__cdecl* deactivate_scene)(std::uint64_t tree) = nullptr;
        script_status(__cdecl* destroy_scene_tree)(std::uint64_t tree) = nullptr;
        // Appends a node to a managed tree. A zero parent creates a root. The returned node id is
        // process-wide and stays valid until the node or one of its ancestors is destroyed.
        script_status(__cdecl* create_node)(std::uint64_t tree, node_handle parent, utf8_view name,
                                            transform_value const* local, node_handle* node) = nullptr;
        script_status(__cdecl* destroy_node)(std::uint64_t tree, node_handle node) = nullptr;
        // Local transform of a managed-tree node, stored in its placement component. These calls
        // do not consult a script scene's spawn slots.
        script_status(__cdecl* get_node_transform)(std::uint64_t tree, node_handle node,
                                                   transform_value* value) = nullptr;
        script_status(__cdecl* set_node_transform)(std::uint64_t tree, node_handle node,
                                                   transform_value const* value) = nullptr;
        script_status(__cdecl* set_node_visible)(std::uint64_t tree, node_handle node,
                                                 std::uint8_t visible) = nullptr;
        // Enumerates library asset ids in sorted order. The returned view borrows the library's
        // storage and stays valid until the library is rescanned. Out-of-range indexes return
        // asset_not_found.
        script_status(__cdecl* list_asset)(std::uint32_t index, utf8_view* id) = nullptr;
        // The asset manager loads one library asset and returns a process-wide handle. The
        // source data stays alive while at least one handle is held.
        script_status(__cdecl* load_asset)(utf8_view id, asset_handle* asset) = nullptr;
        script_status(__cdecl* release_asset)(asset_handle asset) = nullptr;
        // Instantiates the whole asset as a subtree under parent (a zero parent creates a root).
        // The offset is the asset instance root's local transform.
        script_status(__cdecl* bind_asset)(std::uint64_t tree, node_handle parent, asset_handle asset,
                                           transform_value const* offset, node_handle* root) = nullptr;
    };

    enum class native_module : std::uint32_t {
        scene = 1
    };

    // Keep layouts and calling conventions in sync with managed_bootstrap.cs.
    struct native_api {
        void(__cdecl* log)(char const* utf8, std::uint32_t length) = nullptr;
        script_status(__cdecl* get_module)(std::uint32_t module, void const** table) = nullptr;
    };

    // One frame's borrowed state. The layout is exact for this ABI version; any field
    // addition, removal, or reorder changes managed_abi_version.
    struct frame_info {
        std::uint64_t scene = 0;
        float delta_time = 0.0F;
        std::uint32_t input_count = 0;
        std::uint8_t const* input = nullptr;
    };

    static_assert(sizeof(frame_info) == 24);

    struct managed_api {
        // The managed game owns its scene and script lifetime. The host only forwards frames
        // and asks for one program-wide shutdown.
        // The frame and its input bytes are borrowed only during the update.
        script_status(__cdecl* update)(frame_info const* frame) = nullptr;
        script_status(__cdecl* stop)() = nullptr;
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
        bridge_initialization_failed,
        scene_module_unavailable,
        scene_module_invalid,
        game_assembly_missing
    };

    struct host_paths {
        std::filesystem::path assembly;
        std::filesystem::path runtime_config;
        std::filesystem::path nethost;
        std::filesystem::path dotnet_root{};
        // Project script assembly loaded by the managed host during initialization.
        std::filesystem::path game_assembly;
    };

    struct host_result {
        host_status status;
        std::int32_t detail = 0;
    };

    [[nodiscard]] host_result initialize_managed_host(host_paths const& paths, native_api const& api, managed_api& scripts,
                                                     std::uint32_t abi_version = managed_abi_version);

} // namespace deren::scripting
