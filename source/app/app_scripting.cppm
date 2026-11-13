// module version: 0.2.0  (independent of the app version in CMakeLists project(VERSION))

export module deren.app.scripting;

import deren.vstd;
import deren.application_configuration;
import deren.engine.scene_tree;
import deren.gltf_loader;
import deren.scripting.managed_host;
import deren.utility;

export namespace deren::app {
    /**
     * @brief Decoded source data for one managed asset binding.
     *
     * The managed asset registry owns the scene and the resolved material slice for the whole
     * handle lifetime. The binder imports this data without consulting native project object
     * metadata or a preloaded asset slot.
     */
    struct managed_asset_source {
        gltf::scenes const* scenes = nullptr;
        std::span<gltf::resolved_material const> materials;
        std::array<float, 3> import_shift{};
    };

    /**
     * @brief Host callback that turns a managed asset binding into rendered primitives.
     *
     * The callback runs while a managed tree is bound under an existing node. It should attach
     * the asset's imported subtree to @p parent; returning false leaves the binding failed and
     * the wrapper node is removed.
     */
    struct managed_asset_binder {
        void* context = nullptr;
        bool(__cdecl* bind)(void* context, engine::scene_tree::scene_node& parent, std::string_view asset_id,
                            managed_asset_source const& source) = nullptr;
    };

    /**
     * @brief Host callback that makes a managed tree the rendered scene.
     *
     * The managed scene builder creates the tree, fills it, and calls activate_scene() before
     * binding assets. The callback installs it into the renderer and returns whether it was
     * accepted.
     */
    struct managed_scene_host {
        void* context = nullptr;
        bool(__cdecl* set_scene)(void* context, engine::scene_tree::scene& scene) = nullptr;
    };

    class script_scene {
    public:
        script_scene() = default;
        ~script_scene();
        script_scene(script_scene const&) = delete;
        script_scene& operator=(script_scene const&) = delete;

        [[nodiscard]] std::expected<void, std::string> start(
            std::filesystem::path const& assembly, managed_scene_host host,
            std::filesystem::path const& asset_library_root = {}, managed_asset_binder binder = {});
        [[nodiscard]] bool update(float dt, std::span<std::uint8_t const> input_frame);
        /** Drops the startup-only asset binder; bindings after the scene started are unsupported. */
        void clear_asset_binder() noexcept;

    private:
        struct placement {
            scripting::transform_value transform;
            bool dirty = false;
        };

        static placement* find(scripting::node_handle handle);
        static script_scene* find_scene(std::uint64_t scene);
        static void __cdecl log(char const* text, std::uint32_t length);
        static scripting::script_status __cdecl get_module(std::uint32_t module, void const** table);
        static scripting::script_status __cdecl get_transform(scripting::node_handle node, scripting::transform_value* value);
        static scripting::script_status __cdecl set_transform(scripting::node_handle node, scripting::transform_value const* value);
        static scripting::script_status __cdecl try_spawn(std::uint64_t scene, scripting::utf8_view asset,
                                                          scripting::transform_value const* value, scripting::node_handle* node);
        static scripting::script_status __cdecl try_despawn(std::uint64_t scene, scripting::node_handle node);
        static scripting::script_status __cdecl create_scene_tree(std::uint64_t* tree);
        static scripting::script_status __cdecl activate_scene(std::uint64_t tree);
        static scripting::script_status __cdecl get_active_scene(std::uint64_t* tree);
        static scripting::script_status __cdecl deactivate_scene(std::uint64_t tree);
        static scripting::script_status __cdecl destroy_scene_tree(std::uint64_t tree);
        static scripting::script_status __cdecl create_node(std::uint64_t tree, scripting::node_handle parent,
                                                            scripting::utf8_view name,
                                                            scripting::transform_value const* local,
                                                            scripting::node_handle* node);
        static scripting::script_status __cdecl destroy_node(std::uint64_t tree, scripting::node_handle node);
        static scripting::script_status __cdecl get_node_transform(std::uint64_t tree, scripting::node_handle node,
                                                                   scripting::transform_value* value);
        static scripting::script_status __cdecl set_node_transform(std::uint64_t tree, scripting::node_handle node,
                                                                   scripting::transform_value const* value);
        static scripting::script_status __cdecl set_node_visible(std::uint64_t tree, scripting::node_handle node,
                                                                 std::uint8_t visible);
        static scripting::script_status __cdecl list_asset(std::uint32_t index, scripting::utf8_view* id);
        static scripting::script_status __cdecl load_asset(scripting::utf8_view id, scripting::asset_handle* asset);
        static scripting::script_status __cdecl release_asset(scripting::asset_handle asset);
        static scripting::script_status __cdecl bind_asset(std::uint64_t tree, scripting::node_handle parent,
                                                           scripting::asset_handle asset,
                                                           scripting::transform_value const* offset,
                                                           scripting::node_handle* root);

        scripting::managed_api scripts;
        engine::scene_tree::scene* tree = nullptr;
        std::uint64_t tree_id = 0;
        managed_scene_host host;
        managed_asset_binder asset_binder;
        std::uint64_t scene_id = 0;
        bool running = false;
        bool closing = false;
    };
} // namespace deren::app
