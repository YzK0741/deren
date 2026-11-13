#include <cmath>
#include <cstdio>

import deren.scripting.managed_host;

namespace {

    using namespace deren::scripting;

    struct scene_fixture {
        enum class module_mode {
            ok,
            unavailable,
            invalid
        };

        struct tree_node {
            std::uint64_t value = 0;
            transform_value pose{};
        };

        transform_value poses[2]{};
        tree_node nodes[8]{};
        bool second_alive = true;
        unsigned errors = 0;
        unsigned spawned = 0;
        unsigned despawned = 0;
        unsigned trees_created = 0;
        unsigned trees_destroyed = 0;
        struct tree_entry {
            std::uint64_t id = 0;
            std::uint64_t previous = 0;
        };
        tree_entry tree_stack[8]{};
        unsigned tree_depth = 0;
        std::uint64_t live_tree = 0;
        unsigned nodes_created = 0;
        unsigned nodes_destroyed = 0;
        std::uint64_t active_tree = 0;
        std::uint64_t right_node = 0;
        unsigned roots = 0;
        unsigned assets_loaded = 0;
        unsigned assets_released = 0;
        unsigned assets_bound = 0;
        std::uint64_t assets[8]{};
        module_mode module = module_mode::ok;
    };

    scene_fixture* module_context = nullptr;
    bool* log_target = nullptr;

    constexpr node_handle node_zero{.value = 1};
    constexpr node_handle node_one{.value = 2};
    constexpr std::uint64_t spawned_base = 0x100;

    void __cdecl scene_log(char const* message, std::uint32_t length) {
        std::fprintf(stderr, "managed log: %.*s\n", static_cast<int>(length), message);
        if (module_context != nullptr && std::string_view{message, length}.find("on_update") != std::string_view::npos) {
            ++module_context->errors;
        }
    }

    bool is_alive(scene_fixture const& scene, node_handle node) {
        return node.value == node_zero.value || (node.value == node_one.value && scene.second_alive);
    }

    std::size_t pose_index(node_handle node) {
        return node.value == node_zero.value ? 0 : 1;
    }

    scene_fixture::tree_node* find_tree_node_raw(scene_fixture& scene, std::uint64_t const value) {
        for (auto& node : scene.nodes) {
            if (node.value != 0 && node.value == value) {
                return &node;
            }
        }
        return nullptr;
    }

    scene_fixture::tree_node* find_tree_node(scene_fixture& scene, std::uint64_t const value) {
        if (value == scene.right_node && !scene.second_alive) {
            return nullptr;
        }
        return find_tree_node_raw(scene, value);
    }

    transform_value current_pose(scene_fixture& scene, node_handle const node) {
        auto* found = find_tree_node_raw(scene, node.value);
        return found != nullptr ? found->pose : transform_value{};
    }

    bool close(float const left, float const right) {
        return std::abs(left - right) <= 0.01F;
    }

    script_status __cdecl get_pose(node_handle node, transform_value* pose) {
        auto& scene = *module_context;
        if (auto* found = find_tree_node_raw(scene, node.value); found != nullptr) {
            if (node.value == scene.right_node && !scene.second_alive) {
                return script_status::invalid_node;
            }
            *pose = found->pose;
            return script_status::success;
        }
        if (!is_alive(scene, node)) {
            return script_status::invalid_node;
        }
        *pose = scene.poses[pose_index(node)];
        return script_status::success;
    }

    script_status __cdecl set_pose(node_handle node, transform_value const* pose) {
        auto& scene = *module_context;
        if (pose == nullptr || pose->scale.x == 0.0F || pose->scale.y == 0.0F || pose->scale.z == 0.0F) {
            return script_status::invalid_argument;
        }
        if (auto* found = find_tree_node_raw(scene, node.value); found != nullptr) {
            if (node.value == scene.right_node && !scene.second_alive) {
                return script_status::invalid_node;
            }
            found->pose = *pose;
            return script_status::success;
        }
        if (!is_alive(scene, node)) {
            return script_status::invalid_node;
        }
        scene.poses[pose_index(node)] = *pose;
        return script_status::success;
    }

    script_status __cdecl spawn_helmet(std::uint64_t scene, utf8_view asset, transform_value const* pose, node_handle* node) {
        if (scene != 7) {
            return script_status::scene_not_found;
        }
        auto& state = *module_context;
        if (std::string_view{asset.data, asset.length} != "damaged-helmet" || pose->position.x != 0.0F) {
            return script_status::spawn_unavailable;
        }
        *node = {.value = spawned_base + ++state.spawned};
        return script_status::success;
    }

    script_status __cdecl despawn_helmet(std::uint64_t scene, node_handle node) {
        if (scene != 7) {
            return script_status::scene_not_found;
        }
        auto& state = *module_context;
        if (node.value != spawned_base + state.spawned || state.spawned == state.despawned) {
            return script_status::invalid_node;
        }
        ++state.despawned;
        return script_status::success;
    }

    script_status __cdecl create_tree(std::uint64_t* tree) {
        if (tree == nullptr) {
            return script_status::invalid_argument;
        }
        auto& state = *module_context;
        if (state.tree_depth >= std::size(state.tree_stack)) {
            return script_status::script_failure;
        }
        *tree = spawned_base + 0x100 + ++state.trees_created;
        state.tree_stack[state.tree_depth++] = {*tree, state.live_tree};
        state.live_tree = *tree;
        state.roots = 0;
        return script_status::success;
    }

    script_status __cdecl activate_tree(std::uint64_t tree) {
        auto& state = *module_context;
        if (tree == 0 || tree != state.live_tree) {
            return script_status::scene_not_found;
        }
        state.active_tree = tree;
        return script_status::success;
    }

    script_status __cdecl get_active_tree(std::uint64_t* tree) {
        if (tree == nullptr) {
            return script_status::invalid_argument;
        }
        *tree = module_context->active_tree;
        return script_status::success;
    }

    script_status __cdecl deactivate_tree(std::uint64_t tree) {
        auto& state = *module_context;
        if (tree == 0 || state.active_tree != tree) {
            return script_status::scene_not_found;
        }
        state.active_tree = 0;
        return script_status::success;
    }

    script_status __cdecl destroy_tree(std::uint64_t tree) {
        auto& state = *module_context;
        if (tree == 0) {
            return script_status::invalid_argument;
        }
        if (tree == state.active_tree) {
            return script_status::scene_not_found;
        }
        for (unsigned index = 0; index < state.tree_depth; ++index) {
            if (state.tree_stack[index].id != tree) {
                continue;
            }
            state.live_tree = state.tree_stack[index].previous;
            for (unsigned move = index + 1; move < state.tree_depth; ++move) {
                state.tree_stack[move - 1] = state.tree_stack[move];
            }
            state.tree_stack[--state.tree_depth] = {};
            ++state.trees_destroyed;
            return script_status::success;
        }
        return script_status::scene_not_found;
    }

    script_status __cdecl set_tree_visible(std::uint64_t tree, node_handle node, std::uint8_t visible) {
        auto& state = *module_context;
        if (tree == 0 || tree != state.live_tree) {
            return script_status::invalid_argument;
        }
        if (find_tree_node(state, node.value) == nullptr) {
            return script_status::invalid_node;
        }
        (void)visible;
        return script_status::success;
    }

    script_status __cdecl create_node(std::uint64_t tree, node_handle parent, utf8_view name,
                                      transform_value const* local, node_handle* node) {
        auto& state = *module_context;
        if (tree == 0 || local == nullptr || node == nullptr || tree != state.live_tree) {
            return script_status::invalid_argument;
        }
        if (parent.value != 0 && find_tree_node(state, parent.value) == nullptr) {
            return script_status::invalid_node;
        }
        if (state.nodes_created >= std::size(state.nodes)) {
            return script_status::invalid_node;
        }
        (void)name;
        auto& created = state.nodes[state.nodes_created];
        created.value = spawned_base + 0x200 + ++state.nodes_created;
        created.pose = *local;
        if (parent.value == 0) {
            ++state.roots;
            if (state.roots == 2) {
                state.right_node = created.value;
            }
        }
        *node = node_handle{.value = created.value};
        return script_status::success;
    }

    script_status __cdecl destroy_node(std::uint64_t tree, node_handle node) {
        auto& state = *module_context;
        if (tree == 0 || node.value == 0) {
            return script_status::invalid_argument;
        }
        if (tree != state.live_tree) {
            return script_status::invalid_node;
        }
        auto* destroyed = find_tree_node(state, node.value);
        if (destroyed == nullptr) {
            return script_status::invalid_node;
        }
        destroyed->value = 0;
        ++state.nodes_destroyed;
        return script_status::success;
    }

    script_status __cdecl get_node_pose(std::uint64_t tree, node_handle node, transform_value* pose) {
        auto& state = *module_context;
        if (tree == 0 || pose == nullptr) {
            return script_status::invalid_argument;
        }
        if (tree != state.live_tree) {
            return script_status::invalid_node;
        }
        auto* found = find_tree_node(state, node.value);
        if (found == nullptr) {
            return script_status::invalid_node;
        }
        *pose = found->pose;
        return script_status::success;
    }

    script_status __cdecl set_node_pose(std::uint64_t tree, node_handle node, transform_value const* pose) {
        auto& state = *module_context;
        if (tree == 0 || pose == nullptr) {
            return script_status::invalid_argument;
        }
        if (tree != state.live_tree) {
            return script_status::invalid_node;
        }
        auto* found = find_tree_node(state, node.value);
        if (found == nullptr) {
            return script_status::invalid_node;
        }
        found->pose = *pose;
        return script_status::success;
    }

    script_status __cdecl list_asset(std::uint32_t index, utf8_view* id) {
        if (id == nullptr) {
            return script_status::invalid_argument;
        }
        *id = {};
        if (index != 0) {
            return script_status::asset_not_found;
        }
        constexpr std::string_view name = "damaged-helmet";
        *id = {.data = name.data(), .length = static_cast<std::uint32_t>(name.size())};
        return script_status::success;
    }

    script_status __cdecl load_asset(utf8_view id, asset_handle* asset) {
        if (asset == nullptr) {
            return script_status::invalid_argument;
        }
        if (std::string_view{id.data, id.length} != "damaged-helmet") {
            return script_status::asset_not_found;
        }
        auto& state = *module_context;
        if (state.assets_loaded >= std::size(state.assets)) {
            return script_status::asset_invalid;
        }
        *asset = {.value = 0x300 + ++state.assets_loaded};
        state.assets[state.assets_loaded - 1] = asset->value;
        return script_status::success;
    }

    script_status __cdecl release_asset(asset_handle asset) {
        auto& state = *module_context;
        for (auto& value : state.assets) {
            if (value == asset.value && value != 0) {
                value = 0;
                ++state.assets_released;
                return script_status::success;
            }
        }
        return script_status::asset_not_found;
    }

    script_status __cdecl bind_asset(std::uint64_t tree, node_handle parent, asset_handle asset,
                                     transform_value const* offset, node_handle* root) {
        auto& state = *module_context;
        (void)parent;
        if (tree == 0 || offset == nullptr || root == nullptr) {
            return script_status::invalid_argument;
        }
        if (tree != state.live_tree) {
            return script_status::invalid_node;
        }
        bool known = false;
        for (auto const value : state.assets) {
            known = known || value == asset.value;
        }
        if (!known) {
            return script_status::asset_not_found;
        }
        *root = {.value = 0x400 + ++state.assets_bound};
        return script_status::success;
    }

    script_status __cdecl get_scene_module(std::uint32_t module, void const** table) {
        if (table == nullptr) {
            return script_status::invalid_argument;
        }
        *table = nullptr;
        if (module != static_cast<std::uint32_t>(native_module::scene)) {
            return script_status::module_unavailable;
        }
        auto const mode = module_context != nullptr ? module_context->module : scene_fixture::module_mode::ok;
        if (mode == scene_fixture::module_mode::unavailable) {
            return script_status::module_unavailable;
        }
        if (mode == scene_fixture::module_mode::invalid) {
            return script_status::success;
        }
        static scene_api const published{
            .get_transform = get_pose,
            .set_transform = set_pose,
            .try_spawn = spawn_helmet,
            .try_despawn = despawn_helmet,
            .create_scene_tree = create_tree,
            .activate_scene = activate_tree,
            .get_active_scene = get_active_tree,
            .deactivate_scene = deactivate_tree,
            .destroy_scene_tree = destroy_tree,
            .create_node = create_node,
            .destroy_node = destroy_node,
            .get_node_transform = get_node_pose,
            .set_node_transform = set_node_pose,
            .set_node_visible = set_tree_visible,
            .list_asset = list_asset,
            .load_asset = load_asset,
            .release_asset = release_asset,
            .bind_asset = bind_asset};
        *table = &published;
        return script_status::success;
    }

    script_status update_scene(managed_api& scripts, std::uint64_t scene, float delta_time,
                               std::uint8_t const* input = nullptr, std::uint32_t input_count = 0) {
        frame_info const frame{
            .scene = scene,
            .delta_time = delta_time,
            .input_count = input_count,
            .input = input};
        return scripts.update(&frame);
    }

    bool run_scene(host_paths const& paths, bool test_input) {
        scene_fixture scene;
        module_context = &scene;
        native_api const api{.log = scene_log, .get_module = get_scene_module};
        managed_api scripts;
        if (initialize_managed_host(paths, api, scripts).status != host_status::success) {
            std::fputs("startup: managed host initialization failed\n", stderr);
            return false;
        }
        if (scene.active_tree == 0 || scene.trees_created != 1 || scene.nodes_created != 3 ||
            scene.assets_loaded != 1 || scene.assets_bound != 3 || scene.assets_released != 1) {
            std::fprintf(stderr, "startup: tree=%llu trees=%u nodes=%u assets=%u/%u/%u\n",
                         static_cast<unsigned long long>(scene.active_tree), scene.trees_created, scene.nodes_created,
                         scene.assets_loaded, scene.assets_bound, scene.assets_released);
            return false;
        }
        if (scripts.update(nullptr) != script_status::invalid_argument) {
            std::fputs("frame: null frame accepted\n", stderr);
            return false;
        }
        if (test_input) {
            std::uint8_t const pressed[]{0, 3, 3, 3};
            if (update_scene(scripts, 7, 0.5F, pressed, 4) != script_status::success) {
                std::fputs("frame: input update failed\n", stderr);
                return false;
            }
        } else if (update_scene(scripts, 7, 0.5F) != script_status::success) {
            std::fputs("frame: update failed\n", stderr);
            return false;
        }
        if (scripts.stop() != script_status::success || scripts.stop() != script_status::success) {
            std::fputs("shutdown: managed stop failed\n", stderr);
            return false;
        }
        if (scene.active_tree != 0 || scene.live_tree != 0 || scene.trees_destroyed != 1 ||
            scripts.update(nullptr) != script_status::invalid_argument) {
            std::fprintf(stderr, "shutdown: tree=%llu live=%llu destroyed=%u\n",
                         static_cast<unsigned long long>(scene.active_tree),
                         static_cast<unsigned long long>(scene.live_tree), scene.trees_destroyed);
            return false;
        }
        return true;
    }

    void __cdecl log_message(char const* message, std::uint32_t length) {
        *log_target = std::string_view{message, length} == "deren managed host ready";
    }

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        return 2;
    }

    auto const scenario = std::string_view{argv[2]};
    auto const bridge_directory = std::filesystem::path{argv[1]};
    auto paths = deren::scripting::host_paths{
        .assembly = bridge_directory / "Deren.Managed.dll",
        .runtime_config = bridge_directory / "Deren.Managed.runtimeconfig.json",
        .nethost = bridge_directory / "nethost.dll",
        .game_assembly = std::filesystem::path{argv[3]}};
    if (scenario == "scene_lifecycle" || scenario == "scene_input") {
        if (!run_scene(paths, scenario == "scene_input")) {
            std::fputs("Managed scene lifecycle or transform round trip failed\n", stderr);
            return 1;
        }
        return 0;
    }
    auto expected = host_status::success;
    auto abi_version = deren::scripting::managed_abi_version;
    bool received = false;
    log_target = &received;
    auto api = deren::scripting::native_api{.log = log_message, .get_module = get_scene_module};
    managed_api scripts;
    static scene_fixture round_trip_fixture;
    if (scenario == "round_trip") {
        module_context = &round_trip_fixture;
    }

    if (scenario == "missing_assembly") {
        paths.assembly.replace_filename("missing.dll");
        expected = host_status::assembly_missing;
    } else if (scenario == "missing_game_assembly") {
        paths.game_assembly = bridge_directory / "missing-game.dll";
        expected = host_status::game_assembly_missing;
    } else if (scenario == "missing_runtime") {
        paths.dotnet_root = bridge_directory / "missing_dotnet";
        expected = host_status::runtime_discovery_failed;
    } else if (scenario == "abi_mismatch") {
        abi_version = deren::scripting::managed_abi_version + 1u;
        expected = host_status::abi_mismatch;
    } else if (scenario == "scene_module_unavailable" || scenario == "scene_module_invalid") {
        static scene_fixture module_fixture;
        module_context = &module_fixture;
        if (scenario == "scene_module_unavailable") {
            module_fixture.module = scene_fixture::module_mode::unavailable;
            expected = host_status::scene_module_unavailable;
        } else {
            module_fixture.module = scene_fixture::module_mode::invalid;
            expected = host_status::scene_module_invalid;
        }
    } else if (scenario != "round_trip") {
        return 2;
    }

    auto const result = deren::scripting::initialize_managed_host(paths, api, scripts, abi_version);
    if (result.status != expected || received != (expected == host_status::success)) {
        std::fprintf(stderr, "%s: expected status %d, got %d (detail 0x%08x), callback %d\n",
                     argv[2], static_cast<int>(expected), static_cast<int>(result.status),
                     static_cast<unsigned>(result.detail), received);
        return 1;
    }
}
