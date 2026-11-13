export module deren.app.scripting;

import deren.vstd;
import deren.application_configuration;
import deren.engine.scene_tree;
import deren.scripting.managed_host;
import deren.utility;

export namespace deren::app {
    class script_scene {
    public:
        script_scene() = default;
        ~script_scene();
        script_scene(script_scene const&) = delete;
        script_scene& operator=(script_scene const&) = delete;

        [[nodiscard]] std::expected<void, std::string> start(
            std::filesystem::path const& assembly, engine::scene_tree::scene& scene,
            std::span<app_config::scene_object_settings> objects, std::span<utility::ecs::entity const> node_ids,
            std::span<input::action_binding const> actions);
        void read_object_edits();
        [[nodiscard]] bool update(float dt, std::span<std::uint8_t const> input_frame);
        [[nodiscard]] bool publish_transforms();

    private:
        struct placement {
            scripting::transform_value transform;
            bool dirty = false;
        };

        struct object_slot {
            std::size_t index;
            utility::ecs::entity node;
        };

        placement* find(scripting::node_handle handle);
        static void __cdecl log(void*, char const* text, std::uint32_t length);
        static scripting::script_status __cdecl get_transform(void* context, scripting::node_handle node, scripting::transform_value* value);
        static scripting::script_status __cdecl set_transform(void* context, scripting::node_handle node, scripting::transform_value const* value);
        static scripting::script_status __cdecl try_spawn(void* context, scripting::utf8_view asset, scripting::transform_value const* value, scripting::node_handle* node);
        static scripting::script_status __cdecl try_despawn(void* context, scripting::node_handle node);

        scripting::managed_api scripts;
        engine::scene_tree::scene* tree = nullptr;
        utility::vessel::vector<object_slot> slots;
        std::span<app_config::scene_object_settings> scene_objects;
        std::uint64_t scene_id = 0;
        bool running = false;
        bool closing = false;
    };
} // namespace deren::app
