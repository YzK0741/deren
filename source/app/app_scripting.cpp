module deren.app.scripting;

namespace deren::app {
    namespace {
        scripting::transform_value read_pose(app_config::scene_object_settings const& object) {
            return {
                {object.position[0], object.position[1], object.position[2]},
                {object.rotation[0], object.rotation[1], object.rotation[2]},
                {object.scale[0], object.scale[1], object.scale[2]}};
        }

        bool is_finite(scripting::vector_3 value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }
    } // namespace

    script_scene::~script_scene() {
        closing = true;
        if (running && scripts.stop_scene(scene_id) != scripting::script_status::success) {
            utility::log("scripts: scene {} stopped with script errors", scene_id);
        }
    }

    std::expected<void, std::string> script_scene::start(
        std::filesystem::path const& assembly, std::span<app_config::scene_object_settings> objects,
        std::span<input::action_binding const> actions) {
        static std::uint64_t next_scene_id = 1;
        scene_id = next_scene_id++;
        scene_objects = objects;
        utility::vessel::vector<scripting::script_binding> bindings;
        for (std::size_t index = 0; index < objects.size(); ++index) {
            auto const& object = objects[index];
            if (object.spawn_slot) {
                slots.push_back(object_slot{index, utility::ecs::null_entity});
                continue;
            }
            if (!object.script) {
                continue;
            }
            auto const entity = entities.create();
            entities.try_emplace<placement>(entity, placement{read_pose(object)});
            slots.push_back(object_slot{index, entity});
            bindings.push_back(scripting::script_binding{
                {scene_id, entity.index, entity.generation},
                {object.script->data(), static_cast<std::uint32_t>(object.script->size())}});
        }

        auto const runtime_dir = utility::executable_directory() / "managed";
        scripting::native_api const api{
            .context = this, .log = log, .get_transform = get_transform, .set_transform = set_transform, .try_spawn = try_spawn, .try_despawn = try_despawn};
        auto const result = scripting::initialize_managed_host(
            {.assembly = runtime_dir / "Deren.Managed.dll",
             .runtime_config = runtime_dir / "Deren.Managed.runtimeconfig.json",
             .nethost = runtime_dir / "nethost.dll"},
            api, scripts);
        if (result.status != scripting::host_status::success) {
            if (result.status == scripting::host_status::runtime_discovery_failed ||
                result.status == scripting::host_status::runtime_initialization_failed) {
                return std::unexpected(std::format("Cannot start .NET 10 x64 for '{}'. Check the installed runtime (host detail 0x{:08x}).",
                                                   assembly.string(), static_cast<std::uint32_t>(result.detail)));
            }
            return std::unexpected(std::format("Cannot initialize managed scripting from '{}' (host status {}, detail 0x{:08x}).",
                                               runtime_dir.string(), static_cast<int>(result.status), static_cast<std::uint32_t>(result.detail)));
        }
        auto const path = assembly.u8string();
        utility::vessel::vector<scripting::utf8_view> action_names;
        action_names.reserve(actions.size());
        for (auto const& action : actions) {
            action_names.push_back(scripting::utf8_view{action.name.data(), static_cast<std::uint32_t>(action.name.size())});
        }
        auto const status = scripts.start_scene(
            &api,
            scene_id, {reinterpret_cast<char const*>(path.data()), static_cast<std::uint32_t>(path.size())},
            bindings.data(), static_cast<std::uint32_t>(bindings.size()),
            action_names.data(), static_cast<std::uint32_t>(action_names.size()));
        if (status != scripting::script_status::success) {
            return std::unexpected(std::format("Cannot start scripts from '{}' (script status {}). See the preceding script diagnostics.",
                                               assembly.string(), static_cast<int>(status)));
        }
        running = true;
        utility::log("scripts: started {} instance(s) from '{}'", bindings.size(), assembly.string());
        return {};
    }

    void script_scene::read_object_edits() {
        for (auto const& slot : slots) {
            if (auto* value = entities.try_get<placement>(slot.entity)) {
                value->transform = read_pose(scene_objects[slot.index]);
            }
        }
    }

    bool script_scene::update(float dt, std::span<std::uint8_t const> input_frame) {
        if (scripts.update_scene(scene_id, dt, input_frame.data(), static_cast<std::uint32_t>(input_frame.size())) != scripting::script_status::success) {
            utility::error("scripts: could not update scene {}", scene_id);
            return false;
        }
        return true;
    }

    bool script_scene::publish_transforms() {
        bool changed = false;
        for (auto const& slot : slots) {
            auto* value = entities.try_get<placement>(slot.entity);
            auto& object = scene_objects[slot.index];
            if (object.spawn_slot && object.visible != (value != nullptr)) {
                object.visible = value != nullptr;
                changed = true;
            }
            if (value == nullptr || !value->dirty) {
                continue;
            }
            auto const& pose = value->transform;
            object.position = {pose.position.x, pose.position.y, pose.position.z};
            object.rotation = {pose.rotation_degrees.x, pose.rotation_degrees.y, pose.rotation_degrees.z};
            object.scale = {pose.scale.x, pose.scale.y, pose.scale.z};
            value->dirty = false;
            changed = true;
        }
        return changed;
    }

    script_scene::placement* script_scene::find(scripting::entity_handle handle) {
        return handle.scene == scene_id ? entities.try_get<placement>({handle.index, handle.generation}) : nullptr;
    }

    void __cdecl script_scene::log(void*, char const* text, std::uint32_t length) {
        utility::log("scripts: {}", std::string_view{text, length});
    }

    scripting::script_status __cdecl script_scene::get_transform(void* context, scripting::entity_handle entity, scripting::transform_value* value) {
        auto* placement = static_cast<script_scene*>(context)->find(entity);
        if (placement == nullptr) {
            return scripting::script_status::invalid_entity;
        }
        *value = placement->transform;
        return scripting::script_status::success;
    }

    scripting::script_status __cdecl script_scene::set_transform(void* context, scripting::entity_handle entity, scripting::transform_value const* value) {
        auto* placement = static_cast<script_scene*>(context)->find(entity);
        if (placement == nullptr) {
            return scripting::script_status::invalid_entity;
        }
        if (!is_finite(value->position) || !is_finite(value->rotation_degrees) || !is_finite(value->scale)) {
            return scripting::script_status::invalid_argument;
        }
        placement->transform = *value;
        placement->dirty = true;
        return scripting::script_status::success;
    }

    scripting::script_status __cdecl script_scene::try_spawn(void* context, scripting::utf8_view asset,
                                                             scripting::transform_value const* value, scripting::entity_handle* entity) {
        auto& owner = *static_cast<script_scene*>(context);
        if (asset.data == nullptr || asset.length == 0 || value == nullptr || entity == nullptr ||
            !is_finite(value->position) || !is_finite(value->rotation_degrees) || !is_finite(value->scale) ||
            value->scale.x == 0.0f || value->scale.y == 0.0f || value->scale.z == 0.0f) {
            return scripting::script_status::invalid_argument;
        }
        if (owner.closing) {
            return scripting::script_status::spawn_unavailable;
        }
        auto const id = std::string_view{asset.data, asset.length};
        for (auto& slot : owner.slots) {
            auto const& object = owner.scene_objects[slot.index];
            if (object.spawn_slot && !slot.entity.valid() && std::string_view{object.asset} == id) {
                slot.entity = owner.entities.create();
                owner.entities.try_emplace<placement>(slot.entity, placement{*value, true});
                *entity = {owner.scene_id, slot.entity.index, slot.entity.generation};
                return scripting::script_status::success;
            }
        }
        return scripting::script_status::spawn_unavailable;
    }

    scripting::script_status __cdecl script_scene::try_despawn(void* context, scripting::entity_handle entity) {
        auto& owner = *static_cast<script_scene*>(context);
        if (entity.scene == owner.scene_id) {
            utility::ecs::entity const handle{entity.index, entity.generation};
            for (auto& slot : owner.slots) {
                if (handle.valid() && slot.entity == handle && owner.scene_objects[slot.index].spawn_slot &&
                    owner.entities.destroy(handle)) {
                    slot.entity = utility::ecs::null_entity;
                    return scripting::script_status::success;
                }
            }
        }
        return scripting::script_status::invalid_entity;
    }
} // namespace deren::app
