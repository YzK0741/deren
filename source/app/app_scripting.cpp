module;
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>
module deren.app.scripting;
import deren.vstd;
import deren.app.toon;
import deren.asset_library;
import deren.engine.primitive;
import deren.engine.render_layout;
import deren.gaussian_loader;
import deren.gltf_loader;
import deren.toon_material_sidecar;

namespace deren::app {
    namespace {
        bool is_finite(scripting::vector_3 value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        bool is_valid_pose(scripting::transform_value const& value) {
            return is_finite(value.position) && is_finite(value.rotation_degrees) && is_finite(value.scale) &&
                   value.scale.x != 0.0f && value.scale.y != 0.0f && value.scale.z != 0.0f;
        }

        /** A finite value inside [low, high]; the runtime's own setters would clamp, scripts get an error. */
        bool in_range(float value, float low, float high) {
            return std::isfinite(value) && value >= low && value <= high;
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

        // ---- THE CAPTURED MATERIAL TEXT'S THREE PRIMITIVES (HEAD's `app_application.cpp` had them) ----
        //
        // They exist because the `.toon.tsv` beside a character is a dump of the reference game's material
        // inspector: every value arrives as TEXT (`"1.0,1.0,1.0,1.0"`, `"0.6000000238418579"`), and the
        // shading stage consumes the same slots glTF describes - so the text has to become the slot's own
        // type before `resolve_materials` freezes it.
        /** strtof with a full-string check (no exceptions: std::stof would abort under -fno-exceptions). */
        std::optional<float> parse_number(std::string_view const text) {
            if (text.empty()) {
                return std::nullopt;
            }
            deren::utility::vessel::string const copy(text); // strtof needs a null-terminated buffer
            char* end = nullptr;
            float const value = std::strtof(copy.c_str(), &end);
            if (end == copy.c_str() || *end != '\0') {
                return std::nullopt;
            }
            return value;
        }

        /** `"r,g,b[,a]"` with `components` fields; a negative or non-finite field rejects the whole row. */
        std::optional<glm::vec4> parse_toon_colour(std::string_view fields, int const components) {
            glm::vec4 colour(1.0f);
            for (int component = 0; component < components; ++component) {
                auto const comma = fields.find(',');
                auto const value = parse_number(fields.substr(0, comma));
                if (!value || !std::isfinite(*value) || *value < 0.0f ||
                    (component + 1 < components && comma == std::string_view::npos)) {
                    return std::nullopt;
                }
                colour[component] = *value;
                if (comma != std::string_view::npos) {
                    fields.remove_prefix(comma + 1);
                }
            }
            return colour;
        }

        /** The reference's colour rows are DISPLAY-ENCODED; the shader multiplies in linear light. */
        float toon_srgb_decode(float const value) {
            if (value <= 0.04045f) {
                return value / 12.92f;
            }
            return value < 1.0f ? std::pow((value + 0.055f) / 1.055f, 2.4f) : std::pow(value, 2.4f);
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

        // How long one frame boundary may stay unanswered before the host treats the managed side as
        // dead. 0 waits forever, which a debugger breakpoint or a deliberately long blocking task
        // needs; native_api.set_frame_boundary_timeout (ABI 35) is the only writer, and it runs on
        // the render thread or a frame-event worker, hence the atomic.
        std::atomic<std::uint32_t> frame_boundary_timeout_ms{5000};

        // A cap that still leaves room for a long task while keeping a stuck boundary visible as an
        // error inside the same minute rather than as a process that appears hung.
        constexpr std::uint32_t frame_boundary_timeout_cap_ms = 600000;

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
            // THE TOON MATERIAL SIDECAR, when this asset ships one (the normal case for a character).
            // Both are heap-owned so their addresses survive `assets` growing: the lookup state keeps
            // raw pointers to them, and the import reads the model's decoded pixels through the
            // sidecar's texture names (see `toon_texture_callback`).
            std::unique_ptr<deren::toon::sidecar> sidecar;
            std::unique_ptr<deren::app::detail::toon_lookup_state> toon;
            // THE SPLAT CLOUD, for `type = "gaussian"`. A cloud has no glTF scene and no materials:
            // `cloud` is set exactly when `model` is null, and the binder stages it in the shared
            // splat pass instead of importing primitives.
            std::unique_ptr<deren::gaussian::asset> cloud;
            // THE FACE FRAME, measured ONCE at load from the face-family mesh (see
            // `deren::gltf::face_frame_of`). It is the loader's measurement of where the PAINTED FACE
            // looks, which is not where a head bone points; the managed side reads it through ABI 29 and
            // derives the head basis the face SDF shades in. `valid == false` when the asset has no
            // usable face primitive, in which case the renderer's fallback frame stays in force.
            deren::gltf::face_frame face_frame{};
            std::array<float, 3> import_shift{};
            std::array<float, 3> scene_center{};
            float scene_radius = 0.0f;
            std::string default_pipeline;
            utility::vessel::vector<std::string> pipelines;
            unsigned references = 0;
        };

        /** Marks a bind_asset() wrapper root and remembers the pipelines it may switch to. */
        struct pipeline_binding {
            utility::vessel::vector<std::string> pipelines;
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

    std::expected<scripting::startup_options, std::string> script_scene::preinit(
        std::filesystem::path const& assembly, std::int32_t argc, char** argv) {
        auto const runtime_dir = utility::executable_directory() / "managed";
#if defined(_WIN32)
        constexpr char const* nethost_library = "nethost.dll";
#elif defined(__APPLE__)
        constexpr char const* nethost_library = "libnethost.dylib";
#else
        constexpr char const* nethost_library = "libnethost.so";
#endif
        scripting::native_api const api{.log = log,
                                        .get_module = get_module,
                                        .request_screenshot = request_screenshot,
                                        .set_input_event_callback = set_input_event_callback,
                                        .abi_layout = abi_layout,
                                        .notify_rendering = notify_rendering,
                                        .set_frame_boundary_timeout = set_frame_boundary_timeout};
        scripting::startup_options options{};
        auto const result = scripting::configure_managed_host(
            {.assembly = runtime_dir / "Deren.Managed.dll",
             .runtime_config = runtime_dir / "Deren.Managed.runtimeconfig.json",
             .nethost = runtime_dir / nethost_library,
             .game_assembly = assembly,
             .argc = argc,
             .argv = argv},
            api, options);
        if (result.status != scripting::host_status::success) {
            if (result.status == scripting::host_status::runtime_discovery_failed ||
                result.status == scripting::host_status::runtime_initialization_failed) {
                return std::unexpected(std::format("Cannot start .NET 10 for '{}'. Check the installed runtime (host detail 0x{:08x}).",
                                                   assembly.string(), static_cast<std::uint32_t>(result.detail)));
            }
            return std::unexpected(std::format("Cannot configure managed scripting from '{}' (host status {}, detail 0x{:08x}).",
                                               runtime_dir.string(), static_cast<int>(result.status), static_cast<std::uint32_t>(result.detail)));
        }
        return options;
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
#if defined(_WIN32)
        constexpr char const* nethost_library = "nethost.dll";
#elif defined(__APPLE__)
        constexpr char const* nethost_library = "libnethost.dylib";
#else
        constexpr char const* nethost_library = "libnethost.so";
#endif
        scripting::native_api const api{.log = log,
                                        .get_module = get_module,
                                        .request_screenshot = request_screenshot,
                                        .set_input_event_callback = set_input_event_callback,
                                        .abi_layout = abi_layout,
                                        .notify_rendering = notify_rendering,
                                        .set_frame_boundary_timeout = set_frame_boundary_timeout};
        starting_scene = this;
        auto const result = scripting::initialize_managed_host(
            {.assembly = runtime_dir / "Deren.Managed.dll",
             .runtime_config = runtime_dir / "Deren.Managed.runtimeconfig.json",
             .nethost = runtime_dir / nethost_library,
             .game_assembly = assembly},
            api, scripts);
        starting_scene = nullptr;
        if (result.status != scripting::host_status::success) {
            if (result.status == scripting::host_status::runtime_discovery_failed ||
                result.status == scripting::host_status::runtime_initialization_failed) {
                return std::unexpected(std::format("Cannot start .NET 10 for '{}'. Check the installed runtime (host detail 0x{:08x}).",
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

    bool script_scene::await_frame_callback(void (*callback)()) {
        {
            std::lock_guard const lock(frame_mutex);
            frame_pending = true;
        }
        // Only announce the boundary: managed code copies what it needs, runs the batch on its own
        // worker thread and answers through native_api notify_rendering.
        callback();
        std::unique_lock lock(frame_mutex);
        // A managed side that throws its way out of a batch never answers. One timeout and one
        // error line is the whole cost; the flag is cleared so the next frame starts clean. A
        // timeout of 0 waits without a deadline, which is what a breakpoint in a callable needs.
        auto const timeout_ms = frame_boundary_timeout_ms.load(std::memory_order_relaxed);
        bool answered = true;
        if (timeout_ms == 0) {
            frame_cv.wait(lock, [this] { return !frame_pending; });
        } else {
            answered = frame_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !frame_pending; });
        }
        if (!answered) {
            frame_pending = false;
            utility::error("scripts: managed code did not answer a frame boundary within {} ms", timeout_ms);
        }
        return answered;
    }

    scripting::script_status script_scene::set_frame_boundary_timeout(std::uint32_t const milliseconds) {
        if (milliseconds > frame_boundary_timeout_cap_ms) {
            return scripting::script_status::invalid_argument;
        }
        frame_boundary_timeout_ms.store(milliseconds, std::memory_order_relaxed);
        return scripting::script_status::success;
    }

    bool script_scene::signal_before_frame() {
        if (scripts.before_frame == nullptr) {
            return true;
        }
        return await_frame_callback(scripts.before_frame);
    }

    bool script_scene::signal_after_frame() {
        if (scripts.after_frame == nullptr) {
            return true;
        }
        return await_frame_callback(scripts.after_frame);
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

    scripting::script_status script_scene::get_camera(scripting::camera_state* value) {
        if (value == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* owner = starting_scene;
        if (owner == nullptr) {
            auto& scenes = active_scenes();
            owner = scenes.empty() ? nullptr : scenes.back();
        }
        if (owner == nullptr || owner->host.get_camera == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.get_camera(owner->host.context, value) ? scripting::script_status::success
                                                                  : scripting::script_status::invalid_argument;
    }

    scripting::script_status script_scene::set_camera(scripting::camera_state const* value) {
        if (value == nullptr || !std::isfinite(value->yaw_degrees) || !std::isfinite(value->pitch_degrees) ||
            !std::isfinite(value->distance) || value->distance <= 0.0F || !is_finite(value->target)) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* owner = starting_scene;
        if (owner == nullptr) {
            auto& scenes = active_scenes();
            owner = scenes.empty() ? nullptr : scenes.back();
        }
        if (owner == nullptr || owner->host.set_camera == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_camera(owner->host.context, value) ? scripting::script_status::success
                                                                  : scripting::script_status::invalid_argument;
    }

    // ABI 31: THE GAME'S INPUT HANDLER. One game, one handler, so the slot is file scope; the app
    // delivers on the render thread as events happen, which is why nothing here allocates.
    namespace {
        void (*input_callback)(void* context, scripting::input_event const* event) = nullptr;
        void* input_context = nullptr;
    } // namespace

    void script_scene::set_input_event_callback(void (*callback)(void* context, scripting::input_event const* event),
                                                void* context) {
        input_callback = callback;
        input_context = context;
    }

    void script_scene::publish_input_event(scripting::input_event const& event) {
        if (input_callback != nullptr) {
            input_callback(input_context, &event);
        }
    }

    scripting::script_status script_scene::request_screenshot() {
        script_scene* owner = starting_scene;
        if (owner == nullptr) {
            auto& scenes = active_scenes();
            owner = scenes.empty() ? nullptr : scenes.back();
        }
        if (owner == nullptr || owner->host.request_screenshot == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.request_screenshot(owner->host.context)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::set_environment_hdr(scripting::utf8_view const path,
                                                               std::uint32_t const environment_slot) {
        constexpr std::uint32_t max_path_bytes = 4096;
        if (path.length > max_path_bytes || (path.length != 0 && path.data == nullptr)) {
            return scripting::script_status::invalid_argument;
        }
        // The slot index is the engine's, not this module's: 0 is the scene-wide lane and 1..3 are the
        // per-material lanes, so the capacity comes from the layout that owns the heap slots.
        if (environment_slot >= deren::engine::render_layout::heap_environment_capacity) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* owner = starting_scene;
        if (owner == nullptr) {
            auto& scenes = active_scenes();
            owner = scenes.empty() ? nullptr : scenes.back();
        }
        if (owner == nullptr || owner->host.set_environment_hdr == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_environment_hdr(owner->host.context, std::string_view{path.data, path.length},
                                               environment_slot)
                   ? scripting::script_status::success
                   : scripting::script_status::asset_not_found;
    }

    script_scene* script_scene::current_owner() {
        if (starting_scene != nullptr) {
            return starting_scene;
        }
        auto& scenes = active_scenes();
        return scenes.empty() ? nullptr : scenes.back();
    }

    void script_scene::install_ui(managed_ui_host host) {
        this->ui_host = host;
    }

    namespace {
        // A combo is a menu, not a data set: the fixed cap keeps the conversion off the heap and turns an
        // absurd list into invalid_argument instead of a silent truncation.
        inline constexpr std::uint32_t combo_item_capacity = 64;
    } // namespace

    scripting::script_status script_scene::ui_begin_panel(scripting::utf8_view const title,
                                                          float const default_width, float const default_height) {
        if (title.length == 0 || (title.length != 0 && title.data == nullptr)) {
            return scripting::script_status::invalid_argument;
        }
        if (!std::isfinite(default_width) || !std::isfinite(default_height) || default_width < 0.0F ||
            default_height < 0.0F) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        if (owner->ui_host.begin_panel == nullptr) {
            return scripting::script_status::module_unavailable;
        }
        return owner->ui_host.begin_panel(owner->ui_host.context, std::string_view{title.data, title.length},
                                          default_width, default_height)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::ui_end_panel() {
        script_scene* const owner = current_owner();
        if (owner == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        if (owner->ui_host.end_panel == nullptr) {
            return scripting::script_status::module_unavailable;
        }
        return owner->ui_host.end_panel(owner->ui_host.context) ? scripting::script_status::success
                                                                : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::ui_text(scripting::utf8_view const text) {
        if (text.length != 0 && text.data == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        if (owner->ui_host.text == nullptr) {
            return scripting::script_status::module_unavailable;
        }
        return owner->ui_host.text(owner->ui_host.context, std::string_view{text.data, text.length})
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::ui_checkbox(scripting::utf8_view const label, std::uint8_t* const value) {
        if (value == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        if (label.length != 0 && label.data == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        if (owner->ui_host.checkbox == nullptr) {
            return scripting::script_status::module_unavailable;
        }
        return owner->ui_host.checkbox(owner->ui_host.context, std::string_view{label.data, label.length}, value)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::ui_slider(scripting::utf8_view const label, float* const value,
                                                     float const minimum, float const maximum) {
        if (value == nullptr || !std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum) {
            return scripting::script_status::invalid_argument;
        }
        if (label.length != 0 && label.data == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        if (owner->ui_host.slider == nullptr) {
            return scripting::script_status::module_unavailable;
        }
        return owner->ui_host.slider(owner->ui_host.context, std::string_view{label.data, label.length}, value,
                                     minimum, maximum)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::ui_vec3(scripting::utf8_view const label, float* const value,
                                                   float const speed) {
        if (value == nullptr || !std::isfinite(speed) || speed <= 0.0F) {
            return scripting::script_status::invalid_argument;
        }
        if (label.length != 0 && label.data == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        if (owner->ui_host.vec3 == nullptr) {
            return scripting::script_status::module_unavailable;
        }
        return owner->ui_host.vec3(owner->ui_host.context, std::string_view{label.data, label.length}, value, speed)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::ui_combo(scripting::utf8_view const label,
                                                    scripting::utf8_view const* const items,
                                                    std::uint32_t const count, std::int32_t* const selected) {
        if (items == nullptr || count == 0 || selected == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        if (label.length != 0 && label.data == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        if (count > combo_item_capacity) {
            return scripting::script_status::invalid_argument;
        }
        std::array<std::string_view, combo_item_capacity> labels{};
        for (std::uint32_t index = 0; index < count; ++index) {
            if (items[index].length != 0 && items[index].data == nullptr) {
                return scripting::script_status::invalid_argument;
            }
            labels[index] = std::string_view{items[index].data, items[index].length};
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        if (owner->ui_host.combo == nullptr) {
            return scripting::script_status::module_unavailable;
        }
        return owner->ui_host.combo(owner->ui_host.context, std::string_view{label.data, label.length},
                                    labels.data(), count, selected)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::ui_button(scripting::utf8_view const label, std::uint8_t* const pressed) {
        if (pressed == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        if (label.length != 0 && label.data == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        if (owner->ui_host.button == nullptr) {
            return scripting::script_status::module_unavailable;
        }
        return owner->ui_host.button(owner->ui_host.context, std::string_view{label.data, label.length}, pressed)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::ui_wants_mouse(std::uint8_t* const value) {
        if (value == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        if (owner->ui_host.wants_mouse == nullptr) {
            return scripting::script_status::module_unavailable;
        }
        return owner->ui_host.wants_mouse(owner->ui_host.context, value) ? scripting::script_status::success
                                                                         : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::set_exposure(float const exposure) {
        if (!in_range(exposure, 0.05f, 20.0f)) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_exposure == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_exposure(owner->host.context, exposure) ? scripting::script_status::success
                                                                       : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::set_tone_mapping(std::int32_t const mode) {
        if (mode < 0 || mode > 2) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_tone_mapping == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_tone_mapping(owner->host.context, mode) ? scripting::script_status::success
                                                                       : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::set_environment(float const rotation_degrees, float const strength,
                                                           std::int32_t const background, float const color_r,
                                                           float const color_g, float const color_b,
                                                           float const blur) {
        if (!std::isfinite(rotation_degrees) || !in_range(strength, 0.0f, 64.0f) || background < 0 ||
            background > 3 || !in_range(color_r, 0.0f, 1.0f) || !in_range(color_g, 0.0f, 1.0f) ||
            !in_range(color_b, 0.0f, 1.0f) || !in_range(blur, 0.0f, 1.0f)) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_environment == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_environment(owner->host.context, rotation_degrees, strength, background, color_r,
                                           color_g, color_b, blur)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::set_sun_intensity(float const scale) {
        if (!in_range(scale, 0.0f, 3.0f)) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_sun_intensity == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_sun_intensity(owner->host.context, scale) ? scripting::script_status::success
                                                                         : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::set_bloom(float const intensity, float const threshold) {
        if (!in_range(intensity, 0.0f, 4.0f) || !in_range(threshold, 0.0f, 0.75f)) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_bloom == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_bloom(owner->host.context, intensity, threshold) ? scripting::script_status::success
                                                                                : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::set_fxaa(std::uint8_t const enabled, float const subpixel,
                                                    float const edge_threshold) {
        if (!in_range(subpixel, 0.0f, 1.0f) || !in_range(edge_threshold, 0.0f, 1.0f)) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_fxaa == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_fxaa(owner->host.context, enabled != 0, subpixel, edge_threshold)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::set_taa(std::uint8_t const enabled, float const blend_static,
                                                   float const blend_min) {
        if (!in_range(blend_static, 0.0f, 0.99f) || !in_range(blend_min, 0.0f, blend_static)) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_taa == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_taa(owner->host.context, enabled != 0, blend_static, blend_min)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::set_ssao(std::uint8_t const enabled, float const radius,
                                                    float const intensity, std::uint32_t const samples) {
        if (!std::isfinite(radius) || radius < 0.0f || !in_range(intensity, 0.0f, 1.0f) || samples > 16) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_ssao == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_ssao(owner->host.context, enabled != 0, radius, intensity, samples)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::set_gbuffer_debug(std::uint8_t const enabled) {
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_gbuffer_debug == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_gbuffer_debug(owner->host.context, enabled != 0) ? scripting::script_status::success
                                                                                : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::set_sun_direction(float const x, float const y, float const z) {
        // The runtime silently ignores a zero or non-finite vector; a managed script gets told instead.
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || x * x + y * y + z * z <= 1e-12f) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_sun_direction == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_sun_direction(owner->host.context, x, y, z) ? scripting::script_status::success
                                                                           : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::set_area_light(float const centre_x, float const centre_y,
                                                          float const centre_z, float const half,
                                                          std::uint8_t const irradiance, float const axis_x,
                                                          float const axis_y, float const axis_z,
                                                          float const penumbra) {
        // half <= 0 clears the emitter, so the axis is only meaningful for a live one.
        if (!std::isfinite(centre_x) || !std::isfinite(centre_y) || !std::isfinite(centre_z) || !std::isfinite(half) ||
            !std::isfinite(penumbra) || penumbra < 0.0f) {
            return scripting::script_status::invalid_argument;
        }
        if (half > 0.0f && (!std::isfinite(axis_x) || !std::isfinite(axis_y) || !std::isfinite(axis_z) ||
                            axis_x * axis_x + axis_y * axis_y + axis_z * axis_z <= 1e-12f)) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_area_light == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_area_light(owner->host.context, centre_x, centre_y, centre_z, half, irradiance != 0,
                                          axis_x, axis_y, axis_z, penumbra)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::set_clustered_lights(std::uint8_t const enabled) {
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_clustered_lights == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_clustered_lights(owner->host.context, enabled != 0)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::set_max_fps(double const fps) {
        // 0 is the documented "uncapped" value, so anything below it (and NaN, which would slip through
        // a plain comparison) is a caller error rather than a policy.
        if (!std::isfinite(fps) || fps < 0.0) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_max_fps == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_max_fps(owner->host.context, fps)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::set_toon_shading(float const steps, float const softness) {
        // The shader computes its bands from these two floats: a negative count has no meaning, and a
        // count past 32 would quantize into noise, so both are refused instead of clamped.
        if (!std::isfinite(steps) || !std::isfinite(softness) || steps < 0.0F || steps > 32.0F ||
            softness < 0.0F || softness > 1.0F) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_toon_shading == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_toon_shading(owner->host.context, steps, softness)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::set_shadow_enabled(std::uint8_t const enabled) {
        if (enabled > 1) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_shadow_enabled == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_shadow_enabled(owner->host.context, enabled != 0)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }

    scripting::script_status script_scene::asset_face_frame(scripting::asset_handle const asset,
                                                            scripting::face_frame_desc* const value) {
        if (value == nullptr || asset.value == 0) {
            return scripting::script_status::invalid_argument;
        }
        auto& list = assets().assets;
        for (auto const& entry : list) {
            if (entry.id != asset.value) {
                continue;
            }
            // A CLOUD, a model with no face primitive and a model whose face attributes are unusable all
            // land here: managed code then keeps the fallback head frame instead of publishing one nobody
            // measured.
            if (!entry.face_frame.valid) {
                return scripting::script_status::asset_invalid;
            }
            deren::gltf::face_frame const& frame = entry.face_frame;
            *value = scripting::face_frame_desc{
                .forward = {frame.forward.x, frame.forward.y, frame.forward.z},
                .right = {frame.right.x, frame.right.y, frame.right.z},
                .world_forward = {frame.world_forward.x, frame.world_forward.y, frame.world_forward.z},
                .world_right = {frame.world_right.x, frame.world_right.y, frame.world_right.z},
                .world_center = {frame.world_center.x, frame.world_center.y, frame.world_center.z},
                .head_joint = static_cast<std::uint32_t>(frame.head_joint),
                .skin_index = static_cast<std::uint32_t>(frame.skin_index),
                .head_relative = static_cast<std::uint8_t>(frame.head_relative),
                .valid = 1,
                .padding = {0, 0}};
            return scripting::script_status::success;
        }
        return scripting::script_status::asset_not_found;
    }

    scripting::script_status script_scene::set_head_basis(std::uint32_t const index,
                                                          scripting::head_basis_desc const* const value) {
        if (value == nullptr || index >= deren::engine::max_game_toon_characters) {
            return scripting::script_status::invalid_argument;
        }
        auto const finite = [](float const(&vector)[3]) {
            return std::isfinite(vector[0]) && std::isfinite(vector[1]) && std::isfinite(vector[2]);
        };
        auto const length_squared = [](float const(&vector)[3]) {
            return vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2];
        };
        // A frame with a null or a degenerate axis cannot be normalised, and the shader would divide by its
        // length: that is a caller error, not a value to clamp.
        if (!finite(value->front) || !finite(value->right) || !finite(value->up) || !finite(value->center) ||
            length_squared(value->front) < 1.0e-12F || length_squared(value->right) < 1.0e-12F ||
            value->captured > 1 || value->has_center > 1) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_head_basis == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_head_basis(owner->host.context, index, *value)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }
    scripting::script_status script_scene::set_capture_camera(std::uint8_t const enabled, float const position[3],
                                                              std::uint32_t const face, float const near_plane,
                                                              float const far_plane) {
        if (enabled > 1) {
            return scripting::script_status::invalid_argument;
        }
        if (enabled != 0) {
            // The cube faces are the engine's, and a camera with a broken frustum or a non-finite eye
            // would render six useless images rather than fail loudly.
            if (position == nullptr || face > 5 || !std::isfinite(position[0]) || !std::isfinite(position[1]) ||
                !std::isfinite(position[2]) || !std::isfinite(near_plane) || !std::isfinite(far_plane) ||
                near_plane <= 0.0F || far_plane <= near_plane) {
                return scripting::script_status::invalid_argument;
            }
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_capture_camera == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_capture_camera(owner->host.context, enabled, position, face, near_plane, far_plane)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }
    scripting::script_status script_scene::request_radiance_capture() {
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.request_radiance_capture == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        // A refused request means one capture is already armed: the caller asked for two frames' worth
        // of radiance and can only collect one image, which is a caller error rather than a failure.
        return owner->host.request_radiance_capture(owner->host.context)
                   ? scripting::script_status::success
                   : scripting::script_status::invalid_argument;
    }
    scripting::script_status script_scene::acquire_radiance_image(std::uint32_t* const width,
                                                                  std::uint32_t* const height, float* const rgba,
                                                                  std::uint32_t const float_count) {
        if (width == nullptr || height == nullptr || rgba == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.acquire_radiance_image == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.acquire_radiance_image(owner->host.context, *width, *height, rgba, float_count)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }
    scripting::script_status script_scene::reset_temporal_history() {
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.reset_temporal_history == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.reset_temporal_history(owner->host.context) ? scripting::script_status::success
                                                                       : scripting::script_status::script_failure;
    }
    scripting::script_status script_scene::write_radiance_cube(scripting::utf8_view const path, std::uint32_t const size,
                                                               float const* const pixels,
                                                               std::uint32_t const float_count) {
        // The byte count is the caller's contract with the file format: six faces of size*size RGBA
        // floats. A mismatch would write a probe that decodes as garbage, so it is rejected here.
        std::uint64_t const expected = std::uint64_t(6) * size * size * 4;
        if (path.length == 0 || path.data == nullptr || pixels == nullptr || expected != float_count) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.write_radiance_cube == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.write_radiance_cube(owner->host.context, std::string_view(path.data, path.length), size,
                                               pixels, float_count)
                   ? scripting::script_status::success
                   : scripting::script_status::script_failure;
    }
    scripting::script_status script_scene::set_point_lights(scripting::point_light_desc const* const lights,
                                                            std::uint32_t const count) {
        // The count is the caller's contract with the light UBO: the engine has a fixed array, and
        // silently truncating a rig would drop the lights the author put last. A null array with a
        // non-zero count is a broken span rather than an empty set, so it is rejected too.
        if (count > deren::engine::max_punctual_lights || (count != 0 && lights == nullptr)) {
            return scripting::script_status::invalid_argument;
        }
        script_scene* const owner = current_owner();
        if (owner == nullptr || owner->host.set_point_lights == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.set_point_lights(owner->host.context, lights, count)
                   ? scripting::script_status::success
                   : scripting::script_status::invalid_argument;
    }

    scripting::script_status script_scene::set_material_override(std::uint64_t const tree,
                                                                 scripting::node_handle const node,
                                                                 scripting::material_override_desc const* const value) {
        if (value == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        auto* scene = find_managed_tree(tree);
        if (scene == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        auto* tree_entry = find_managed_tree_entry(tree);
        if (tree_entry == nullptr || !tree_entry->active || tree_entry->owner == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        auto* target = scene->find_node(to_entity(node));
        if (target == nullptr) {
            return scripting::script_status::invalid_node;
        }
        auto& owner = tree_entry->owner;
        if (owner->host.apply_material_override == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        // The host refuses an out-of-range factor the same way the flat record's validators do, so
        // a rejection is the caller's error rather than a broken asset.
        return owner->host.apply_material_override(owner->host.context, *target, *value)
                   ? scripting::script_status::success
                   : scripting::script_status::invalid_argument;
    }

    scripting::script_status script_scene::asset_pipeline_count(scripting::asset_handle const asset,
                                                                std::uint32_t* const count) {
        if (count == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        *count = 0;
        auto* source = find_asset(asset.value);
        if (source == nullptr) {
            return scripting::script_status::asset_not_found;
        }
        *count = static_cast<std::uint32_t>(source->pipelines.size());
        return scripting::script_status::success;
    }

    scripting::script_status script_scene::asset_pipeline(scripting::asset_handle const asset,
                                                          std::uint32_t const index,
                                                          scripting::utf8_view* const name) {
        if (name == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        *name = {};
        auto* source = find_asset(asset.value);
        if (source == nullptr) {
            return scripting::script_status::asset_not_found;
        }
        if (index >= source->pipelines.size()) {
            return scripting::script_status::asset_not_found;
        }
        std::string const& pipeline = source->pipelines[index];
        *name = {.data = pipeline.data(), .length = static_cast<std::uint32_t>(pipeline.size())};
        return scripting::script_status::success;
    }

    scripting::script_status script_scene::asset_default_pipeline(scripting::asset_handle const asset,
                                                                  scripting::utf8_view* const name) {
        if (name == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        *name = {};
        auto* source = find_asset(asset.value);
        if (source == nullptr) {
            return scripting::script_status::asset_not_found;
        }
        if (source->default_pipeline.empty()) {
            return scripting::script_status::asset_invalid;
        }
        *name = {.data = source->default_pipeline.data(),
                 .length = static_cast<std::uint32_t>(source->default_pipeline.size())};
        return scripting::script_status::success;
    }

    scripting::script_status script_scene::set_pipeline(std::uint64_t const tree, scripting::node_handle const node,
                                                        scripting::utf8_view const pipeline) {
        if (pipeline.data == nullptr || pipeline.length == 0) {
            return scripting::script_status::invalid_argument;
        }
        auto* scene = find_managed_tree(tree);
        if (scene == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        auto* tree_entry = find_managed_tree_entry(tree);
        if (tree_entry == nullptr || !tree_entry->active || tree_entry->owner == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        auto* target = scene->find_node(to_entity(node));
        if (target == nullptr) {
            return scripting::script_status::invalid_node;
        }
        auto* binding = engine::scene_tree::components().try_get<pipeline_binding>(to_entity(node));
        if (binding == nullptr) {
            return scripting::script_status::invalid_node;
        }
        std::string_view const requested{pipeline.data, pipeline.length};
        bool supported = false;
        for (std::string const& candidate : binding->pipelines) {
            if (candidate == requested) {
                supported = true;
                break;
            }
        }
        if (!supported) {
            return scripting::script_status::invalid_argument;
        }
        auto& owner = tree_entry->owner;
        if (owner->host.apply_pipeline == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        return owner->host.apply_pipeline(owner->host.context, *target, requested)
                   ? scripting::script_status::success
                   : scripting::script_status::asset_invalid;
    }

    void script_scene::log(char const* text, std::uint32_t length) {
        utility::log("scripts: {}", std::string_view{text, length});
    }

    scripting::script_status script_scene::get_module(std::uint32_t const module, void const** table,
                                                      std::uint32_t* const byte_size) {
        if (table == nullptr || byte_size == nullptr) {
            return scripting::script_status::invalid_argument;
        }
        *table = nullptr;
        *byte_size = 0;
        if (module == static_cast<std::uint32_t>(scripting::native_module::ui)) {
            // The panel table exists even when no overlay is installed: the managed side then gets
            // module_unavailable from the first widget instead of a silently empty frame.
            static scripting::ui_api const ui_api{
                .begin_panel = ui_begin_panel,
                .end_panel = ui_end_panel,
                .text = ui_text,
                .checkbox = ui_checkbox,
                .slider = ui_slider,
                .vec3 = ui_vec3,
                .combo = ui_combo,
                .button = ui_button,
                .wants_mouse = ui_wants_mouse,
            };
            *table = &ui_api;
            *byte_size = sizeof(ui_api);
            return scripting::script_status::success;
        }
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
            .asset_face_frame = asset_face_frame,
            .set_head_basis = set_head_basis,
            .set_capture_camera = set_capture_camera,
            .request_radiance_capture = request_radiance_capture,
            .acquire_radiance_image = acquire_radiance_image,
            .reset_temporal_history = reset_temporal_history,
            .write_radiance_cube = write_radiance_cube};
        *table = &scene_api;
        *byte_size = sizeof(scene_api);
        return scripting::script_status::success;
    }

    scripting::script_status script_scene::abi_layout(scripting::abi_layout_entry* const out,
                                                      std::uint32_t const capacity) {
        using scripting::abi_type;
        // ABI 33: one entry per struct the managed side mirrors. The module tables are not listed:
        // get_module() reports their size, so scene_api and ui_api each check their own table.
        constexpr scripting::abi_layout_entry entries[] = {
            {.type = abi_type::native_api, .size = sizeof(scripting::native_api)},
            {.type = abi_type::managed_api, .size = sizeof(scripting::managed_api)},
            {.type = abi_type::frame_info, .size = sizeof(scripting::frame_info)},
            {.type = abi_type::transform_value, .size = sizeof(scripting::transform_value)},
            {.type = abi_type::input_event, .size = sizeof(scripting::input_event)},
            {.type = abi_type::point_light_desc, .size = sizeof(scripting::point_light_desc)},
            {.type = abi_type::material_override_desc, .size = sizeof(scripting::material_override_desc)},
            {.type = abi_type::startup_options, .size = sizeof(scripting::startup_options)},
            {.type = abi_type::head_basis_desc, .size = sizeof(scripting::head_basis_desc)},
            {.type = abi_type::face_frame_desc, .size = sizeof(scripting::face_frame_desc)},
        };
        constexpr auto count = static_cast<std::uint32_t>(std::size(entries));
        if (out == nullptr || capacity < count) {
            return scripting::script_status::invalid_argument;
        }
        for (std::uint32_t index = 0; index < count; ++index) {
            out[index] = entries[index];
        }
        return scripting::script_status::success;
    }

    scripting::script_status script_scene::notify_rendering() {
        script_scene* const owner = current_owner();
        if (owner == nullptr) {
            return scripting::script_status::scene_not_found;
        }
        std::lock_guard const lock(owner->frame_mutex);
        if (!owner->frame_pending) {
            // Nothing is waiting: a duplicate or out-of-step answer. Reporting it keeps such a
            // managed side visible instead of letting the call settle some later frame's wait.
            return scripting::script_status::invalid_argument;
        }
        owner->frame_pending = false;
        owner->frame_cv.notify_all();
        return scripting::script_status::success;
    }

    scripting::script_status script_scene::create_scene_tree(std::uint64_t* tree) {
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

    scripting::script_status script_scene::activate_scene(std::uint64_t const tree) {
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

    scripting::script_status script_scene::get_active_scene(std::uint64_t* tree) {
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

    scripting::script_status script_scene::deactivate_scene(std::uint64_t const tree) {
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

    scripting::script_status script_scene::destroy_scene_tree(std::uint64_t const tree) {
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

    scripting::script_status script_scene::create_node(std::uint64_t const tree, scripting::node_handle const parent,
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

    scripting::script_status script_scene::destroy_node(std::uint64_t const tree, scripting::node_handle const node) {
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

    scripting::script_status script_scene::get_node_transform(std::uint64_t const tree, scripting::node_handle const node,
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

    scripting::script_status script_scene::set_node_transform(std::uint64_t const tree, scripting::node_handle const node,
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

    scripting::script_status script_scene::set_node_visible(std::uint64_t const tree,
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

    scripting::script_status script_scene::list_asset(std::uint32_t const index,
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

    scripting::script_status script_scene::load_asset(scripting::utf8_view const id,
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
        // A GAUSSIAN ASSET IS A CLOUD, NOT A MODEL: `model` names a `.ply` the splat loader reads, and
        // there is no scene to import, no material to resolve and no sidecar to look for. Its bounds are
        // the cloud's own 3-sigma support, which is what frames it and what the binder reports.
        if (entry->type == asset_library::asset_type::gaussian) {
            auto loaded = deren::gaussian::load_ply(entry->model.string());
            if (!loaded) {
                utility::error("scripts: could not read the gaussian cloud '{}' from '{}' (error {}, byte {})",
                               entry->id, entry->model.string(), static_cast<std::int32_t>(loaded.error().code),
                               loaded.error().byte_offset);
                return scripting::script_status::asset_invalid;
            }
            glm::vec3 const low{loaded->bounds_min[0], loaded->bounds_min[1], loaded->bounds_min[2]};
            glm::vec3 const high{loaded->bounds_max[0], loaded->bounds_max[1], loaded->bounds_max[2]};
            auto const cloud_center = (low + high) * 0.5F;
            auto const cloud_radius = glm::length(high - low) * 0.5F;
            std::array<float, 3> const import_shift{-cloud_center.x, -cloud_center.y - cloud_radius, -cloud_center.z};
            std::array<float, 3> const scene_center{cloud_center.x + import_shift[0],
                                                    cloud_center.y + import_shift[1],
                                                    cloud_center.z + import_shift[2]};
            utility::log("gaussian cloud: '{}' - {} splats, SH degree {}, bounds ({:.2f}, {:.2f}, {:.2f})..({:.2f}, "
                         "{:.2f}, {:.2f})",
                         entry->model.string(), loaded->particles.size(), loaded->sh_degree, low.x, low.y, low.z,
                         high.x, high.y, high.z);
            auto const cloud_handle = state.next_asset_id++;
            state.assets.push_back(managed_asset{.id = cloud_handle,
                                                 .asset_id = entry->id,
                                                 .model = nullptr,
                                                 .materials = {},
                                                 .sidecar = nullptr,
                                                 .toon = nullptr,
                                                 .cloud = std::make_unique<deren::gaussian::asset>(std::move(*loaded)),
                                                 .import_shift = import_shift,
                                                 .scene_center = scene_center,
                                                 .scene_radius = cloud_radius,
                                                 .default_pipeline = {},
                                                 .pipelines = {},
                                                 .references = 1});
            *asset = {.value = cloud_handle};
            return scripting::script_status::success;
        }
        auto loaded = gltf::load_model(entry->model.string(),
                                       gltf::load_options{.extract_subtilis_payload = false,
                                                          .asset_up_axis = entry->z_up ? gltf::up_axis::z
                                                                                       : gltf::up_axis::y});
        if (!loaded) {
            utility::error("scripts: could not load asset '{}' from '{}'", entry->id, entry->model.string());
            return scripting::script_status::asset_invalid;
        }
        auto model = std::make_unique<gltf::scenes>(std::move(*loaded));
        auto const bounds = gltf::compute_scene_bounds(*model);
        // THE TOON MATERIAL SIDECAR, resolved against THIS model's images. A `.toon.tsv` beside the
        // model names a second set of slots glTF has no concept of (diffuse ramp, shadow LUT, specular
        // ramp, matcap, face SDF) and nothing connects that file to the model except IMAGE NAMES. The
        // lookup is baked once per asset here and installed right before the import (see the binder in
        // run.cpp), because the runtime's import is the only moment that reads it.
        std::unique_ptr<deren::toon::sidecar> sidecar;
        std::unique_ptr<deren::app::detail::toon_lookup_state> toon;
        auto const loaded_sidecar = deren::toon::load_sidecar(entry->model.string());
        if (!loaded_sidecar.has_value()) {
            utility::log("toon sidecar: NOT READ for '{}' - {}", entry->model.string(), loaded_sidecar.error());
        } else if (loaded_sidecar->empty()) {
            utility::log("toon sidecar: none beside '{}' (the normal case for a model that is not a character)",
                         entry->model.string());
        } else {
            sidecar = std::make_unique<deren::toon::sidecar>(*loaded_sidecar);
            deren::app::detail::log_toon_sidecar(*model, *sidecar);
            toon = std::make_unique<deren::app::detail::toon_lookup_state>();
            toon->sidecar = sidecar.get();
            toon->scenes = model.get();
            deren::app::detail::bake_toon_textures(*toon);
        }
        // ---- THE CAPTURED MATERIAL INPUTS ARE STAGED HERE, BEFORE THE glTF SLOTS ARE RESOLVED ----
        //
        // The sidecar's rows for `_UseEmission` / `_EmissionMap` / `_EmissionBrightness` /
        // `_EmissionColor` describe the emissive slot of a material whose glTF node states NOTHING (the
        // reference game drove it from the material inspector instead), and an OVERLAY material states
        // its tint in `_BaseColor` with the same reason. `resolve_materials` is the moment those slots
        // are finalised into the draw records, so the write has to happen before it - afterwards the
        // value would only reach the mip chain, not the shading stage.
        if (sidecar != nullptr) {
            for (auto& material : model->materials) {
                auto const* row = sidecar->find(material.name);
                if (row == nullptr) {
                    continue;
                }
                // An overlay leaf multiplies its captured `_BaseColor` over what the character stage wrote;
                // its alpha is squared because the reference's overlay shader does that (see HEAD).
                if (gltf::overlay_kind_of(material.name) != gltf::overlay_kind::none &&
                    row->others.contains("_GameToon0")) {
                    auto const base = row->others.find("_BaseColor");
                    if (base != row->others.end()) {
                        if (auto const tint = parse_toon_colour(base->second, 4); tint && tint->a <= 1.0f) {
                            material.factors.base_color_factor =
                                glm::vec4(toon_srgb_decode(tint->r), toon_srgb_decode(tint->g),
                                          toon_srgb_decode(tint->b), tint->a * tint->a);
                        }
                    }
                    continue;
                }
                // The emissive staging is the CLOTH family's only: the reference's body/face/hair materials
                // state `_UseEmission 0` and their glow is the albedo's, so a material that switches the
                // flag off keeps the slot glTF gave it. A material that already HAS an emissive texture is
                // the artist's own choice and is left alone.
                if (!row->enabled_by_flag("_UseEmission") ||
                    gltf::toon_family_of(material.name) != gltf::toon_family::cloth ||
                    material.texture_indices.contains("emissive")) {
                    continue;
                }
                // `_GameToon0` is the per-material captured profile vector; its FIRST field is the cloth
                // flag (`1` when the game actually ran this material's emissive path). The row exists for
                // every material, so the gate is the value rather than the row's presence.
                auto const profile = row->others.find("_GameToon0");
                if (profile == row->others.end()) {
                    continue;
                }
                auto const fields = std::string_view(profile->second);
                float const captured_cloth = parse_number(fields.substr(0, fields.find(','))).value_or(0.0f);
                if (!std::isfinite(captured_cloth) || captured_cloth <= 0.5f) {
                    continue;
                }
                auto const map = row->slot("_EmissionMap");
                auto const texture = model->texture_index_by_name(map);
                float const brightness = row->scalar("_EmissionBrightness", 1.0f);
                if (!texture || !std::isfinite(brightness) || brightness <= 0.0f) {
                    continue;
                }
                glm::vec3 tint(0.0f);
                if (auto const colour = row->others.find("_EmissionColor"); colour != row->others.end()) {
                    auto const value = parse_toon_colour(colour->second, 3);
                    if (!value) {
                        continue;
                    }
                    tint = glm::vec3(toon_srgb_decode(value->r), toon_srgb_decode(value->g), toon_srgb_decode(value->b));
                }
                material.texture_indices.emplace("emissive", *texture);
                material.factors.emissive_factor = tint * brightness;
                utility::log("toon emission: '{}' uses '{}' at brightness {}", material.name, map, brightness);
            }
        }
        // A sidecar'd asset keeps its raw decoded pixels alive: the toon lanes read them through the
        // lookup during the import, and `consume_texture_pixels` would have moved them into the mip
        // chains already (the mip chain is what the ordinary PBR slots sample, so nothing else needs
        // them once the toon lookup is installed).
        auto materials = gltf::resolve_materials(*model, sidecar == nullptr);
        auto const center = (bounds.min + bounds.max) * 0.5F;
        auto const radius = glm::length(bounds.max - bounds.min) * 0.5F;
        std::array<float, 3> const import_shift{-center.x, -center.y - radius, -center.z};
        // Where the bounds' centre lands once the import shift is applied - the sphere the shadow
        // fit frames (HEAD's single-object `scene_sink`, which is (0, -radius, 0)).
        std::array<float, 3> const scene_center{center.x + import_shift[0],
                                                center.y + import_shift[1],
                                                center.z + import_shift[2]};
        // THE FACE FRAME, measured once here rather than on every query: it picks the face-family mesh
        // and averages its UV side bands, and the answer cannot change after the load.
        deren::gltf::face_frame const face_frame = deren::gltf::face_frame_of(*model, 0);
        if (face_frame.valid) {
            utility::log("face frame: '{}' forward ({:.3f}, {:.3f}, {:.3f}) right ({:.3f}, {:.3f}, {:.3f}) head-relative {}",
                         entry->id, face_frame.forward.x, face_frame.forward.y, face_frame.forward.z,
                         face_frame.right.x, face_frame.right.y, face_frame.right.z, face_frame.head_relative);
        }
        auto const handle = state.next_asset_id++;
        utility::vessel::vector<std::string> pipelines;
        pipelines.reserve(entry->pipelines.size());
        for (std::string const& pipeline : entry->pipelines) {
            pipelines.push_back(pipeline);
        }
        state.assets.push_back(managed_asset{
            .id = handle,
            .asset_id = entry->id,
            .model = std::move(model),
            .materials = std::move(materials),
            .sidecar = std::move(sidecar),
            .toon = std::move(toon),
            .cloud = nullptr,
            .face_frame = face_frame,
            .import_shift = import_shift,
            .scene_center = scene_center,
            .scene_radius = radius,
            .default_pipeline = entry->default_pipeline,
            .pipelines = std::move(pipelines),
            .references = 1});
        *asset = {.value = handle};
        return scripting::script_status::success;
    }

    scripting::script_status script_scene::release_asset(scripting::asset_handle const asset) {
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

    scripting::script_status script_scene::bind_asset(std::uint64_t const tree, scripting::node_handle const parent,
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
        if (engine::scene_tree::components().try_emplace<pipeline_binding>(to_entity(wrapper_handle),
                                                                           pipeline_binding{source->pipelines}) == nullptr) {
            scene->destroy_node(wrapper->node_id);
            return scripting::script_status::invalid_node;
        }

        if (is_active) {
            auto* owner = tree_entry == nullptr ? nullptr : tree_entry->owner;
            managed_asset_source const asset_source{
                .scenes = source->model.get(),
                .cloud = source->cloud.get(),
                .materials = source->materials,
                .import_shift = source->import_shift,
                .scene_center = source->scene_center,
                .scene_radius = source->scene_radius,
                .toon = source->toon.get(),
                .default_pipeline = source->default_pipeline,
                .pipelines = source->pipelines};
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

    scripting::script_status script_scene::get_transform(scripting::node_handle node, scripting::transform_value* value) {
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

    scripting::script_status script_scene::set_transform(scripting::node_handle node, scripting::transform_value const* value) {
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

    scripting::script_status script_scene::try_spawn(std::uint64_t const scene, scripting::utf8_view asset,
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

    scripting::script_status script_scene::try_despawn(std::uint64_t const scene, scripting::node_handle node) {
        static_cast<void>(scene);
        static_cast<void>(node);
        return scripting::script_status::spawn_unavailable;
    }
} // namespace deren::app