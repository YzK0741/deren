module;
#include <algorithm>
#include <cmath>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
module deren.app.scripting;
import deren.asset_library;
import deren.gltf_loader;

namespace deren::app {
    namespace {
        bool is_finite(scripting::vector_3 value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        bool is_valid_pose(scripting::transform_value const& value) {
            return is_finite(value.position) && is_finite(value.rotation_degrees) && is_finite(value.scale) &&
                   value.scale.x != 0.0f && value.scale.y != 0.0f && value.scale.z != 0.0f;
        }

        glm::mat4 to_local_matrix(scripting::transform_value const& value) {
            glm::mat4 local = glm::translate(glm::mat4(1.0f), glm::vec3{value.position.x, value.position.y, value.position.z});
            local = glm::rotate(local, glm::radians(value.rotation_degrees.y), glm::vec3{0.0f, 1.0f, 0.0f});
            local = glm::rotate(local, glm::radians(value.rotation_degrees.x), glm::vec3{1.0f, 0.0f, 0.0f});
            local = glm::rotate(local, glm::radians(value.rotation_degrees.z), glm::vec3{0.0f, 0.0f, 1.0f});
            return glm::scale(local, glm::vec3{value.scale.x, value.scale.y, value.scale.z});
        }

        scripting::node_handle to_handle(utility::ecs::entity id) {
            if (!id.valid()) {
                return {};
            }
            return {.value = (static_cast<std::uint64_t>(id.generation) << 32) | id.index};
        }

        utility::ecs::entity to_entity(scripting::node_handle handle) {
            if (handle.value == 0) {
                return {};
            }
            utility::ecs::entity const id{static_cast<std::uint32_t>(handle.value),
                                          static_cast<std::uint32_t>(handle.value >> 32)};
            return id.valid() ? id : utility::ecs::entity{};
        }

        scripting::transform_value pose_from_matrix(glm::mat4 const& matrix) {
            glm::vec3 scale{1.0f, 1.0f, 1.0f};
            glm::vec3 translation{0.0f, 0.0f, 0.0f};
            glm::vec3 skew{0.0f, 0.0f, 0.0f};
            glm::vec4 perspective{0.0f, 0.0f, 0.0f, 1.0f};
            glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
            if (!glm::decompose(matrix, scale, orientation, translation, skew, perspective) ||
                !std::isfinite(translation.x) || !std::isfinite(translation.y) || !std::isfinite(translation.z) ||
                !std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z) ||
                !std::isfinite(orientation.w) || !std::isfinite(orientation.x) || !std::isfinite(orientation.y) ||
                !std::isfinite(orientation.z) || scale.x == 0.0f || scale.y == 0.0f || scale.z == 0.0f) {
                return {};
            }
            auto const rotation = glm::degrees(glm::eulerAngles(glm::normalize(orientation)));
            return {
                {.x = translation.x, .y = translation.y, .z = translation.z},
                {.x = rotation.x, .y = rotation.y, .z = rotation.z},
                {.x = scale.x, .y = scale.y, .z = scale.z}};
        }

        utility::vessel::vector<script_scene*>& active_scenes() {
            static utility::vessel::vector<script_scene*> scenes;
            return scenes;
        }

        struct managed_tree {
            std::uint64_t id = 0;
            std::unique_ptr<engine::scene_tree::scene> owned;
            engine::scene_tree::scene* value = nullptr;
            script_scene* owner = nullptr;
            bool active = false;
        };

        // Managed trees outlive the script_scene objects that created them. Keep the registry
        // alive until process exit so its destruction order is independent of the node registry.
        utility::vessel::vector<managed_tree>& managed_trees() {
            static auto* trees = new utility::vessel::vector<managed_tree>();
            return *trees;
        }

        std::uint64_t next_tree_id() {
            static std::uint64_t next = 1;
            return next++;
        }

        script_scene* starting_scene = nullptr;

        managed_tree* find_managed_tree_entry(std::uint64_t const id) {
            for (auto& entry : managed_trees()) {
                if (entry.id == id) {
                    return &entry;
                }
            }
            return nullptr;
        }

        engine::scene_tree::scene* find_managed_tree(std::uint64_t const id) {
            auto* entry = find_managed_tree_entry(id);
            return entry == nullptr ? nullptr : entry->value;
        }

        struct managed_asset {
            std::uint64_t id = 0;
            std::string asset_id;
            std::unique_ptr<gltf::scenes> model;
            utility::vessel::vector<gltf::resolved_material> materials;
            std::array<float, 3> import_shift{};
            unsigned references = 0;
        };

        struct asset_state {
            std::filesystem::path root;
            asset_library::asset_library library;
            bool scanned = false;
            utility::vessel::vector<managed_asset> assets;
            std::uint64_t next_asset_id = 1;
        };

        // Asset source data outlives the script scenes that loaded it; like managed trees, the
        // registry itself is intentionally leaked so static destruction order cannot invalidate it.
        asset_state& assets() {
            static auto* state = new asset_state();
            return *state;
        }

        void configure_asset_library(std::filesystem::path const& root) {
            if (root.empty()) {
                return;
            }
            auto& state = assets();
            if (state.scanned && state.root == root) {
                return;
            }
            state.root = root;
            state.library = asset_library::asset_library::scan(root);
            state.scanned = true;
            if (!state.library.valid()) {
                utility::error("scripts: asset library '{}' is not usable: {}", root.string(), state.library.error());
            }
        }

        managed_asset* find_asset(std::uint64_t const id) {
            for (auto& entry : assets().assets) {
                if (entry.id == id) {
                    return &entry;
                }
            }
            return nullptr;
        }

    } // namespace

    script_scene::~script_scene() {
        closing = true;
        if (running && scripts.stop != nullptr && scripts.stop() != scripting::script_status::success) {
            utility::log("scripts: managed game stopped with script errors");
        }
        auto& scenes = active_scenes();
        std::erase(scenes, this);
        if (tree_id != 0) {
            if (auto* entry = find_managed_tree_entry(tree_id)) {
                entry->active = false;
                entry->owner = nullptr;
            }
        }
        tree = nullptr;
        tree_id = 0;
    }

    std::expected<void, std::string> script_scene::start(
        std::filesystem::path const& assembly, managed_scene_host scene_host,
        std::filesystem::path const& asset_library_root, managed_asset_binder binder) {
        configure_asset_library(asset_library_root);
        static std::uint64_t next_scene_id = 1;
        scene_id = next_scene_id++;
        host = scene_host;
        tree = nullptr;
        tree_id = 0;
        asset_binder = binder;

        auto const runtime_dir = utility::executable_directory() / "managed";
        scripting::native_api const api{.log = log, .get_module = get_module};
        starting_scene = this;
        auto const result = scripting::initialize_managed_host(
            {.assembly = runtime_dir / "Deren.Managed.dll",
             .runtime_config = runtime_dir / "Deren.Managed.runtimeconfig.json",
             .nethost = runtime_dir / "nethost.dll",
             .game_assembly = assembly},
            api, scripts);
        starting_scene = nullptr;
        if (result.status != scripting::host_status::success) {
            if (result.status == scripting::host_status::runtime_discovery_failed ||
                result.status == scripting::host_status::runtime_initialization_failed) {
                return std::unexpected(std::format("Cannot start .NET 10 x64 for '{}'. Check the installed runtime (host detail 0x{:08x}).",
                                                   assembly.string(), static_cast<std::uint32_t>(result.detail)));
            }
            return std::unexpected(std::format("Cannot initialize managed scripting from '{}' (host status {}, detail 0x{:08x}).",
                                               runtime_dir.string(), static_cast<int>(result.status), static_cast<std::uint32_t>(result.detail)));
        }
        if (tree == nullptr) {
            return std::unexpected("The managed game did not activate a scene tree.");
        }
        running = true;
        active_scenes().push_back(this);
        utility::log("scripts: managed game initialized from '{}'", assembly.string());
        return {};
    }

    bool script_scene::update(float dt, std::span<std::uint8_t const> input_frame) {
        scripting::frame_info const frame{
            .scene = scene_id,
            .delta_time = dt,
            .input_count = static_cast<std::uint32_t>(input_frame.size()),
            .input = input_frame.data()};
        if (scripts.update(&frame) != scripting::script_status::success) {
            utility::error("scripts: could not update managed scene");
            return false;
        }
        return true;
    }

    void script_scene::clear_asset_binder() noexcept {
        asset_binder = {};
    }

    script_scene::placement* script_scene::find(scripting::node_handle handle) {
        utility::ecs::entity const id = to_entity(handle);
        if (!id.valid()) {
            return nullptr;
        }
        return engine::scene_tree::components().try_get<placement>(id);
    }

    script_scene* script_scene::find_scene(std::uint64_t const scene) {
        for (auto* candidate : active_scenes()) {
            if (candidate->scene_id == scene) {
                return candidate;
            }
        }
        return nullptr;
    }

    void __cdecl script_scene::log(char const* text, std::uint32_t length) {
        utility::log("scripts: {}", std::string_view{text, length});
    }

    scripting::script_status __cdecl script_scene::get_module(std::uint32_t const module, void const** table) {
        if (table == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        *table = nullptr;
        if (module != static_cast<std::uint32_t>(scripting::native_module::scene)) {
            return scripting::script_status::module_unavailable;
        }
        static scripting::scene_api const scene_api{
            .get_transform = get_transform,
            .set_transform = set_transform,
            .try_spawn = try_spawn,
            .try_despawn = try_despawn,
            .create_scene_tree = create_scene_tree,
            .activate_scene = activate_scene,
            .get_active_scene = get_active_scene,
            .deactivate_scene = deactivate_scene,
            .destroy_scene_tree = destroy_scene_tree,
            .create_node = create_node,
            .destroy_node = destroy_node,
            .get_node_transform = get_node_transform,
            .set_node_transform = set_node_transform,
            .set_node_visible = set_node_visible,
            .list_asset = list_asset,
            .load_asset = load_asset,
            .release_asset = release_asset,
            .bind_asset = bind_asset};
        *table = &scene_api;
        return scripting::script_status::success;
    }

    scripting::script_status __cdecl script_scene::create_scene_tree(std::uint64_t* tree) {
        if (tree == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        *tree = 0;
        auto value = std::make_unique<engine::scene_tree::scene>();
        auto* raw = value.get();
        auto const id = next_tree_id();
        managed_trees().push_back(managed_tree{.id = id, .owned = std::move(value), .value = raw, .owner = nullptr, .active = false});
        *tree = id;
        return scripting::script_status::success;
    }

    scripting::script_status __cdecl script_scene::activate_scene(std::uint64_t const tree) {
        if (starting_scene == nullptr || tree == 0) {
            return scripting::script_status::invalid_argument;
        }
        auto* entry = find_managed_tree_entry(tree);
        if (entry == nullptr || entry->value == nullptr || entry->owner != nullptr) {
            return scripting::script_status::scene_not_found;
        }
        if (starting_scene->tree_id != 0) {
            return scripting::script_status::invalid_argument;
        }
        entry->owner = starting_scene;
        entry->active = true;
        starting_scene->tree = entry->value;
        starting_scene->tree_id = entry->id;
        if (starting_scene->host.set_scene == nullptr ||
            !starting_scene->host.set_scene(starting_scene->host.context, *entry->value)) {
            entry->owner = nullptr;
            entry->active = false;
            starting_scene->tree = nullptr;
            starting_scene->tree_id = 0;
            return scripting::script_status::invalid_argument;
        }
        return scripting::script_status::success;
    }

    scripting::script_status __cdecl script_scene::get_active_scene(std::uint64_t* tree) {
        if (tree == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        *tree = 0;
        for (auto* candidate : active_scenes()) {
            if (candidate->tree_id != 0) {
                *tree = candidate->tree_id;
                return scripting::script_status::success;
            }
        }
        return scripting::script_status::scene_not_found;
    }

    scripting::script_status __cdecl script_scene::deactivate_scene(std::uint64_t const tree) {
        auto* entry = find_managed_tree_entry(tree);
        if (entry == nullptr || !entry->active) {
            return scripting::script_status::scene_not_found;
        }
        if (entry->owner != nullptr && entry->owner->tree_id == tree) {
            entry->owner->tree = nullptr;
            entry->owner->tree_id = 0;
        }
        entry->owner = nullptr;
        entry->active = false;
        return scripting::script_status::success;
    }

    scripting::script_status __cdecl script_scene::destroy_scene_tree(std::uint64_t const tree) {
        if (tree == 0) {
            return scripting::script_status::invalid_argument;
        }
        auto& trees = managed_trees();
        for (auto it = trees.begin(); it != trees.end(); ++it) {
            if (it->id == tree) {
                if (it->active) {
                    return scripting::script_status::invalid_argument;
                }
                trees.erase(it);
                return scripting::script_status::success;
            }
        }
        return scripting::script_status::scene_not_found;
    }

    scripting::script_status __cdecl script_scene::create_node(std::uint64_t const tree, scripting::node_handle const parent,
                                                               scripting::utf8_view const name,
                                                               scripting::transform_value const* local,
                                                               scripting::node_handle* node) {
        if (node == nullptr || local == nullptr || (name.data == nullptr && name.length != 0) ||
            !is_valid_pose(*local)) {
            return scripting::script_status::invalid_argument;
        }
        *node = {};
        auto* scene = find_managed_tree(tree);
        if (scene == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        engine::scene_tree::scene_node* added = nullptr;
        if (parent.value == 0) {
            added = &scene->add_root();
        } else {
            auto* parent_node = scene->find_node(to_entity(parent));
            if (parent_node == nullptr) {
                return scripting::script_status::invalid_node;
            }
            added = &parent_node->add_child();
        }
        added->node_name.assign(name.data == nullptr ? "" : name.data, name.length);
        added->local = to_local_matrix(*local);
        auto const handle = to_handle(added->node_id);
        if (engine::scene_tree::components().try_emplace<placement>(to_entity(handle), placement{*local}) == nullptr) {
            scene->destroy_node(added->node_id);
            return scripting::script_status::invalid_node;
        }
        *node = handle;
        return scripting::script_status::success;
    }

    scripting::script_status __cdecl script_scene::destroy_node(std::uint64_t const tree, scripting::node_handle const node) {
        auto* scene = find_managed_tree(tree);
        if (scene == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        auto const id = to_entity(node);
        if (!id.valid()) {
            return scripting::script_status::invalid_argument;
        }
        return scene->destroy_node(id) ? scripting::script_status::success : scripting::script_status::invalid_node;
    }

    scripting::script_status __cdecl script_scene::get_node_transform(std::uint64_t const tree, scripting::node_handle const node,
                                                                      scripting::transform_value* value) {
        if (value == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        auto* scene = find_managed_tree(tree);
        if (scene == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        auto const id = to_entity(node);
        if (scene->find_node(id) == nullptr) {
            return scripting::script_status::invalid_node;
        }
        auto const* placement_value = engine::scene_tree::components().try_get<placement>(id);
        if (placement_value == nullptr) {
            return scripting::script_status::invalid_node;
        }
        *value = placement_value->transform;
        return scripting::script_status::success;
    }

    scripting::script_status __cdecl script_scene::set_node_transform(std::uint64_t const tree, scripting::node_handle const node,
                                                                      scripting::transform_value const* value) {
        if (value == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        auto* scene = find_managed_tree(tree);
        if (scene == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        auto const id = to_entity(node);
        auto* scene_node = scene->find_node(id);
        if (scene_node == nullptr) {
            return scripting::script_status::invalid_node;
        }
        if (!is_valid_pose(*value)) {
            return scripting::script_status::invalid_argument;
        }
        auto* placement_value = engine::scene_tree::components().try_get<placement>(id);
        if (placement_value == nullptr) {
            return scripting::script_status::invalid_node;
        }
        placement_value->transform = *value;
        placement_value->dirty = true;
        scene_node->local = to_local_matrix(*value);
        return scripting::script_status::success;
    }

    scripting::script_status __cdecl script_scene::set_node_visible(std::uint64_t const tree,
                                                                    scripting::node_handle const node,
                                                                    std::uint8_t const visible) {
        auto* scene = find_managed_tree(tree);
        if (scene == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        auto* scene_node = scene->find_node(to_entity(node));
        if (scene_node == nullptr) {
            return scripting::script_status::invalid_node;
        }
        scene_node->visible = visible != 0;
        return scripting::script_status::success;
    }

    scripting::script_status __cdecl script_scene::list_asset(std::uint32_t const index,
                                                              scripting::utf8_view* const id) {
        if (id == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        *id = {};
        auto const& state = assets();
        if (!state.scanned || !state.library.valid()) {
            return scripting::script_status::asset_invalid;
        }
        auto const entries = state.library.entries();
        if (index >= entries.size()) {
            return scripting::script_status::asset_not_found;
        }
        auto const& entry = entries[index];
        *id = {.data = entry.id.data(), .length = static_cast<std::uint32_t>(entry.id.size())};
        return scripting::script_status::success;
    }

    scripting::script_status __cdecl script_scene::load_asset(scripting::utf8_view const id,
                                                              scripting::asset_handle* asset) {
        if (asset == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        *asset = {};
        if (id.data == nullptr || id.length == 0) {
            return scripting::script_status::invalid_argument;
        }
        auto& state = assets();
        if (!state.scanned || !state.library.valid()) {
            return scripting::script_status::asset_invalid;
        }
        std::string_view const id_text{id.data, id.length};
        for (auto& entry : state.assets) {
            if (entry.asset_id == id_text) {
                ++entry.references;
                *asset = {.value = entry.id};
                return scripting::script_status::success;
            }
        }
        auto const* entry = state.library.find(id_text);
        if (entry == nullptr) {
            return scripting::script_status::asset_not_found;
        }
        auto loaded = gltf::load_model(entry->model.string());
        if (!loaded) {
            utility::error("scripts: could not load asset '{}' from '{}'", entry->id, entry->model.string());
            return scripting::script_status::asset_invalid;
        }
        auto model = std::make_unique<gltf::scenes>(std::move(*loaded));
        auto const bounds = gltf::compute_scene_bounds(*model);
        auto materials = gltf::resolve_materials(*model, true);
        auto const center = (bounds.min + bounds.max) * 0.5F;
        auto const radius = glm::length(bounds.max - bounds.min) * 0.5F;
        auto const handle = state.next_asset_id++;
        state.assets.push_back(managed_asset{
            .id = handle,
            .asset_id = entry->id,
            .model = std::move(model),
            .materials = std::move(materials),
            .import_shift = {-center.x, -center.y - radius, -center.z},
            .references = 1});
        *asset = {.value = handle};
        return scripting::script_status::success;
    }

    scripting::script_status __cdecl script_scene::release_asset(scripting::asset_handle const asset) {
        if (asset.value == 0) {
            return scripting::script_status::invalid_argument;
        }
        auto& list = assets().assets;
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (it->id != asset.value) {
                continue;
            }
            if (it->references == 0) {
                return scripting::script_status::asset_invalid;
            }
            if (--it->references == 0) {
                list.erase(it);
            }
            return scripting::script_status::success;
        }
        return scripting::script_status::asset_not_found;
    }

    scripting::script_status __cdecl script_scene::bind_asset(std::uint64_t const tree, scripting::node_handle const parent,
                                                              scripting::asset_handle const asset,
                                                              scripting::transform_value const* offset,
                                                              scripting::node_handle* root) {
        if (root == nullptr || offset == nullptr || !is_valid_pose(*offset)) {
            return scripting::script_status::invalid_argument;
        }
        *root = {};
        auto* scene = find_managed_tree(tree);
        if (scene == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        auto* tree_entry = find_managed_tree_entry(tree);
        bool const is_active = tree_entry != nullptr && tree_entry->active;
        auto* source = find_asset(asset.value);
        if (source == nullptr) {
            return scripting::script_status::asset_not_found;
        }

        engine::scene_tree::scene_node* wrapper = nullptr;
        if (parent.value == 0) {
            wrapper = &scene->add_root();
        } else {
            auto* parent_node = scene->find_node(to_entity(parent));
            if (parent_node == nullptr) {
                return scripting::script_status::invalid_node;
            }
            wrapper = &parent_node->add_child();
        }
        wrapper->node_name.assign(source->asset_id);
        wrapper->local = to_local_matrix(*offset);
        auto const wrapper_handle = to_handle(wrapper->node_id);
        if (engine::scene_tree::components().try_emplace<placement>(to_entity(wrapper_handle), placement{*offset}) == nullptr) {
            scene->destroy_node(wrapper->node_id);
            return scripting::script_status::invalid_node;
        }

        if (is_active) {
            auto* owner = tree_entry == nullptr ? nullptr : tree_entry->owner;
            managed_asset_source const asset_source{
                .scenes = source->model.get(),
                .materials = source->materials,
                .import_shift = source->import_shift};
            if (owner == nullptr || owner->asset_binder.bind == nullptr ||
                !owner->asset_binder.bind(owner->asset_binder.context, *wrapper, source->asset_id, asset_source)) {
                scene->destroy_node(wrapper->node_id);
                return scripting::script_status::asset_invalid;
            }
            *root = wrapper_handle;
            return scripting::script_status::success;
        }

        scripting::transform_value const identity{
            .position = {.x = 0.0f, .y = 0.0f, .z = 0.0f},
            .rotation_degrees = {.x = 0.0f, .y = 0.0f, .z = 0.0f},
            .scale = {.x = 1.0f, .y = 1.0f, .z = 1.0f}};
        auto add_node_components = [](engine::scene_tree::scene_node& node,
                                      scripting::transform_value const& pose) {
            return engine::scene_tree::components().try_emplace<placement>(node.node_id, placement{pose}) != nullptr;
        };

        for (auto const& source_scene : source->model->scene) {
            auto& scene_root = wrapper->add_child();
            scene_root.node_name.assign(source_scene.name.data(), source_scene.name.size());
            if (!add_node_components(scene_root, identity)) {
                scene->destroy_node(wrapper->node_id);
                return scripting::script_status::invalid_node;
            }
            auto append_children = [&](auto&& self, engine::scene_tree::scene_node& target,
                                       gltf::scene const& scene_value,
                                       std::size_t const index) -> bool {
                if (index >= scene_value.nodes.size()) {
                    return false;
                }
                auto const& value = scene_value.nodes[index];
                auto& node = target.add_child();
                node.node_name.assign(value.name.data(), value.name.size());
                node.local = value.local_transform;
                node.source_index = value.source_index;
                if (!add_node_components(node, pose_from_matrix(value.local_transform))) {
                    return false;
                }
                for (auto const child : value.children) {
                    if (!self(self, node, scene_value, child)) {
                        return false;
                    }
                }
                return true;
            };
            for (auto const index : source_scene.root_indices) {
                if (!append_children(append_children, scene_root, source_scene, index)) {
                    scene->destroy_node(wrapper->node_id);
                    return scripting::script_status::invalid_node;
                }
            }
        }
        *root = wrapper_handle;
        return scripting::script_status::success;
    }

    scripting::script_status __cdecl script_scene::get_transform(scripting::node_handle node, scripting::transform_value* value) {
        if (value == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        auto* placement = find(node);
        if (placement == nullptr) {
            return scripting::script_status::invalid_node;
        }
        *value = placement->transform;
        return scripting::script_status::success;
    }

    scripting::script_status __cdecl script_scene::set_transform(scripting::node_handle node, scripting::transform_value const* value) {
        if (value == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        auto* placement = find(node);
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

    scripting::script_status __cdecl script_scene::try_spawn(std::uint64_t const scene, scripting::utf8_view asset,
                                                             scripting::transform_value const* value, scripting::node_handle* node) {
        static_cast<void>(scene);
        if (asset.data == nullptr || asset.length == 0 || value == nullptr || node == nullptr ||
            !is_finite(value->position) || !is_finite(value->rotation_degrees) || !is_finite(value->scale) ||
            value->scale.x == 0.0f || value->scale.y == 0.0f || value->scale.z == 0.0f) {
            return scripting::script_status::invalid_argument;
        }
        // Spawn slots belong to the managed scene. The game creates and destroys those nodes through
        // the scene_tree API instead of asking the native project layer for a predeclared slot.
        return scripting::script_status::spawn_unavailable;
    }

    scripting::script_status __cdecl script_scene::try_despawn(std::uint64_t const scene, scripting::node_handle node) {
        static_cast<void>(scene);
        static_cast<void>(node);
        return scripting::script_status::spawn_unavailable;
    }
} // namespace deren::app
