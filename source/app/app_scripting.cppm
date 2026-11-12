export module deren.app.scripting;

import deren.vstd;
import deren.application_configuration;
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
            std::filesystem::path const& assembly, std::span<app_config::scene_object_settings> objects);
        void read_object_edits();
        [[nodiscard]] bool update(float dt);
        [[nodiscard]] bool publish_transforms();

    private:
        struct placement {
            scripting::transform_value transform;
            bool dirty = false;
        };

        struct object_slot {
            std::size_t index;
            utility::ecs::entity entity;
        };

        placement* find(scripting::entity_handle handle);
        static void __cdecl log(void*, char const* text, std::uint32_t length);
        static scripting::script_status __cdecl get_transform(void* context, scripting::entity_handle entity, scripting::transform_value* value);
        static scripting::script_status __cdecl set_transform(void* context, scripting::entity_handle entity, scripting::transform_value const* value);

        scripting::managed_api scripts;
        utility::ecs::registry entities;
        utility::vessel::vector<object_slot> slots;
        std::span<app_config::scene_object_settings> scene_objects;
        std::uint64_t scene_id = 0;
        bool running = false;
    };
} // namespace deren::app
