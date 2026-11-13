module deren.app.scripting;

namespace deren::app {
    namespace {
        scripting::transform_value read_pose(app_config::scene_object_settings const& object) {
            return {
                {.x = object.position[0], .y = object.position[1], .z = object.position[2]},
                {.x = object.rotation[0], .y = object.rotation[1], .z = object.rotation[2]},
                {.x = object.scale[0], .y = object.scale[1], .z = object.scale[2]}};
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
        std::filesystem::path const& assembly, engine::scene_tree::scene& scene,
        std::span<app_config::scene_object_settings> objects, std::span<utility::ecs::entity const> node_ids,
        std::span<input::action_binding const> actions) {
        if (node_ids.size() != objects.size()) {
            return std::unexpected(std::format("Script scene has {} object setting(s) but {} scene node id(s).", objects.size(), node_ids.size()));
        }
        static std::uint64_t next_scene_id = 1;
        scene_id = next_scene_id++;
        scene_objects = objects;
        tree = &scene;
        slots.clear();
        utility::vessel::vector<scripting::script_binding> bindings;
        for (std::size_t index = 0; index < objects.size(); ++index) {
            auto const& object = objects[index];
            auto const node = node_ids[index];
            if (object.spawn_slot) {
                if (!scene.find_node(node)) {
                    return std::unexpected(std::format("Spawn slot '{}' has no live scene node.", object.name));
                }
                slots.push_back(object_slot{index, node});
                continue;
            }
            if (!object.script) {
                continue;
            }
            if (!scene.find_node(node)) {
                return std::unexpected(std::format("Script object '{}' has no live scene node.", object.name));
            }
            if (scene.components.try_emplace<placement>(node, placement{read_pose(object)}) == nullptr) {
                return std::unexpected(std::format("Script object '{}' already has a script placement.", object.name));
            }
            slots.push_back(object_slot{index, node});
            bindings.push_back(scripting::script_binding{
                {scene_id, node.index, node.generation},
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
        if (tree == nullptr) {
            return;
        }
        for (auto const& slot : slots) {
            if (auto* value = tree->components.try_get<placement>(slot.node)) {
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
        if (tree == nullptr) {
            return false;
        }
        bool changed = false;
        for (auto const& slot : slots) {
            auto* value = tree->components.try_get<placement>(slot.node);
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

    script_scene::placement* script_scene::find(scripting::node_handle handle) {
        if (handle.scene != scene_id || tree == nullptr) {
            return nullptr;
        }
        auto* node = tree->find_node({handle.index, handle.generation});
        return node == nullptr ? nullptr : tree->components.try_get<placement>(node->node_id);
    }

    void __cdecl script_scene::log(void*, char const* text, std::uint32_t length) {
        utility::log("scripts: {}", std::string_view{text, length});
    }

    scripting::script_status __cdecl script_scene::get_transform(void* context, scripting::node_handle node, scripting::transform_value* value) {
        if (value == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        auto* placement = static_cast<script_scene*>(context)->find(node);
        if (placement == nullptr) {
            return scripting::script_status::invalid_node;
        }
        *value = placement->transform;
        return scripting::script_status::success;
    }

    scripting::script_status __cdecl script_scene::set_transform(void* context, scripting::node_handle node, scripting::transform_value const* value) {
        if (value == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        auto* placement = static_cast<script_scene*>(context)->find(node);
        if (placement == nullptr) {
            return scripting::script_status::invalid_node;
        }
        if (!is_finite(value->position) || !is_finite(value->rotation_degrees) || !is_finite(value->scale) ||
            value->scale.x == 0.0f || value->scale.y == 0.0f || value->scale.z == 0.0f) {
            return scripting::script_status::invalid_argument;
        }
        placement->transform = *value;
        placement->dirty = true;
        return scripting::script_status::success;
    }

    scripting::script_status __cdecl script_scene::try_spawn(void* context, scripting::utf8_view asset,
                                                             scripting::transform_value const* value, scripting::node_handle* node) {
        auto& owner = *static_cast<script_scene*>(context);
        if (asset.data == nullptr || asset.length == 0 || value == nullptr || node == nullptr ||
            !is_finite(value->position) || !is_finite(value->rotation_degrees) || !is_finite(value->scale) ||
            value->scale.x == 0.0f || value->scale.y == 0.0f || value->scale.z == 0.0f) {
            return scripting::script_status::invalid_argument;
        }
        if (owner.closing || owner.tree == nullptr) {
            return scripting::script_status::spawn_unavailable;
        }
        auto const id = std::string_view{asset.data, asset.length};
        for (auto& slot : owner.slots) {
            auto const& object = owner.scene_objects[slot.index];
            if (object.spawn_slot && std::string_view{object.asset} == id &&
                owner.tree->find_node(slot.node) != nullptr &&
                owner.tree->components.try_get<placement>(slot.node) == nullptr) {
                if (owner.tree->components.try_emplace<placement>(slot.node, placement{*value, true}) == nullptr) {
                    return scripting::script_status::spawn_unavailable;
                }
                owner.scene_objects[slot.index].visible = true;
                *node = {owner.scene_id, slot.node.index, slot.node.generation};
                return scripting::script_status::success;
            }
        }
        return scripting::script_status::spawn_unavailable;
    }

    scripting::script_status __cdecl script_scene::try_despawn(void* context, scripting::node_handle node) {
        auto& owner = *static_cast<script_scene*>(context);
        if (node.scene == owner.scene_id && owner.tree != nullptr) {
            utility::ecs::entity const handle{node.index, node.generation};
            for (auto& slot : owner.slots) {
                if (handle.valid() && slot.node == handle && owner.scene_objects[slot.index].spawn_slot &&
                    owner.tree->components.remove<placement>(handle)) {
                    owner.scene_objects[slot.index].visible = false;
                    return scripting::script_status::success;
                }
            }
        }
        return scripting::script_status::invalid_node;
    }
} // namespace deren::app
