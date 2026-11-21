#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

import deren.scripting.managed_host;
import deren.scripting.audio_host;
import deren.ui.rml_scripting;

extern "C" bool deren_audio_test_no_device();
extern "C" bool deren_audio_test_read_frames(float* output, std::uint64_t samples);

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
            // ABI 40: the name the node was created with, then possibly renamed to. The name lives in
            // the node itself, exactly like the engine's, so a readback borrows these bytes.
            char name[256] = {};
            std::uint32_t name_length = 0;
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
        camera_state camera{};
        unsigned roots = 0;
        unsigned assets_loaded = 0;
        unsigned assets_released = 0;
        unsigned assets_bound = 0;
        std::uint64_t assets[8]{};
        module_mode module = module_mode::ok;
        // ABI 33: how many bytes to add to the native_api entry the layout answer reports, so a
        // scenario can prove the managed side refuses a layout it does not understand.
        std::uint32_t abi_size_delta = 0;
    };

    scene_fixture* module_context = nullptr;
    int audio_module_mode = 0;
    int rml_module_mode = 0;

    bool* log_target = nullptr;
    std::uint32_t screenshot_requests = 0;
    std::uint32_t environment_requests = 0;
    bool environment_path_is_empty = false;
    std::uint32_t environment_slot = ~0u;    // the slot the last environment request named
    std::uint32_t environment_slot_mask = 0; // one bit per slot any request named
    std::uint32_t pipeline_queries = 0;
    std::uint32_t pipeline_assignments = 0;
    std::uint32_t post_processing_requests = 0;
    std::uint32_t post_processing_rejections = 0;
    std::uint32_t lighting_requests = 0;
    std::uint32_t lighting_rejections = 0;
    // ABI 26: the punctual light set. `engine_light_capacity` mirrors the host's own cap so the
    // fixture rejects an over-long rig exactly where the host does.
    // ABI 27: per-object material factors.
    std::uint32_t material_override_requests = 0;
    std::uint32_t material_override_rejections = 0;
    material_override_desc material_override_last{};
    std::uint32_t light_rig_requests = 0;
    std::uint32_t light_rig_rejections = 0;
    std::uint32_t light_rig_count = 0;
    constexpr std::uint32_t engine_light_capacity = 128;
    // ABI 31: the debug panel and the raw input stream. The demo describes a panel every frame, so the
    // fixture counts the verbs instead of checking one call: a demo that stopped building its panel
    // would otherwise look like a host with a perfect panel table.
    std::uint32_t ui_panels = 0;
    std::uint32_t ui_texts = 0;
    std::uint32_t ui_checkboxes = 0;
    std::uint32_t ui_sliders = 0;
    std::uint32_t ui_vec3s = 0;
    std::uint32_t ui_combos = 0;
    std::uint32_t ui_buttons = 0;
    std::uint32_t ui_wants_mouse_calls = 0;
    std::uint32_t ui_rejections = 0;
    constexpr std::uint32_t ui_combo_capacity = 64;
    // The input stream: registration counter and the last sink the managed side installed.
    std::uint32_t input_registrations = 0;
    // The handler the managed side registered, so the test can hand it an event of its own.
    void (*input_callback)(void* context, input_event const* event) = nullptr;
    void* input_context = nullptr;
    // The last line the managed side logged: how the test notices a handler ran.
    std::string last_log;
    std::uint32_t input_clears = 0;
    bool input_sink_installed = false;
    bool radiance_outstanding = false;
    // ABI 29: the face measurement and the published head frame. The helmet scenario asks for neither
    // (only a `game_toon` character drives them), so the assertions below keep them at zero there.
    std::uint32_t face_frame_queries = 0;
    std::uint32_t head_basis_requests = 0;
    std::uint32_t head_basis_rejections = 0;
    constexpr std::uint64_t face_frame_asset = 1;
    constexpr std::uint32_t head_slot_capacity = 16;
    std::uint32_t head_basis_slot = 0;
    bool head_basis_captured = false;
    // ABI 30: the offscreen reflection-probe capture. The helmet scenario captures nothing, so the
    // assertions below keep every entry untouched; the counters exist so a game that started a capture
    // without a probe request is visible here instead of only in a picture.
    std::uint32_t capture_camera_calls = 0;
    std::uint32_t radiance_requests = 0;
    std::uint32_t radiance_reads = 0;
    std::uint32_t temporal_resets = 0;
    std::uint32_t cube_writes = 0;
    std::uint32_t capture_rejections = 0;
    constexpr std::uint32_t capture_face_capacity = 6;
    // ABI 35: the animation player. The helmet demo names no motion, so the player must stay absent
    // there; the animated-asset scenario asserts that the demo selected a clip and started its clock.
    std::uint32_t animation_queries = 0;
    std::uint32_t animation_name_queries = 0;
    std::uint32_t animation_selects = 0;
    std::uint32_t animation_play_requests = 0;
    std::uint32_t animation_speed_requests = 0;
    std::uint32_t animation_time_requests = 0;
    std::uint32_t animation_last_select = ~0u;
    std::uint8_t animation_last_playing = 0;
    float animation_last_speed = 0.0F;
    // Off in the helmet scenario: an asset that names no motion must leave the player absent.
    bool animation_available = false;
    // The clip the fixture pretends the player holds; the names are reported from here.
    constexpr std::uint32_t animation_clip_count = 2;
    constexpr char const* animation_clip_names[animation_clip_count] = {"idle", "bibideba"};
    // ABI 40: renaming a node. The self check performs one accepted rename, one rejection of its own
    // (the over-long name; the empty one never leaves the C# wrapper), one stale handle and two name
    // reads, so a wrapper that stopped forwarding any of them shows up here.
    std::uint32_t node_name_renames = 0;
    std::uint32_t node_name_rejections = 0;
    std::uint32_t node_name_misses = 0;
    std::uint32_t node_name_reads = 0;
    // ABI 41: the projection aspect. The self check accepts a box aspect, restores the window aspect
    // with zero, and has a negative value and NaN refused, so forwarding and host-side validation are
    // both visible here.
    std::uint32_t aspect_sets = 0;
    std::uint32_t aspect_rejections = 0;
    std::uint32_t aspect_last_reset = 0;
    float aspect_last = 0.0F;

    constexpr node_handle node_zero{.value = 1};
    constexpr node_handle node_one{.value = 2};
    constexpr std::uint64_t spawned_base = 0x100;

    void scene_log(char const* message, std::uint32_t length) {
        std::fprintf(stderr, "managed log: %.*s\n", static_cast<int>(length), message);
        last_log.assign(message, length);
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

    script_status get_pose(node_handle node, transform_value* pose) {
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

    script_status set_pose(node_handle node, transform_value const* pose) {
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

    script_status spawn_helmet(std::uint64_t scene, utf8_view asset, transform_value const* pose, node_handle* node) {
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

    script_status despawn_helmet(std::uint64_t scene, node_handle node) {
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

    script_status create_tree(std::uint64_t* tree) {
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

    script_status activate_tree(std::uint64_t tree) {
        auto& state = *module_context;
        if (tree == 0 || tree != state.live_tree) {
            return script_status::scene_not_found;
        }
        state.active_tree = tree;
        return script_status::success;
    }

    script_status get_active_tree(std::uint64_t* tree) {
        if (tree == nullptr) {
            return script_status::invalid_argument;
        }
        *tree = module_context->active_tree;
        return script_status::success;
    }

    script_status deactivate_tree(std::uint64_t tree) {
        auto& state = *module_context;
        if (tree == 0 || state.active_tree != tree) {
            return script_status::scene_not_found;
        }
        state.active_tree = 0;
        return script_status::success;
    }

    script_status get_camera(camera_state* value) {
        if (value == nullptr) {
            return script_status::invalid_argument;
        }
        *value = module_context->camera;
        return script_status::success;
    }

    script_status set_camera(camera_state const* value) {
        if (value == nullptr) {
            return script_status::invalid_argument;
        }
        module_context->camera = *value;
        return script_status::success;
    }

    script_status destroy_tree(std::uint64_t tree) {
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

    script_status set_tree_visible(std::uint64_t tree, node_handle node, std::uint8_t visible) {
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

    script_status create_node(std::uint64_t tree, node_handle parent, utf8_view name,
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
        auto& created = state.nodes[state.nodes_created];
        created.value = spawned_base + 0x200 + ++state.nodes_created;
        created.pose = *local;
        auto const name_length = name.data == nullptr ? 0u : std::min<std::uint32_t>(name.length, 256u);
        if (name_length != 0) {
            std::memcpy(created.name, name.data, name_length);
        }
        created.name_length = name_length;
        if (parent.value == 0) {
            ++state.roots;
            if (state.roots == 2) {
                state.right_node = created.value;
            }
        }
        *node = node_handle{.value = created.value};
        return script_status::success;
    }

    script_status destroy_node(std::uint64_t tree, node_handle node) {
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

    // ABI 38/40: the name readback and the rename. The fixture answers both against the node's own
    // storage, so the self check can prove that what a rename stored is what the next read returns.
    script_status node_name(std::uint64_t tree, node_handle node, utf8_view* name) {
        auto& state = *module_context;
        if (name == nullptr) {
            return script_status::invalid_argument;
        }
        if (tree == 0 || tree != state.live_tree) {
            return script_status::scene_not_found;
        }
        auto* found = find_tree_node(state, node.value);
        if (found == nullptr) {
            return script_status::invalid_node;
        }
        ++node_name_reads;
        *name = {.data = found->name, .length = found->name_length};
        return script_status::success;
    }

    script_status set_node_name(std::uint64_t tree, node_handle node, utf8_view name) {
        auto& state = *module_context;
        if (name.data == nullptr || name.length == 0 || name.length > 256u ||
            std::string_view{name.data, name.length}.find('\0') != std::string_view::npos) {
            ++node_name_rejections;
            return script_status::invalid_argument;
        }
        if (tree == 0 || tree != state.live_tree) {
            return script_status::scene_not_found;
        }
        auto* found = find_tree_node(state, node.value);
        if (found == nullptr) {
            ++node_name_misses;
            return script_status::invalid_node;
        }
        std::memcpy(found->name, name.data, name.length);
        found->name_length = name.length;
        ++node_name_renames;
        return script_status::success;
    }

    script_status set_scene_aspect(float aspect) {
        if (aspect != 0.0F && (!std::isfinite(aspect) || aspect <= 0.0F)) {
            ++aspect_rejections;
            return script_status::invalid_argument;
        }
        ++aspect_sets;
        aspect_last = aspect;
        aspect_last_reset = aspect == 0.0F ? 1u : 0u;
        return script_status::success;
    }

    script_status get_node_pose(std::uint64_t tree, node_handle node, transform_value* pose) {
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

    script_status set_node_pose(std::uint64_t tree, node_handle node, transform_value const* pose) {
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

    script_status list_asset(std::uint32_t index, utf8_view* id) {
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

    script_status load_asset(utf8_view id, asset_handle* asset) {
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

    script_status release_asset(asset_handle asset) {
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

    script_status bind_asset(std::uint64_t tree, node_handle parent, asset_handle asset,
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

    script_status set_environment_hdr(utf8_view path, std::uint32_t requested_slot);

    // ABI 39: the toon light rig, published on the scene table below.
    script_status set_toon_rig(toon_rig_desc const* value);
    script_status get_toon_rig(toon_rig_desc* value);

    // ABI 35: the fake animation player, over the counters declared beside the other fixtures.
    script_status get_animation_state(animation_state* out) {
        ++animation_queries;
        if (out == nullptr) {
            return script_status::invalid_argument;
        }
        if (!animation_available) {
            return script_status::module_unavailable;
        }
        *out = {};
        out->playable_count = animation_clip_count;
        out->current = animation_last_select == ~0u ? 0u : animation_last_select;
        out->has_active = 1;
        out->playing = animation_last_playing;
        out->available = 1;
        out->time = 0.5F;
        out->duration = 2.0F;
        out->speed = animation_last_speed;
        return script_status::success;
    }

    script_status animation_playable_count(std::uint32_t* count) {
        if (count == nullptr) {
            return script_status::invalid_argument;
        }
        *count = 0;
        if (!animation_available) {
            return script_status::module_unavailable;
        }
        *count = animation_clip_count;
        return script_status::success;
    }

    script_status animation_playable_name(std::uint32_t index, utf8_view* name) {
        if (name == nullptr) {
            return script_status::invalid_argument;
        }
        *name = {};
        ++animation_name_queries;
        if (!animation_available) {
            return script_status::module_unavailable;
        }
        if (index >= animation_clip_count) {
            return script_status::invalid_argument;
        }
        *name = {.data = animation_clip_names[index], .length = static_cast<std::uint32_t>(std::strlen(animation_clip_names[index]))};
        return script_status::success;
    }

    script_status select_animation(std::uint32_t index) {
        ++animation_selects;
        animation_last_select = index;
        if (!animation_available) {
            return script_status::module_unavailable;
        }
        return index < animation_clip_count ? script_status::success : script_status::invalid_argument;
    }

    script_status set_animation_playing(std::uint8_t playing) {
        ++animation_play_requests;
        animation_last_playing = playing;
        if (!animation_available) {
            return script_status::module_unavailable;
        }
        return playing <= 1 ? script_status::success : script_status::invalid_argument;
    }

    script_status set_animation_speed(float speed) {
        ++animation_speed_requests;
        animation_last_speed = speed;
        if (!animation_available) {
            return script_status::module_unavailable;
        }
        return script_status::success;
    }

    script_status set_animation_time(float seconds) {
        ++animation_time_requests;
        if (!animation_available) {
            return script_status::module_unavailable;
        }
        return seconds >= 0.0F ? script_status::success : script_status::invalid_argument;
    }

    bool known_asset(asset_handle asset) {
        if (module_context == nullptr) {
            return false;
        }
        for (auto const value : module_context->assets) {
            if (value == asset.value && value != 0) {
                return true;
            }
        }
        return false;
    }

    script_status asset_pipeline_count(asset_handle asset, std::uint32_t* count) {
        if (count == nullptr) {
            return script_status::invalid_argument;
        }
        *count = 0;
        if (!known_asset(asset)) {
            return script_status::asset_not_found;
        }
        ++pipeline_queries;
        // the demo asset declares only the derived default
        *count = 1;
        return script_status::success;
    }

    script_status asset_pipeline(asset_handle asset, std::uint32_t index, utf8_view* name) {
        if (name == nullptr) {
            return script_status::invalid_argument;
        }
        *name = {};
        if (!known_asset(asset)) {
            return script_status::asset_not_found;
        }
        if (index != 0) {
            return script_status::asset_not_found;
        }
        ++pipeline_queries;
        constexpr std::string_view pipeline = "pbr";
        *name = {.data = pipeline.data(), .length = static_cast<std::uint32_t>(pipeline.size())};
        return script_status::success;
    }

    script_status asset_default_pipeline(asset_handle asset, utf8_view* name) {
        if (name == nullptr) {
            return script_status::invalid_argument;
        }
        *name = {};
        if (!known_asset(asset)) {
            return script_status::asset_not_found;
        }
        ++pipeline_queries;
        constexpr std::string_view pipeline = "pbr";
        *name = {.data = pipeline.data(), .length = static_cast<std::uint32_t>(pipeline.size())};
        return script_status::success;
    }

    script_status set_pipeline(std::uint64_t tree, node_handle node, utf8_view pipeline) {
        if (module_context == nullptr) {
            return script_status::scene_not_found;
        }
        if (tree != module_context->active_tree || tree == 0) {
            return script_status::scene_not_found;
        }
        if (node.value < 0x400 || node.value > 0x400 + module_context->assets_bound) {
            return script_status::invalid_node;
        }
        if (std::string_view{pipeline.data, pipeline.length} != "pbr") {
            return script_status::invalid_argument;
        }
        ++pipeline_assignments;
        return script_status::success;
    }

    bool in_range(float const value, float const low, float const high) {
        return std::isfinite(value) && value >= low && value <= high;
    }

    // The nine runtime post-processing knobs. The real host validates in app_scripting before
    // forwarding; the fixture keeps the same range checks so the game's out-of-range call is
    // rejected here too, and counts the calls that were accepted.
    script_status set_exposure(float exposure) {
        if (!in_range(exposure, 0.05F, 20.0F)) {
            ++post_processing_rejections;
            return script_status::invalid_argument;
        }
        ++post_processing_requests;
        return script_status::success;
    }

    script_status set_tone_mapping(std::int32_t mode) {
        if (mode < 0 || mode > 2) {
            ++post_processing_rejections;
            return script_status::invalid_argument;
        }
        ++post_processing_requests;
        return script_status::success;
    }

    script_status set_environment(float rotation_degrees, float strength, std::int32_t background, float color_r,
                                  float color_g, float color_b, float blur) {
        if (!std::isfinite(rotation_degrees) || !in_range(strength, 0.0F, 64.0F) || background < 0 ||
            background > 3 || !in_range(color_r, 0.0F, 1.0F) || !in_range(color_g, 0.0F, 1.0F) ||
            !in_range(color_b, 0.0F, 1.0F) || !in_range(blur, 0.0F, 1.0F)) {
            ++post_processing_rejections;
            return script_status::invalid_argument;
        }
        ++post_processing_requests;
        return script_status::success;
    }

    script_status set_sun_intensity(float scale) {
        if (!in_range(scale, 0.0F, 3.0F)) {
            ++post_processing_rejections;
            return script_status::invalid_argument;
        }
        ++post_processing_requests;
        return script_status::success;
    }

    script_status set_bloom(float intensity, float threshold) {
        if (!in_range(intensity, 0.0F, 4.0F) || !in_range(threshold, 0.0F, 0.75F)) {
            ++post_processing_rejections;
            return script_status::invalid_argument;
        }
        ++post_processing_requests;
        return script_status::success;
    }

    script_status set_fxaa(std::uint8_t enabled, float subpixel, float edge_threshold) {
        if (enabled > 1 || !in_range(subpixel, 0.0F, 1.0F) || !in_range(edge_threshold, 0.0F, 1.0F)) {
            ++post_processing_rejections;
            return script_status::invalid_argument;
        }
        ++post_processing_requests;
        return script_status::success;
    }

    script_status set_taa(std::uint8_t enabled, float blend_static, float blend_min) {
        if (enabled > 1 || !in_range(blend_static, 0.0F, 0.99F) || blend_min < 0.0F ||
            blend_min > blend_static) {
            ++post_processing_rejections;
            return script_status::invalid_argument;
        }
        ++post_processing_requests;
        return script_status::success;
    }

    script_status set_ssao(std::uint8_t enabled, float radius, float intensity, std::uint32_t samples) {
        if (enabled > 1 || !std::isfinite(radius) || radius < 0.0F || !in_range(intensity, 0.0F, 1.0F) ||
            samples > 16) {
            ++post_processing_rejections;
            return script_status::invalid_argument;
        }
        ++post_processing_requests;
        return script_status::success;
    }

    script_status set_gbuffer_debug(std::uint8_t enabled) {
        if (enabled > 1) {
            ++post_processing_rejections;
            return script_status::invalid_argument;
        }
        ++post_processing_requests;
        return script_status::success;
    }

    // The three runtime lighting knobs. The real host validates in app_scripting before forwarding;
    // the fixture keeps the same checks so the game's invalid direction is rejected here too.
    script_status set_sun_direction(float x, float y, float z) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || x * x + y * y + z * z <= 1e-12F) {
            ++lighting_rejections;
            return script_status::invalid_argument;
        }
        ++lighting_requests;
        return script_status::success;
    }

    script_status set_area_light(float centre_x, float centre_y, float centre_z, float half, std::uint8_t irradiance,
                                 float axis_x, float axis_y, float axis_z, float penumbra) {
        if (!std::isfinite(centre_x) || !std::isfinite(centre_y) || !std::isfinite(centre_z) ||
            !std::isfinite(half) || !std::isfinite(penumbra) || penumbra < 0.0F || irradiance > 1 ||
            (half > 0.0F && (!std::isfinite(axis_x) || !std::isfinite(axis_y) || !std::isfinite(axis_z) ||
                             axis_x * axis_x + axis_y * axis_y + axis_z * axis_z <= 1e-12F))) {
            ++lighting_rejections;
            return script_status::invalid_argument;
        }
        ++lighting_requests;
        return script_status::success;
    }

    script_status set_clustered_lights(std::uint8_t enabled) {
        if (enabled > 1) {
            ++lighting_rejections;
            return script_status::invalid_argument;
        }
        ++lighting_requests;
        return script_status::success;
    }

    /// ABI 26: the punctual light set. The fixture validates the same things the host does, so a
    /// managed caller that sent a bad record would be reported here instead of reaching a renderer.
    /// ABI 30: the capture camera. `enabled == 0` clears it; a live camera needs a position, a face in
    /// 0..5, a positive near plane, and a far plane beyond it.
    script_status set_capture_camera(std::uint8_t enabled, float const position[3], std::uint32_t face,
                                     float near_plane, float far_plane) {
        ++capture_camera_calls;
        if (enabled > 1) {
            ++capture_rejections;
            return script_status::invalid_argument;
        }
        if (enabled == 0) {
            return script_status::success;
        }
        if (position == nullptr || face >= capture_face_capacity || !std::isfinite(position[0]) ||
            !std::isfinite(position[1]) || !std::isfinite(position[2]) || !(near_plane > 0.0F) ||
            !(far_plane > near_plane)) {
            ++capture_rejections;
            return script_status::invalid_argument;
        }
        return script_status::success;
    }

    /// ABI 30: one outstanding radiance request at a time.
    script_status request_radiance_capture() {
        ++radiance_requests;
        if (radiance_outstanding) {
            ++capture_rejections;
            return script_status::invalid_argument;
        }
        radiance_outstanding = true;
        return script_status::success;
    }

    /// ABI 30: the readback of the requested face, as canonicalized RGBA floats.
    script_status acquire_radiance_image(std::uint32_t* width, std::uint32_t* height, float* rgba,
                                         std::uint32_t float_count) {
        ++radiance_reads;
        if (width == nullptr || height == nullptr || rgba == nullptr || !radiance_outstanding) {
            ++capture_rejections;
            return script_status::invalid_argument;
        }
        constexpr std::uint32_t face_side = 16;
        if (float_count != face_side * face_side * 4) {
            ++capture_rejections;
            return script_status::invalid_argument;
        }
        for (std::uint32_t index = 0; index < float_count; ++index) {
            rgba[index] = index % 4 == 3 ? 1.0F : 0.25F;
        }
        *width = face_side;
        *height = face_side;
        radiance_outstanding = false;
        return script_status::success;
    }

    /// ABI 30: a relight-independent helper the capture loop calls between faces.
    script_status reset_temporal_history() {
        ++temporal_resets;
        return script_status::success;
    }

    /// ABI 30: the cube file. The fixture validates the same geometry and value ranges the writer does.
    script_status write_radiance_cube(utf8_view path, std::uint32_t size, float const* pixels,
                                      std::uint32_t float_count) {
        ++cube_writes;
        if (path.data == nullptr || path.length == 0 || pixels == nullptr || size < 16 || size > 512 ||
            (size & (size - 1)) != 0 || float_count != 6u * size * size * 4) {
            ++capture_rejections;
            return script_status::invalid_argument;
        }
        for (std::uint32_t index = 0; index < float_count; ++index) {
            if (!std::isfinite(pixels[index]) || pixels[index] < 0.0F || pixels[index] > 65504.0F ||
                (index % 4 == 3 && pixels[index] != 1.0F)) {
                ++capture_rejections;
                return script_status::invalid_argument;
            }
        }
        return script_status::success;
    }

    script_status set_point_lights(point_light_desc const* lights, std::uint32_t count) {
        if (count > engine_light_capacity || (count != 0 && lights == nullptr)) {
            ++light_rig_rejections;
            return script_status::invalid_argument;
        }
        for (std::uint32_t index = 0; index < count; ++index) {
            auto const& record = lights[index];
            if (!std::isfinite(record.position[0]) || !std::isfinite(record.position[1]) ||
                !std::isfinite(record.position[2]) || !std::isfinite(record.intensity) ||
                !std::isfinite(record.range) || record.range <= 0.0F || !std::isfinite(record.outer_cos) ||
                !std::isfinite(record.inner_cos) || !std::isfinite(record.npr_mode) || record.spot > 1 ||
                record.has_inner_cos > 1) {
                ++light_rig_rejections;
                return script_status::invalid_argument;
            }
        }
        ++light_rig_requests;
        light_rig_count = count;
        return script_status::success;
    }

    /// ABI 28: frame pacing, the toon band count, and the shadow switch.
    std::uint32_t max_fps_requests = 0;
    std::uint32_t toon_shading_requests = 0;
    std::uint32_t shadow_toggles = 0;
    double max_fps_last = 0.0;
    float toon_steps_last = 0.0F;
    float toon_softness_last = 0.0F;
    bool shadow_enabled_last = true;
    script_status set_max_fps(double fps) {
        if (!std::isfinite(fps) || fps < 0.0) {
            return script_status::invalid_argument;
        }
        ++max_fps_requests;
        max_fps_last = fps;
        return script_status::success;
    }

    script_status set_toon_shading(float steps, float softness) {
        if (!std::isfinite(steps) || !std::isfinite(softness) || steps < 0.0F || steps > 32.0F ||
            softness < 0.0F || softness > 1.0F) {
            return script_status::invalid_argument;
        }
        ++toon_shading_requests;
        toon_steps_last = steps;
        toon_softness_last = softness;
        return script_status::success;
    }

    script_status set_shadow_enabled(std::uint8_t enabled) {
        if (enabled > 1) {
            return script_status::invalid_argument;
        }
        ++shadow_toggles;
        shadow_enabled_last = enabled != 0;
        return script_status::success;
    }

    /// ABI 29: the glTF loader's face measurement, as the loader reports it. The fixture hands back a
    /// plausible frame so a managed caller's own maths is exercised; nothing in the helmet scenario asks
    /// for it, which is why the counters are asserted to stay zero there.
    script_status query_face_frame(asset_handle asset, face_frame_desc* value) {
        ++face_frame_queries;
        if (value == nullptr || asset.value == 0) {
            return script_status::invalid_argument;
        }
        if (asset.value != face_frame_asset) {
            return script_status::asset_not_found;
        }
        *value = face_frame_desc{.forward = {0.0F, 0.0F, 1.0F},
                                 .right = {-1.0F, 0.0F, 0.0F},
                                 .world_forward = {0.0F, 0.0F, 1.0F},
                                 .world_right = {-1.0F, 0.0F, 0.0F},
                                 .world_center = {0.0F, 1.6F, 0.0F},
                                 .head_joint = 0,
                                 .skin_index = 0,
                                 .head_relative = 1,
                                 .valid = 1,
                                 .padding = {}};
        return script_status::success;
    }

    /// ABI 29: the head frame one character slot is shaded against. The fixture rejects the same
    /// degenerate frames the host does.
    script_status set_head_basis(std::uint32_t index, head_basis_desc const* value) {
        ++head_basis_requests;
        if (value == nullptr || index >= head_slot_capacity) {
            ++head_basis_rejections;
            return script_status::invalid_argument;
        }
        auto const& record = *value;
        auto const usable = [](float const* axis) {
            return std::isfinite(axis[0]) && std::isfinite(axis[1]) && std::isfinite(axis[2]) &&
                   axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2] > 1.0e-12F;
        };
        if (!usable(record.front) || !usable(record.right) || !usable(record.up) ||
            !std::isfinite(record.center[0]) || !std::isfinite(record.center[1]) ||
            !std::isfinite(record.center[2]) || record.captured > 1 || record.has_center > 1) {
            ++head_basis_rejections;
            return script_status::invalid_argument;
        }
        head_basis_slot = index;
        head_basis_captured = record.captured != 0;
        return script_status::success;
    }

    /// ABI 27: per-object material factors. The fixture mirrors the host's validator, so a managed
    /// caller that sent a non-finite factor or a lane above 3 is reported here.
    script_status set_material_override(std::uint64_t tree, node_handle node,
                                        material_override_desc const* value) {
        if (value == nullptr) {
            ++material_override_rejections;
            return script_status::invalid_argument;
        }
        auto const& record = *value;
        if (!std::isfinite(record.tint[0]) || !std::isfinite(record.tint[1]) ||
            !std::isfinite(record.tint[2]) || !std::isfinite(record.metallic_scale) ||
            !std::isfinite(record.roughness_scale) || !std::isfinite(record.emissive_scale) ||
            record.environment_slot > 3u || record.unlit > 1 || record.enabled > 1) {
            ++material_override_rejections;
            return script_status::invalid_argument;
        }
        if (tree == 0 || node.value == 0) {
            ++material_override_rejections;
            return script_status::invalid_node;
        }
        ++material_override_requests;
        material_override_last = record;
        return script_status::success;
    }

    /// ABI 31 panel verbs. The fixture validates what the host validates (an empty label is not a
    /// widget) so a managed caller cannot silently produce an empty panel.
    script_status ui_begin_panel(utf8_view title, float default_width, float default_height) {
        if (title.length == 0 || title.data == nullptr || !std::isfinite(default_width) ||
            !std::isfinite(default_height) || default_width < 0.0F || default_height < 0.0F) {
            ++ui_rejections;
            return script_status::invalid_argument;
        }
        ++ui_panels;
        return script_status::success;
    }

    script_status ui_end_panel() {
        return script_status::success;
    }

    script_status ui_text(utf8_view) {
        ++ui_texts;
        return script_status::success;
    }

    script_status ui_checkbox(utf8_view label, std::uint8_t* value) {
        if (label.length == 0 || value == nullptr) {
            ++ui_rejections;
            return script_status::invalid_argument;
        }
        ++ui_checkboxes;
        return script_status::success;
    }

    script_status ui_slider(utf8_view label, float* value, float minimum, float maximum) {
        if (label.length == 0 || value == nullptr || !std::isfinite(minimum) || !std::isfinite(maximum) ||
            minimum > maximum) {
            ++ui_rejections;
            return script_status::invalid_argument;
        }
        ++ui_sliders;
        return script_status::success;
    }

    script_status ui_vec3(utf8_view label, float* value, float speed) {
        if (label.length == 0 || value == nullptr || !std::isfinite(speed) || speed <= 0.0F) {
            ++ui_rejections;
            return script_status::invalid_argument;
        }
        ++ui_vec3s;
        return script_status::success;
    }

    script_status ui_combo(utf8_view label, utf8_view const* items, std::uint32_t count, std::int32_t* selected) {
        if (label.length == 0 || items == nullptr || selected == nullptr || count == 0 ||
            count > ui_combo_capacity) {
            ++ui_rejections;
            return script_status::invalid_argument;
        }
        ++ui_combos;
        return script_status::success;
    }

    script_status ui_button(utf8_view label, std::uint8_t* pressed) {
        if (label.length == 0 || pressed == nullptr) {
            ++ui_rejections;
            return script_status::invalid_argument;
        }
        ++ui_buttons;
        return script_status::success;
    }

    script_status ui_wants_mouse(std::uint8_t* value) {
        if (value == nullptr) {
            ++ui_rejections;
            return script_status::invalid_argument;
        }
        // The demo shows the pointer state as a label, so this answer only has to be well formed.
        *value = 0;
        ++ui_wants_mouse_calls;
        return script_status::success;
    }

    void set_input_callback(void (*callback)(void* context, input_event const* event), void* context) {
        input_callback = callback;
        input_context = context;
        if (callback == nullptr) {
            ++input_clears;
        } else {
            ++input_registrations;
        }
        input_sink_installed = callback != nullptr;
    }

    script_status get_scene_module(std::uint32_t module, void const** table, std::uint32_t* byte_size) {
        if (table == nullptr || byte_size == nullptr) {
            return script_status::invalid_argument;
        }
        *table = nullptr;
        *byte_size = 0;
        if (module == static_cast<std::uint32_t>(native_module::rml_ui)) {
            if (rml_module_mode == 0) {
                return script_status::module_unavailable;
            }
            static rml_ui_api const incomplete{};
            // ABI 42: a host built before the pointer/rect/style entries. The base six stay valid, so
            // the managed side must keep working and refuse only the two optional calls.
            static rml_ui_api const legacy = [] {
                rml_ui_api value = deren::ui::rml_scripting::table();
                value.get_element_rect = nullptr;
                value.set_element_style = nullptr;
                return value;
            }();
            *table = rml_module_mode == 3   ? &incomplete
                     : rml_module_mode == 4 ? &legacy
                                            : &deren::ui::rml_scripting::table();
            *byte_size = rml_module_mode == 2 ? sizeof(rml_ui_api) + sizeof(void*) : offsetof(rml_ui_api, set_markup);
            return script_status::success;
        }
        if (module == static_cast<std::uint32_t>(native_module::audio)) {
            if (audio_module_mode == 0) {
                return script_status::module_unavailable;
            }
            static audio_api const incomplete{};
            *table = audio_module_mode == 3 ? &incomplete : &audio_host::table();
            *byte_size = sizeof(audio_api) + (audio_module_mode == 2 ? 8 : 0);
            return script_status::success;
        }
        if (module == static_cast<std::uint32_t>(native_module::ui)) {
            static ui_api const published_ui{.begin_panel = ui_begin_panel,
                                             .end_panel = ui_end_panel,
                                             .text = ui_text,
                                             .checkbox = ui_checkbox,
                                             .slider = ui_slider,
                                             .vec3 = ui_vec3,
                                             .combo = ui_combo,
                                             .button = ui_button,
                                             .wants_mouse = ui_wants_mouse};
            *table = &published_ui;
            *byte_size = sizeof(published_ui);
            return script_status::success;
        }
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
            .bind_asset = bind_asset,
            .get_camera = get_camera,
            .set_camera = set_camera,
            .set_environment_hdr = set_environment_hdr,
            .asset_pipeline_count = asset_pipeline_count,
            .asset_pipeline = asset_pipeline,
            .asset_default_pipeline = asset_default_pipeline,
            .set_pipeline = set_pipeline,
            .set_exposure = set_exposure,
            .set_tone_mapping = set_tone_mapping,
            .set_environment = set_environment,
            .set_sun_intensity = set_sun_intensity,
            .set_bloom = set_bloom,
            .set_fxaa = set_fxaa,
            .set_taa = set_taa,
            .set_ssao = set_ssao,
            .set_gbuffer_debug = set_gbuffer_debug,
            .set_sun_direction = set_sun_direction,
            .set_area_light = set_area_light,
            .set_clustered_lights = set_clustered_lights,
            .set_point_lights = set_point_lights,
            .set_material_override = set_material_override,
            .set_max_fps = set_max_fps,
            .set_toon_shading = set_toon_shading,
            .set_shadow_enabled = set_shadow_enabled,
            .asset_face_frame = query_face_frame,
            .set_head_basis = set_head_basis,
            .set_capture_camera = set_capture_camera,
            .request_radiance_capture = request_radiance_capture,
            .acquire_radiance_image = acquire_radiance_image,
            .reset_temporal_history = reset_temporal_history,
            .write_radiance_cube = write_radiance_cube,
            .get_animation_state = get_animation_state,
            .animation_playable_count = animation_playable_count,
            .animation_playable_name = animation_playable_name,
            .select_animation = select_animation,
            .set_animation_playing = set_animation_playing,
            .set_animation_speed = set_animation_speed,
            .set_animation_time = set_animation_time,
            .node_name = node_name,
            .set_toon_rig = set_toon_rig,
            .get_toon_rig = get_toon_rig,
            .set_node_name = set_node_name,
            .set_scene_aspect = set_scene_aspect};
        *table = &published;
        *byte_size = sizeof(published);
        return script_status::success;
    }

    script_status publish_abi_layout(abi_layout_entry* out, std::uint32_t capacity) {
        // Mirrors the host in app_scripting.cpp so the managed side sees the real layouts here too.
        auto const delta = module_context != nullptr ? module_context->abi_size_delta : 0u;
        abi_layout_entry entries[] = {
            {.type = abi_type::native_api, .size = static_cast<std::uint32_t>(sizeof(native_api) + delta)},
            {.type = abi_type::managed_api, .size = sizeof(managed_api)},
            {.type = abi_type::frame_info, .size = sizeof(frame_info)},
            {.type = abi_type::transform_value, .size = sizeof(transform_value)},
            {.type = abi_type::input_event, .size = sizeof(input_event)},
            {.type = abi_type::point_light_desc, .size = sizeof(point_light_desc)},
            {.type = abi_type::material_override_desc, .size = sizeof(material_override_desc)},
            {.type = abi_type::startup_options, .size = sizeof(startup_options)},
            {.type = abi_type::head_basis_desc, .size = sizeof(head_basis_desc)},
            {.type = abi_type::face_frame_desc, .size = sizeof(face_frame_desc)},
            {.type = abi_type::animation_state, .size = sizeof(animation_state)},
            {.type = abi_type::diagnostic_counters, .size = sizeof(diagnostic_counters)},
            {.type = abi_type::toon_rig_desc, .size = sizeof(toon_rig_desc)},
        };
        constexpr auto count = static_cast<std::uint32_t>(std::size(entries));
        if (out == nullptr || capacity < count) {
            return script_status::invalid_argument;
        }
        for (std::uint32_t index = 0; index < count; ++index) {
            out[index] = entries[index];
        }
        return script_status::success;
    }

    script_status request_screenshot() {
        ++screenshot_requests;
        return script_status::success;
    }

    // The managed side answers each frame boundary from its own worker thread, so the count must be
    // atomic. It is the proof that the host's wait was released instead of timing out.
    std::atomic<unsigned> notify_count{0};

    script_status record_notify() {
        ++notify_count;
        return script_status::success;
    }

    // ABI 35: the demo project sets the boundary wait in initialize. The fake records the value so
    // the scenario can prove the managed-to-host direction works, not just that the field exists.
    std::atomic<std::uint32_t> boundary_timeout_ms{0};

    script_status record_timeout(std::uint32_t const milliseconds) {
        if (milliseconds > 600000) {
            return script_status::invalid_argument;
        }
        boundary_timeout_ms = milliseconds;
        return script_status::success;
    }

    // The real host blocks on a condition variable after it hands over a boundary. This test calls
    // the managed callbacks directly, so it has to wait the same way instead of racing the worker
    // thread that runs a non-empty batch.
    bool wait_for_notify(unsigned const expected) {
        for (int attempt = 0; attempt < 500; ++attempt) {
            if (notify_count.load() >= expected) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    script_status set_environment_hdr(utf8_view path, std::uint32_t requested_slot) {
        // The demo game asks for whatever `[lighting] environment_hdr` (or the procedural sky, encoded
        // as an empty path) says: both spellings are legal, so the fake records the request instead of
        // dictating one. The host itself validates the slot and the path - this fake only counts the
        // round trip and remembers the slot, so the scenario can prove slot 0 is what a project that
        // never names a slot actually asks for.
        if (path.length != 0 && path.data == nullptr) {
            return script_status::invalid_argument;
        }
        if (requested_slot >= 4u) {
            return script_status::invalid_argument;
        }
        environment_path_is_empty = path.length == 0;
        environment_slot = requested_slot;
        environment_slot_mask |= 1u << requested_slot;
        ++environment_requests;
        return script_status::success;
    }

    // ABI 39: the toon light rig. The real host clamps/normalises in script_scene and queues the
    // result between frames; the fake mirrors both halves (including "get answers the queued value")
    // so the managed self test round trips through the published table instead of a stub.
    toon_rig_desc toon_rig_current{};
    toon_rig_desc toon_rig_next{};
    bool toon_rig_pending = false;
    std::uint32_t toon_rig_sets = 0;
    std::uint32_t toon_rig_rejections = 0;
    std::uint32_t toon_rig_gets = 0;

    script_status set_toon_rig(toon_rig_desc const* value) {
        if (value == nullptr) {
            return script_status::invalid_argument;
        }
        float const* const lanes = reinterpret_cast<float const*>(value);
        for (int lane = 0; lane < 40; ++lane) {
            if (!std::isfinite(lanes[lane])) {
                ++toon_rig_rejections;
                return script_status::invalid_argument;
            }
        }
        if (value->reference_modes[2] != 0.0F || value->reference_modes[3] != 0.0F) {
            ++toon_rig_rejections;
            return script_status::invalid_argument;
        }
        toon_rig_next = *value;
        toon_rig_next.day[0] = std::clamp(value->day[0], 0.0F, 1.0F);
        toon_rig_next.other_colour[3] = std::clamp(value->other_colour[3], 0.0F, 3.0F);
        toon_rig_next.shadow_softness[0] = std::clamp(std::round(value->shadow_softness[0]), 0.0F, 4.0F);
        toon_rig_next.reference_modes[0] = value->reference_modes[0] != 0.0F ? 1.0F : 0.0F;
        toon_rig_next.reference_modes[1] = value->reference_modes[1] != 0.0F ? 1.0F : 0.0F;
        ++toon_rig_sets;
        toon_rig_pending = true;
        return script_status::success;
    }

    script_status get_toon_rig(toon_rig_desc* value) {
        if (value == nullptr) {
            return script_status::invalid_argument;
        }
        ++toon_rig_gets;
        *value = toon_rig_pending ? toon_rig_next : toon_rig_current;
        return script_status::success;
    }

    // ABI 37: the report the real host turns into deren-crash.log and terminates on. This fake
    // records it and answers success, which is the only way a test process survives to check that
    // the message, the managed location and the managed stack all crossed the boundary.
    std::uint32_t panic_reports = 0;
    std::uint32_t panic_line = 0;
    std::string panic_message;
    std::string panic_file;
    std::string panic_stack;

    script_status record_panic(utf8_view message, utf8_view source_file, std::uint32_t line, utf8_view stack) {
        if ((message.length != 0 && message.data == nullptr) || (source_file.length != 0 && source_file.data == nullptr) ||
            (stack.length != 0 && stack.data == nullptr)) {
            return script_status::invalid_argument;
        }
        auto const text = [](utf8_view const view) {
            return view.length == 0 ? std::string{} : std::string(reinterpret_cast<char const*>(view.data), view.length);
        };
        ++panic_reports;
        panic_line = line;
        panic_message = text(message);
        panic_file = text(source_file);
        panic_stack = text(stack);
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
        native_api const api{.log = scene_log,
                             .get_module = get_scene_module,
                             .request_screenshot = request_screenshot,
                             .set_input_event_callback = set_input_callback,
                             .abi_layout = publish_abi_layout,
                             .notify_rendering = record_notify,
                             .set_frame_boundary_timeout = record_timeout,
                             .panic = record_panic};
        managed_api scripts;
        // ABI 37: the demo reports one managed fault when it sees this variable. The fake host above
        // answers success instead of terminating, which is the only way a test process survives to
        // inspect the report, so the probe is armed for exactly these runs.
        // <cstdlib> is what declares _putenv_s on both compilers this project builds with.
        static_cast<void>(_putenv_s("DEREN_PANIC_PROBE", "1"));
        if (initialize_managed_host(paths, api, scripts).status != host_status::success) {
            std::fputs("startup: managed host initialization failed\n", stderr);
            return false;
        }
        if (panic_reports != 1 || panic_message.empty() || panic_line == 0 ||
            panic_file.find("game_host.cs") == std::string::npos || panic_stack.empty()) {
            std::fprintf(stderr, "startup: panic reports=%u line=%u file='%s' message='%s' stack=%zu\n", panic_reports,
                         panic_line, panic_file.c_str(), panic_message.c_str(), panic_stack.size());
            return false;
        }
        if (scripts.before_frame == nullptr || scripts.after_frame == nullptr) {
            std::fputs("frame: boundary callbacks missing\n", stderr);
            return false;
        }
        // An empty before-frame batch answers inline; the wait must settle exactly once.
        notify_count = 0;
        scripts.before_frame();
        if (!wait_for_notify(1) || notify_count != 1) {
            std::fprintf(stderr, "frame: before-frame wait answered %u times, expected 1\n", notify_count.load());
            return false;
        }
        if (scene.active_tree == 0 || scene.trees_created != 1 || scene.nodes_created != 3 ||
            scene.assets_loaded != 1 || scene.assets_bound != 3 || scene.assets_released != 1) {
            std::fprintf(stderr, "startup: tree=%llu trees=%u nodes=%u assets=%u/%u/%u\n",
                         static_cast<unsigned long long>(scene.active_tree), scene.trees_created, scene.nodes_created,
                         scene.assets_loaded, scene.assets_bound, scene.assets_released);
            return false;
        }
        // ABI 35: the demo sets 4000 ms in initialize, so a value other than the default proves the
        // managed-to-host direction rather than just the size of the table.
        if (boundary_timeout_ms.load() != 4000u) {
            std::fprintf(stderr, "startup: boundary timeout %u ms, expected 4000\n", boundary_timeout_ms.load());
            return false;
        }
        // supported_pipelines() queries the count and one entry, then the default is queried and
        // applied to the bind root by the demo game.
        if (pipeline_queries != 3 || pipeline_assignments != 1) {
            std::fprintf(stderr, "startup: pipeline queries=%u assignments=%u\n", pipeline_queries,
                         pipeline_assignments);
            return false;
        }
        // The demo sets all nine post-processing knobs, has two out-of-range calls rejected, then
        // restores the startup values: 18 accepted calls and 2 rejections. The framework's own
        // baseline (the project's `[lighting]` profile and `[render]` runtime knobs, applied before
        // the demo's initialize) adds the environment, sun-intensity and seven look calls: 27.
        if (post_processing_requests != 27 || post_processing_rejections != 2) {
            std::fprintf(stderr, "startup: post-processing requests=%u rejections=%u\n", post_processing_requests,
                         post_processing_rejections);
            return false;
        }
        // set + set + clustered, one invalid direction, then the three restores: 6 accepted, 1 rejected.
        // The framework baseline adds the profile's sun direction, its "no area light" clear and the
        // clustered-lights lane: 9.
        if (lighting_requests != 9 || lighting_rejections != 1) {
            std::fprintf(stderr, "startup: lighting requests=%u rejections=%u\n", lighting_requests,
                         lighting_rejections);
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
        // The demo queues its one-shot after-frame task; the boundary must run it, answer the wait,
        // and only then may the next update observe the task's effect.
        notify_count = 0;
        scripts.after_frame();
        if (!wait_for_notify(1)) {
            std::fprintf(stderr, "frame: after-frame wait was never answered\n");
            return false;
        }
        auto const follow_status = update_scene(scripts, 7, 0.5F);
        if (notify_count != 1 || follow_status != script_status::success) {
            std::fprintf(stderr, "frame: after-frame wait answered %u times, update %u\n", notify_count.load(),
                         static_cast<unsigned>(follow_status));
            return false;
        }
        // The environment request is the project's `[lighting] environment_hdr` round trip. Two calls
        // reach the host - `project_config.apply()` and the demo's own `try_set_hdr` - and the game
        // accepts either spelling of the sky, so the contract under test is "at least one request and
        // never a rejected one", not a hard-coded spell count.
        if (screenshot_requests != 1 || environment_requests == 0) {
            std::fprintf(stderr, "frame: screenshot requests=%u environment requests=%u\n", screenshot_requests,
                         environment_requests);
            return false;
        }
        // ... and the set of slots asked for is exactly what the demo is supposed to use: slot 0 from
        // the startup path (`[lighting] environment_hdr` or the procedural sky) and slot 1 from the
        // per-material lane the demo binds to its left helmet. A project path that wrote into a lane
        // it did not name, or a lane that never reached the host, shows up here.
        if (environment_slot_mask != 0b11u) {
            std::fprintf(stderr, "frame: environment slots asked for = 0x%x, expected 0x3 (slots 0 and 1)\n",
                         environment_slot_mask);
            return false;
        }
        // The project light rig is project data pushed only on the demo's `--asset` path; this
        // scenario runs the helmet demo, so the punctual set must still be untouched (a helmet demo
        // that suddenly lit itself with a character rig would be a regression, not a feature).
        if (light_rig_requests != 0 || light_rig_rejections != 0) {
            std::fprintf(stderr, "frame: point light requests=%u rejections=%u\n", light_rig_requests,
                         light_rig_rejections);
            return false;
        }
        // The demo builds its panel every frame: no panel, or a rejected verb, means the managed side
        // and the published table disagree about the ABI.
        if (ui_panels == 0 || ui_rejections != 0) {
            std::fprintf(stderr, "frame: ui panels=%u rejections=%u\n", ui_panels, ui_rejections);
            return false;
        }
        // The demo registers its handler on the first update; the registration is the proof the
        // native_api entry reached managed code.
        if (input_registrations != 1 || !input_sink_installed) {
            std::fprintf(stderr, "frame: input registrations=%u installed=%d\n", input_registrations,
                         input_sink_installed ? 1 : 0);
            return false;
        }
        // DELIVERY: what the host publishes must reach the handler the game registered. The app's own
        // forwarder only renames fields, so this is the seam that proves the managed dispatch decodes
        // the flat record and calls the game - the demo logs the first event it ever sees.
        if (input_callback == nullptr) {
            std::fputs("frame: input callback missing after registration\n", stderr);
            return false;
        }
        {
            input_event press{};
            press.type = input_event_type::mouse_button;
            press.button = 0;
            press.down = 1;
            input_callback(input_context, &press);
            if (last_log.find("input stream live") == std::string::npos) {
                std::fprintf(stderr, "frame: input handler did not report delivery (last log: %s)\n",
                             last_log.c_str());
                return false;
            }
        }
        // The helmet demo captures no reflection probe, so a capture entry reached here means a game
        // started a six-face capture it never asked for.
        if (capture_camera_calls != 0 || radiance_requests != 0 || radiance_reads != 0 || temporal_resets != 0 ||
            cube_writes != 0 || capture_rejections != 0) {
            std::fprintf(stderr,
                         "frame: capture calls camera=%u requests=%u reads=%u resets=%u writes=%u rejections=%u\n",
                         capture_camera_calls, radiance_requests, radiance_reads, temporal_resets, cube_writes,
                         capture_rejections);
            return false;
        }
        // The helmet demo binds an asset whose manifest names NO motion, so the host player must stay
        // absent and the demo must not try to drive one: the update loop only reads the snapshot.
        if (animation_selects != 0 || animation_play_requests != 0 || animation_speed_requests != 0 ||
            animation_time_requests != 0 || animation_name_queries != 0) {
            std::fprintf(stderr,
                         "frame: animation commands selects=%u play=%u speed=%u time=%u names=%u\n",
                         animation_selects, animation_play_requests, animation_speed_requests,
                         animation_time_requests, animation_name_queries);
            return false;
        }
        // The head frame is a character feature: the loader measures it per asset and the managed side
        // publishes it for a `game_toon` slot. The helmet demo runs `pbr`, so neither entry may be touched.
        if (face_frame_queries != 0 || head_basis_requests != 0 || head_basis_rejections != 0) {
            std::fprintf(stderr, "frame: face frame queries=%u head basis requests=%u rejections=%u\n",
                         face_frame_queries, head_basis_requests, head_basis_rejections);
            return false;
        }
        // The demo binds exactly one override: the left helmet is moved to environment slot 1, which
        // is the per-material lane the same run fills through set_environment_hdr. The record must
        // arrive enabled, with the factors intact (the imported factors, so only the slot differs).
        if (material_override_requests != 1 || material_override_rejections != 0) {
            std::fprintf(stderr, "frame: material override requests=%u rejections=%u\n",
                         material_override_requests, material_override_rejections);
            return false;
        }
        if (material_override_last.environment_slot != 1 || material_override_last.enabled != 1 ||
            material_override_last.tint[0] != 1.0f || material_override_last.metallic_scale != 1.0f ||
            material_override_last.roughness_scale != 1.0f || material_override_last.emissive_scale != 1.0f) {
            std::fprintf(stderr,
                         "frame: material override slot=%u enabled=%u tint=%g metallic=%g roughness=%g emissive=%g\n",
                         material_override_last.environment_slot, material_override_last.enabled,
                         static_cast<double>(material_override_last.tint[0]),
                         static_cast<double>(material_override_last.metallic_scale),
                         static_cast<double>(material_override_last.roughness_scale),
                         static_cast<double>(material_override_last.emissive_scale));
            return false;
        }
        if (toon_rig_sets != 0 || toon_rig_rejections != 0 || toon_rig_gets != 0) {
            std::fprintf(stderr, "frame: toon rig sets=%u rejections=%u gets=%u\n", toon_rig_sets,
                         toon_rig_rejections, toon_rig_gets);
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

    // ABI 34: the two boundaries and the six registration verbs. The demo game turns itself into a
    // self check when it sees `--frame-events-selftest`: it registers one task of every kind on the
    // first frame and fails its own update the moment a count drifts. This case owns the other half
    // of the contract, the part managed code cannot see: every boundary answers exactly once, in
    // order, with the render thread waiting in between.
    bool run_frame_events(host_paths const& paths) {
        char const* selftest_argv[]{"test_managed_host", "--frame-events-selftest"};
        auto selftest_paths = paths;
        selftest_paths.argc = 2;
        selftest_paths.argv = const_cast<char**>(selftest_argv);
        scene_fixture scene;
        module_context = &scene;
        native_api const api{.log = scene_log,
                             .get_module = get_scene_module,
                             .request_screenshot = request_screenshot,
                             .set_input_event_callback = set_input_callback,
                             .abi_layout = publish_abi_layout,
                             .notify_rendering = record_notify,
                             .set_frame_boundary_timeout = record_timeout,
                             .panic = record_panic};
        managed_api scripts;
        if (initialize_managed_host(selftest_paths, api, scripts).status != host_status::success) {
            std::fputs("frame events: managed host initialization failed\n", stderr);
            return false;
        }
        if (scripts.before_frame == nullptr || scripts.after_frame == nullptr) {
            std::fputs("frame events: boundary callbacks missing\n", stderr);
            return false;
        }
        notify_count = 0;
        unsigned answered = 0;
        for (int frame = 1; frame <= 6; ++frame) {
            // The first update registers the tasks; every later one verifies the boundaries that ran
            // since, so a failure here is the managed self check reporting a broken contract.
            if (update_scene(scripts, 7, 0.5F) != script_status::success) {
                std::fprintf(stderr, "frame events: the self check failed before boundary %d\n", frame);
                return false;
            }
            scripts.before_frame();
            ++answered;
            if (!wait_for_notify(answered) || notify_count != answered) {
                std::fprintf(stderr, "frame events: before boundary %d answered %u times, expected %u\n", frame,
                             notify_count.load(), answered);
                return false;
            }
            scripts.after_frame();
            ++answered;
            if (!wait_for_notify(answered) || notify_count != answered) {
                std::fprintf(stderr, "frame events: after boundary %d answered %u times, expected %u\n", frame,
                             notify_count.load(), answered);
                return false;
            }
        }
        if (update_scene(scripts, 7, 0.5F) != script_status::success) {
            std::fputs("frame events: the final self check failed\n", stderr);
            return false;
        }
        return true;
    }

    // ABI 39: the toon light rig round trip. The demo turns itself into a self check when it sees
    // `--toon-selftest`: it reads the current rig, proves a non-finite lane is rejected without
    // disturbing the queued value, applies 0.25 and reads it back, then proves out-of-range lanes map
    // onto the documented ranges. This case owns the host half: the call counts, and that what
    // reached the host is the normalised rig rather than the raw request or a failure.
    bool run_toon_rig(host_paths const& paths) {
        char const* selftest_argv[]{"test_managed_host", "--toon-selftest"};
        auto selftest_paths = paths;
        selftest_paths.argc = 2;
        selftest_paths.argv = const_cast<char**>(selftest_argv);
        scene_fixture scene;
        module_context = &scene;
        native_api const api{.log = scene_log,
                             .get_module = get_scene_module,
                             .request_screenshot = request_screenshot,
                             .set_input_event_callback = set_input_callback,
                             .abi_layout = publish_abi_layout,
                             .notify_rendering = record_notify,
                             .set_frame_boundary_timeout = record_timeout,
                             .panic = record_panic};
        managed_api scripts;
        if (initialize_managed_host(selftest_paths, api, scripts).status != host_status::success) {
            std::fputs("toon rig: managed host initialization failed\n", stderr);
            return false;
        }
        if (update_scene(scripts, 7, 0.5F) != script_status::success) {
            std::fputs("toon rig: the managed self check failed\n", stderr);
            return false;
        }
        // Four reads: the baseline, the unchanged value after the rejection, the 0.25 request and the
        // clamped request. Three accepted writes (0.25, the out-of-range one, the restore) and one
        // rejection for the NaN lane.
        if (toon_rig_sets != 3 || toon_rig_rejections != 1 || toon_rig_gets != 4) {
            std::fprintf(stderr, "toon rig: sets=%u rejections=%u gets=%u\n", toon_rig_sets, toon_rig_rejections,
                         toon_rig_gets);
            return false;
        }
        // The last write is the self check restoring the startup lanes, so the queued rig is the one
        // the scene began with - proof the readback was used as the starting point of every change.
        if (!toon_rig_pending || toon_rig_next.day[0] != toon_rig_current.day[0] ||
            toon_rig_next.shadow_softness[0] != toon_rig_current.shadow_softness[0]) {
            std::fprintf(stderr, "toon rig: restore missed day %g vs %g, softness %g vs %g\n",
                         static_cast<double>(toon_rig_next.day[0]), static_cast<double>(toon_rig_current.day[0]),
                         static_cast<double>(toon_rig_next.shadow_softness[0]),
                         static_cast<double>(toon_rig_current.shadow_softness[0]));
            return false;
        }
        if (scripts.stop() != script_status::success) {
            std::fputs("toon rig: managed stop failed\n", stderr);
            return false;
        }
        return true;
    }

    // ABI 40: renaming a node. The demo turns itself into a self check when it sees
    // `--node-name-selftest`: it creates and activates a tree, creates one named node, renames it and
    // reads the name back, then proves an empty name, a 300-byte name and a well-formed but stale
    // handle are refused without disturbing the stored name. This case owns the host half: exactly one
    // rename landed, the over-long name and the stale handle reached the host as refusals, the empty
    // name was refused before the call left the wrapper, and the stored bytes are the ones read back.
    bool run_node_name(host_paths const& paths) {
        char const* selftest_argv[]{"test_managed_host", "--node-name-selftest"};
        auto selftest_paths = paths;
        selftest_paths.argc = 2;
        selftest_paths.argv = const_cast<char**>(selftest_argv);
        scene_fixture scene;
        module_context = &scene;
        native_api const api{.log = scene_log,
                             .get_module = get_scene_module,
                             .request_screenshot = request_screenshot,
                             .set_input_event_callback = set_input_callback,
                             .abi_layout = publish_abi_layout,
                             .notify_rendering = record_notify,
                             .set_frame_boundary_timeout = record_timeout,
                             .panic = record_panic};
        managed_api scripts;
        if (initialize_managed_host(selftest_paths, api, scripts).status != host_status::success) {
            std::fputs("node name: managed host initialization failed\n", stderr);
            return false;
        }
        if (update_scene(scripts, 7, 0.5F) != script_status::success) {
            std::fputs("node name: the managed self check failed\n", stderr);
            return false;
        }
        // One accepted rename, one host-side rejection (the 300-byte name), one stale handle, and two
        // reads (after the rename and after the refusals).
        if (node_name_renames != 1 || node_name_rejections != 1 || node_name_misses != 1 ||
            node_name_reads != 2) {
            std::fprintf(stderr, "node name: renames=%u rejections=%u misses=%u reads=%u\n", node_name_renames,
                         node_name_rejections, node_name_misses, node_name_reads);
            return false;
        }
        auto const* renamed = find_tree_node(scene, scene.nodes[0].value);
        if (renamed == nullptr ||
            std::string_view{renamed->name, renamed->name_length} != "renamed-node") {
            std::fprintf(stderr, "node name: the stored name is '%.*s'\n",
                         renamed != nullptr ? static_cast<int>(renamed->name_length) : 0,
                         renamed != nullptr ? renamed->name : "");
            return false;
        }
        if (scripts.stop() != script_status::success) {
            std::fputs("node name: managed stop failed\n", stderr);
            return false;
        }
        return true;
    }

    // ABI 41: the projection aspect. The demo turns itself into a self check when it sees
    // `--scene-aspect-selftest`: it activates a tree and then names the shape of an editor viewport,
    // resets to the window aspect with zero, and has a negative aspect and NaN refused. This case owns
    // the host half: exactly the two accepted values arrived, the last one was the reset, and the two
    // malformed ones never got past validation.
    bool run_scene_aspect(host_paths const& paths) {
        char const* selftest_argv[]{"test_managed_host", "--scene-aspect-selftest"};
        auto selftest_paths = paths;
        selftest_paths.argc = 2;
        selftest_paths.argv = const_cast<char**>(selftest_argv);
        scene_fixture scene;
        module_context = &scene;
        native_api const api{.log = scene_log,
                             .get_module = get_scene_module,
                             .request_screenshot = request_screenshot,
                             .set_input_event_callback = set_input_callback,
                             .abi_layout = publish_abi_layout,
                             .notify_rendering = record_notify,
                             .set_frame_boundary_timeout = record_timeout,
                             .panic = record_panic};
        managed_api scripts;
        if (initialize_managed_host(selftest_paths, api, scripts).status != host_status::success) {
            std::fputs("scene aspect: managed host initialization failed\n", stderr);
            return false;
        }
        if (update_scene(scripts, 7, 0.5F) != script_status::success) {
            std::fputs("scene aspect: the managed self check failed\n", stderr);
            return false;
        }
        if (aspect_sets != 2 || aspect_rejections != 2 || aspect_last_reset != 1 ||
            aspect_last != 0.0F) {
            std::fprintf(stderr, "scene aspect: sets=%u rejections=%u last=%g reset=%u\n", aspect_sets,
                         aspect_rejections, static_cast<double>(aspect_last), aspect_last_reset);
            return false;
        }
        if (scripts.stop() != script_status::success) {
            std::fputs("scene aspect: managed stop failed\n", stderr);
            return false;
        }
        return true;
    }

    bool run_rml(host_paths const& paths, int mode) {
        rml_module_mode = mode;
        deren::ui::rml_document document;
        deren::ui::rml_scripting host(document);
        std::string fixture = VR_RMLUI_DEMO_DIR;
        char const* arguments[]{"test_managed_host", "--rml-selftest", mode == 1 ? fixture.c_str() : "missing"};
        auto configured = paths;
        configured.argc = 3;
        configured.argv = const_cast<char**>(arguments);
        scene_fixture scene;
        module_context = &scene;
        native_api const api{.log = scene_log, .get_module = get_scene_module, .request_screenshot = request_screenshot, .set_input_event_callback = set_input_callback, .abi_layout = publish_abi_layout, .notify_rendering = record_notify, .set_frame_boundary_timeout = record_timeout, .panic = record_panic};
        managed_api scripts;
        auto const result = initialize_managed_host(configured, api, scripts);
        bool ok = result.status == host_status::success;
        if (ok && mode == 1) {
            ok = document.text("status") == "Copied <text> & \xe4\xb8\xad";
            document.paint(640, 480);
            document.input({.type = deren::ui::input_type::move, .x = 60, .y = 190});
            document.input({.type = deren::ui::input_type::button, .button = 0, .down = true});
            document.input({.type = deren::ui::input_type::button, .button = 0, .down = false});
            ok = update_scene(scripts, 7, 0.01F) == script_status::success && ok;
        }
        if (result.status == host_status::success) {
            ok = scripts.stop != nullptr && scripts.stop() == script_status::success && ok;
        }
        if (!ok) {
            std::fprintf(stderr, "RmlUi self-test failed: mode %d, host status %d\n", mode, static_cast<int>(result.status));
        }
        return ok;
    }

    bool run_rml_layout(host_paths const& paths, bool optional_absent) {
        rml_module_mode = optional_absent ? 4 : 1;
        deren::ui::rml_document document;
        deren::ui::rml_scripting host(document);
        std::string fixture = VR_RMLUI_DEMO_DIR;
        char const* arguments[]{"test_managed_host", "--rml-layout-selftest", fixture.c_str(),
                                optional_absent ? "--rml-optional-absent" : "--rml-layout-present"};
        auto configured = paths;
        configured.argc = 4;
        configured.argv = const_cast<char**>(arguments);
        scene_fixture scene;
        module_context = &scene;
        native_api const api{.log = scene_log, .get_module = get_scene_module, .request_screenshot = request_screenshot, .set_input_event_callback = set_input_callback, .abi_layout = publish_abi_layout, .notify_rendering = record_notify, .set_frame_boundary_timeout = record_timeout, .panic = record_panic};
        managed_api scripts;
        auto const result = initialize_managed_host(configured, api, scripts);
        bool ok = result.status == host_status::success;
        if (ok) {
            // The managed side authors in density-independent pixels; the window is 2x, so the
            // viewport reports 320x240 and a pointer at framebuffer (10, 10) reports (5, 5).
            document.set_density_independent_pixel_ratio(2.0F);
            document.paint(640, 480);
            auto const first = update_scene(scripts, 7, 0.01F);
            ok = first == script_status::success && ok;
        }
        if (ok && !optional_absent) {
            document.paint(640, 480); // The width style written by the first tick needs a layout pass.
            float x = 0, y = 0, width = 0, height = 0;
            ok = document.element_rect("apply", x, y, width, height) && ok;
            int const center_x = static_cast<int>((x + width * 0.5F) * 2.0F);
            int const center_y = static_cast<int>((y + height * 0.5F) * 2.0F);
            document.input({.type = deren::ui::input_type::move, .x = center_x, .y = center_y});
            document.input({.type = deren::ui::input_type::button, .x = center_x, .y = center_y, .button = 0, .down = true});
            document.input({.type = deren::ui::input_type::move, .x = 10, .y = 10});
            document.input({.type = deren::ui::input_type::button, .x = 10, .y = 10, .button = 0, .down = false});
            ok = update_scene(scripts, 7, 0.01F) == script_status::success && ok;
        }
        if (result.status == host_status::success) {
            ok = scripts.stop != nullptr && scripts.stop() == script_status::success && ok;
        }
        if (!ok) {
            std::fprintf(stderr, "RmlUi layout self-test failed: optional_absent %d, host status %d\n",
                         optional_absent ? 1 : 0, static_cast<int>(result.status));
        }
        return ok;
    }

    bool run_audio(host_paths const& paths, int mode) {
        audio_module_mode = mode;
        std::string fixture = (std::filesystem::path(VR_AUDIO_FIXTURES) / "tone.opus").string();
        char const* arguments[]{"test_managed_host", "--audio-selftest", mode == 1 ? fixture.c_str() : "missing"};
        auto configured = paths;
        configured.argc = 3;
        configured.argv = const_cast<char**>(arguments);
        scene_fixture scene;
        module_context = &scene;
        if (!deren_audio_test_no_device() || !audio_host::begin_session()) {
            return false;
        }
        native_api const api{.log = scene_log, .get_module = get_scene_module, .request_screenshot = request_screenshot, .set_input_event_callback = set_input_callback, .abi_layout = publish_abi_layout, .notify_rendering = record_notify, .set_frame_boundary_timeout = record_timeout, .panic = record_panic};
        managed_api scripts;
        auto const result = initialize_managed_host(configured, api, scripts);
        bool ok = result.status == host_status::success;
        if (ok && mode == 1) {
            float output[512]{};
            ok = deren_audio_test_read_frames(output, 512) && update_scene(scripts, 7, 0.01f) == script_status::success;
        }
        if (ok) {
            ok = scripts.stop && scripts.stop() == script_status::success;
        }
        audio_host::end_session();
        if (ok && mode == 1) {
            ok = update_scene(scripts, 7, 0.01f) == script_status::success;
        }
        if (!ok) {
            std::fprintf(stderr, "audio self-test failed: mode %d, host status %d\n", mode, static_cast<int>(result.status));
        }
        return ok;
    }

    void log_message(char const* message, std::uint32_t length) {
        auto const view = std::string_view{message, length};
        last_log.assign(view);
        *log_target = view == "deren managed host ready";
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
    if (scenario == "rml_lifecycle" || scenario == "rml_module_unavailable" ||
        scenario == "rml_module_invalid" || scenario == "rml_module_incomplete") {
        int const mode = scenario == "rml_lifecycle" ? 1 : scenario == "rml_module_invalid"  ? 2
                                                       : scenario == "rml_module_incomplete" ? 3
                                                                                             : 0;
        return run_rml(paths, mode) ? 0 : 1;
    }
    if (scenario == "rml_layout" || scenario == "rml_module_optional_absent") {
        return run_rml_layout(paths, scenario == "rml_module_optional_absent") ? 0 : 1;
    }
    if (scenario == "audio_lifecycle" || scenario == "audio_module_unavailable" ||
        scenario == "audio_module_invalid" || scenario == "audio_module_incomplete") {
        int const mode = scenario == "audio_lifecycle" ? 1 : scenario == "audio_module_invalid"  ? 2
                                                         : scenario == "audio_module_incomplete" ? 3
                                                                                                 : 0;
        return run_audio(paths, mode) ? 0 : 1;
    }
    if (scenario == "frame_events") {
        if (!run_frame_events(paths)) {
            std::fputs("Managed frame event boundaries failed\n", stderr);
            return 1;
        }
        return 0;
    }
    if (scenario == "toon_rig") {
        if (!run_toon_rig(paths)) {
            std::fputs("Managed toon light rig round trip failed\n", stderr);
            return 1;
        }
        return 0;
    }
    if (scenario == "node_name") {
        if (!run_node_name(paths)) {
            std::fputs("Managed node rename failed\n", stderr);
            return 1;
        }
        return 0;
    }
    if (scenario == "scene_aspect") {
        if (!run_scene_aspect(paths)) {
            std::fputs("Managed scene aspect failed\n", stderr);
            return 1;
        }
        return 0;
    }
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
    auto api = deren::scripting::native_api{.log = log_message,
                                            .get_module = get_scene_module,
                                            .request_screenshot = request_screenshot,
                                            .abi_layout = publish_abi_layout,
                                            .notify_rendering = record_notify,
                                            .set_frame_boundary_timeout = record_timeout,
                                            .panic = record_panic};
    if (scenario == "abi_layout_mismatch") {
        // ABI 33: the fake host reports a native_api size the managed side does not share, and the
        // layout check lives in configure_from_engine, so this scenario has to go through configure.
        static scene_fixture layout_fixture;
        layout_fixture.abi_size_delta = 8;
        module_context = &layout_fixture;
        deren::scripting::startup_options options{};
        auto const result = deren::scripting::configure_managed_host(paths, api, options);
        if (result.status != host_status::invalid_api ||
            last_log.find("the layouts of 'native_api' disagree") == std::string::npos) {
            std::fprintf(stderr, "abi_layout_mismatch: status %d, last log '%s'\n",
                         static_cast<int>(result.status), last_log.c_str());
            return 1;
        }
        return 0;
    }
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
