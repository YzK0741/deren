#include <cstdio>

import deren.scripting.managed_host;

namespace {

    using namespace deren::scripting;

    struct scene_fixture {
        transform_value poses[2]{};
        bool second_alive = true;
        unsigned errors = 0;
    };

    void __cdecl scene_log(void* context, char const* message, std::uint32_t length) {
        if (std::string_view{message, length}.find("on_update") != std::string_view::npos) {
            ++static_cast<scene_fixture*>(context)->errors;
        }
    }

    bool is_alive(scene_fixture const& scene, entity_handle entity) {
        return entity.scene == 7 && entity.generation == 1 &&
               (entity.index == 0 || (entity.index == 1 && scene.second_alive));
    }

    script_status __cdecl get_pose(void* context, entity_handle entity, transform_value* pose) {
        auto const& scene = *static_cast<scene_fixture*>(context);
        if (!is_alive(scene, entity)) {
            return script_status::invalid_entity;
        }
        *pose = scene.poses[entity.index];
        return script_status::success;
    }

    script_status __cdecl set_pose(void* context, entity_handle entity, transform_value const* pose) {
        auto& scene = *static_cast<scene_fixture*>(context);
        if (!is_alive(scene, entity)) {
            return script_status::invalid_entity;
        }
        scene.poses[entity.index] = *pose;
        return script_status::success;
    }

    bool run_scene(host_paths const& paths, std::filesystem::path const& game_path) {
        scene_fixture scene;
        scene.poses[0].position.x = -1.0F;
        scene.poses[1].position.x = 1.0F;
        native_api const api{.context = &scene, .log = scene_log, .get_transform = get_pose, .set_transform = set_pose};
        managed_api scripts;
        if (initialize_managed_host(paths, api, scripts).status != host_status::success) {
            return false;
        }

        auto const assembly = game_path.u8string();
        utf8_view const path{reinterpret_cast<char const*>(assembly.data()), static_cast<std::uint32_t>(assembly.size())};
        script_binding const bindings[]{{{7, 0, 1}, {"game.rotate", 11}}, {{7, 1, 1}, {"game.rotate", 11}}};
        script_binding const invalid_binding{{7, 0, 1}, {"game.missing", 12}};
        if (scripts.start_scene(&api, 7, path, &invalid_binding, 1) != script_status::script_failure ||
            scripts.start_scene(&api, 7, path, bindings, 2) != script_status::success ||
            scripts.start_scene(&api, 7, path, bindings, 2) != script_status::scene_exists ||
            scripts.update_scene(7, 0.5F) != script_status::success ||
            scene.poses[0].rotation_degrees.y != 15.0F || scene.poses[1].rotation_degrees.y != 30.0F) {
            return false;
        }

        scene.second_alive = false;
        if (scripts.update_scene(7, 0.5F) != script_status::success ||
            scripts.update_scene(7, 0.5F) != script_status::success ||
            scene.poses[0].rotation_degrees.y != 45.0F || scene.poses[1].rotation_degrees.y != 30.0F ||
            scene.errors != 1) {
            return false;
        }

        scene.second_alive = true;
        return scripts.update_scene(7, -0.5F) == script_status::success &&
               scene.poses[0].rotation_degrees.y == 30.0F && scene.poses[1].rotation_degrees.y == 30.0F &&
               scripts.stop_scene(7) == script_status::success &&
               scene.poses[0].position.z == 1.0F && scene.poses[1].position.z == 1.0F &&
               scripts.update_scene(7, 0.5F) == script_status::scene_not_found &&
               scripts.stop_scene(7) == script_status::scene_not_found;
    }

    void __cdecl log_message(void* context, char const* message, std::uint32_t length) {
        auto& received = *static_cast<bool*>(context);
        received = std::string_view{message, length} == "deren managed host ready";
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
        .nethost = bridge_directory / "nethost.dll"};
    if (scenario == "scene_lifecycle") {
        if (!run_scene(paths, std::filesystem::path{argv[3]})) {
            std::fputs("Managed scene lifecycle or transform round trip failed\n", stderr);
            return 1;
        }
        return 0;
    }
    auto expected = host_status::success;
    bool received = false;
    auto api = deren::scripting::native_api{.context = &received, .log = log_message, .get_transform = get_pose, .set_transform = set_pose};
    managed_api scripts;

    if (scenario == "missing_assembly") {
        paths.assembly.replace_filename("missing.dll");
        expected = host_status::assembly_missing;
    } else if (scenario == "missing_runtime") {
        paths.dotnet_root = bridge_directory / "missing_dotnet";
        expected = host_status::runtime_discovery_failed;
    } else if (scenario == "abi_mismatch") {
        ++api.version;
        expected = host_status::abi_mismatch;
    } else if (scenario == "abi_table_too_small") {
        api.size = sizeof(api.size) + sizeof(api.version);
        expected = host_status::abi_mismatch;
    } else if (scenario == "managed_table_too_small") {
        scripts.size = sizeof(scripts.size) + sizeof(scripts.version);
        expected = host_status::abi_mismatch;
    } else if (scenario != "round_trip") {
        return 2;
    }

    auto const result = deren::scripting::initialize_managed_host(paths, api, scripts);
    if (result.status != expected || received != (expected == host_status::success)) {
        std::fprintf(stderr, "%s: expected status %d, got %d (detail 0x%08x), callback %d\n",
                     argv[2], static_cast<int>(expected), static_cast<int>(result.status),
                     static_cast<unsigned>(result.detail), received);
        return 1;
    }

    if (scenario == "round_trip") {
        received = false;
        auto const repeated_result = deren::scripting::initialize_managed_host(paths, api, scripts);
        if (repeated_result.status != host_status::success || !received) {
            std::fprintf(stderr, "second initialization failed (status %d, detail 0x%08x)\n",
                         static_cast<int>(repeated_result.status), static_cast<unsigned>(repeated_result.detail));
            return 1;
        }
    }
}
