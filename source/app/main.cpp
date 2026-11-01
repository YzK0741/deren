#define GLFW_INCLUDE_NONE // glfw3.h must not pull <GL/gl.h>: we link no OpenGL, and this states it instead of relying on the environment
#include "../engine/lighting/environment_controls.hpp"
#include "../engine/lighting/radiance_capture.hpp"
#include "../engine/lighting/reflection_probe.hpp"
#include "radiance_cube_capture.hpp"
#include "startup_splash.hpp"

#include <GLFW/glfw3.h> // the application's window: this file creates it and the backend only binds to it (see glfw_window_host)
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
// ---- THE GOO REFERENCE'S PRE-INTEGRATED FGD LUT IS A PNG, AND THIS IS THE DECODER ----
//
// `source/third_party/stb/stb_image.h` is already vendored and already used by `gltf_loader.cpp` for GLB textures, and
// that translation unit defines `STB_IMAGE_IMPLEMENTATION` (whose functions are `STBIDEF`, i.e. `extern`). A
// SECOND implementation in this file is therefore not a double definition but a LINK CLASH, and the fix is the
// documented one: `STB_IMAGE_STATIC` makes this file's copy file-local, so the two never meet at link time. It
// costs a second copy of ~7 KB of decode routines and buys the one thing a global image needs - a decoder at the
// point of use, without making the engine core depend on `gltf_loader` (see `deren_engine`'s own note on why
// that dependency is deliberately absent).
//
// The include path resolves because `deren_engine` exports `source/third_party/` as a PUBLIC include directory, and
// `deren` links it.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
import deren.utility.shared;
import deren.vstd;
import deren.application_configuration;
import deren.chores; // demo bootstrap helpers (shader loading / dir locating / pipelines)
import deren.gltf_loader;
import deren.gaussian_loader;                  // ... and a Gaussian splat cloud, when the model path names a .ply
import deren.engine.gaussian_splatting.raster; // the cloud's immutable GPU asset (make_raster_asset) and the snapshot the runtime prepares
import deren.toon_material_sidecar;            // the .toon.tsv a character model carries: its toon maps, and the _Use flags
import deren.utility;                          // re-exports deren.utility:frame_clock / frame_stats / bvh / vessel / thread_pool / data_block
import deren.engine.animation;
import deren.engine.animation.mmd_motion; // VMD (MMD motion) parsing, retargeting and clip baking      // animation::controller: glTF playback / skinning / morphs on the runtime tree
import deren.engine.math;
import deren.engine.scene_tree; // scene storage + GPU primitives (was vulkan.model)
// the backend-creation contract (create_info): what this app hands the runtime below
import deren.promise.rhi;
#include "environment_loader.hpp"
import deren.engine.runtime;
import deren.engine.backend_loader;    // load_api_core(): the app owns the acquisition (batch ⑥)
import deren.engine.render_start_demo; // the example's pass wiring: this app's chain, from outside the renderer
#include "toon_material.hpp"

namespace {

    // ---- THE APPLICATION'S WINDOW, AND GLFW'S LIFETIME (the window moved out of the backend) ----
    //
    // THE WINDOW BELONGS TO THE APPLICATION NOW: this type initialises GLFW, creates the window the frame
    // is presented in, and destroys both on the way out. The engine only BINDS to it - the pointer travels
    // through `create_info::native_window`, the core builds its surface from it and never creates and
    // never destroys a window. A null handle selects rendering into ordinary images.
    //
    // THE DECLARATION ORDER IS THE CONTRACT: an instance of this type is constructed BEFORE the runtime, so
    // that it is destroyed AFTER it. The core's surface was created from this window, and GLFW's
    // `glfwTerminate` destroys every remaining window - so tearing the window (or GLFW) down while the
    // runtime is still alive would pull it out from under a live surface. Default local destruction order
    // (reverse of construction) gives exactly the order this needs.
    //
    // Scripted captures use no window. The explicit --capture-windowed regression override
    // creates a hidden window so resize and presentation can still be tested.
    struct glfw_window_host {
        GLFWwindow* window = nullptr;

        glfw_window_host(int32_t const width, int32_t const height, char const* const title, bool const visible, bool const offscreen = false) {
            if (offscreen) {
                return;
            }
            // THE BACKEND USED TO CALL THIS AND IGNORE THE RESULT (`core::init_window`'s bare `glfwInit()`).
            // The application owns the initialisation now, so a failure is reported where it happens instead
            // of surfacing later as a null extension list or a window that never appears.
            if (glfwInit() != GLFW_TRUE) {
                deren::utility::panic("glfwInit failed");
            }
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
            glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
            this->window = glfwCreateWindow(width, height, title, nullptr, nullptr);
            if (this->window == nullptr) {
                deren::utility::panic("glfwCreateWindow failed");
            }
            // belt and braces, as in `core::init_window`: a platform may ignore the hint, so the visibility
            // is also enforced explicitly (both calls are idempotent)
            if (visible) {
                glfwShowWindow(this->window);
            } else {
                glfwHideWindow(this->window);
            }
        }
        // one owner: the pointer is handed to the engine as a BORROWED handle, so a second owner of the
        // same window (a copy) would destroy it twice
        glfw_window_host(glfw_window_host const&) = delete;
        glfw_window_host& operator=(glfw_window_host const&) = delete;
        ~glfw_window_host() {
            if (this->window != nullptr) {
                glfwDestroyWindow(this->window);
            }
            if (this->window != nullptr) {
                glfwTerminate();
            }
        }
    };

    // ---- scripted capture (dev tool) ----
    // Verifying anything visual used to need a human at the keyboard (F12). Two flags remove
    // that: `--capture-frames <n>` renders n frames, saves a screenshot through EXACTLY the F12
    // path (same read-back, same PNG writer, same logged line) and quits; `--capture-camera
    // <yaw,pitch,distance>` overrides the orbit camera at startup (degrees / scene units) so a
    // specific view - e.g. looking down-sun, where a shadow leak shows - reproduces on demand.
    // Both are stripped from argv before app_config sees them, so the positional model /
    // grid-side slots keep their meaning.
    struct capture_options {
        deren::app::radiance_capture::options radiance;
        bool windowed = false;                                                        // explicit window-system regression captures
        int32_t frames = 0;                                                           // 0 = normal interactive run
        std::optional<deren::utility::vessel::array<float, 3>> camera = std::nullopt; // yaw(deg), pitch(deg), distance
        std::optional<glm::vec3> target = std::nullopt;                               // orbit target override (optional)
        // Degrees of YAW added per presented frame (`--capture-sweep`). 0 - the default - is a fixed
        // camera, which is what every other capture in this repository is.
        //
        // WHY IT EXISTS: with the camera still, every reprojection path in the renderer is exercised only
        // in its trivial case - a motion vector of zero, a history fetched from the pixel it came from.
        // TAA's resolve, the GI temporal accumulation, the reflection's history and the velocity target
        // itself are all "correct" under a static camera no matter how they are written, so a break in
        // any of them passed the capture harness. This makes a capture move the camera by a fixed amount
        // per FRAME INDEX (not per wall-clock second), so a sweep capture is as reproducible as a still
        // one - the gate compares two runs of it like any other scenario.
        float sweep_yaw_deg_per_frame = 0.0f;
        // Seconds of ANIMATION time added per PRESENTED frame (`--capture-animation-sweep`). 0 - the
        // default - leaves playback on the wall clock, which is what an interactive run wants.
        //
        // WHY IT EXISTS, and it is the camera sweep's argument one member up, for the other half of the
        // frame: a capture of a DEFORMING mesh is only reproducible if the pose is a function of the frame
        // index. [render] animation_time makes such a capture reproducible and USELESS for this purpose - a
        // pinned pose uploads the same skin matrices every frame, so the previous-frame deformation equals
        // the current one and the deformation term of the motion vector is exactly zero, which is why a
        // pinned-pose screenshot cannot tell a deformation-aware renderer from the one that ignores
        // deformation. The wall clock (frame_clock::delta_seconds()) is reproducible in neither direction.
        // So playback gets the frame-indexed clock the camera already has: the pose is a function of the
        // frames PRESENTED, and two runs of one animation sweep are byte-identical.
        float animation_seconds_per_frame = 0.0f;
    };

    // strtof with a full-string check (no exceptions: std::stof would abort under -fno-exceptions)
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

    capture_options parse_capture_options(int32_t const argc, char** argv, deren::utility::vessel::vector<char*>& filtered) {
        capture_options options = {};
        filtered.push_back(argv[0]);
        // "--flag value" or "--flag=value"; returns the value and advances i past it
        auto const take_value = [&](int32_t& i, std::string_view const arg, std::string_view const name) -> std::optional<std::string_view> {
            if (arg == name) {
                return i + 1 < argc ? std::optional<std::string_view>(argv[++i]) : std::nullopt;
            }
            if (arg.size() > name.size() && arg[name.size()] == '=' && arg.starts_with(name)) {
                return arg.substr(name.size() + 1);
            }
            return std::nullopt;
        };
        for (int32_t i = 1; i < argc; ++i) {
            std::string_view const arg(argv[i]);
            auto const key = arg.substr(0, arg.find('='));
            if (key == "--capture-radiance-cube" || key == "--probe-position" || key == "--probe-size" || key == "--probe-near" || key == "--probe-far") {
                auto value = take_value(i, arg, key);
                if (!value)
                    options.radiance.invalid = true;
                else
                    deren::app::radiance_capture::parse_option(options.radiance, key, *value);
                continue;
            }
            if (arg == "--capture-windowed") {
                options.windowed = true;
                continue;
            }
            // --mmd-motion is consumed here even though main scans argv for it itself: the app's
            // parser would otherwise treat the flag as the positional model path and panic with
            // 0xC0000409 (exactly what an unknown first positional does).
            if (take_value(i, arg, "--mmd-motion").has_value()) {
                continue;
            }
            if (std::optional<std::string_view> const value = take_value(i, arg, "--capture-frames")) {
                if (std::optional<float> const frames = parse_number(*value)) {
                    options.frames = static_cast<int32_t>(std::max(0.0f, *frames));
                } else {
                    deren::utility::log("capture: ignoring '--capture-frames {}' (expected a frame count)", *value);
                }
                continue;
            }
            if (std::optional<std::string_view> const value = take_value(i, arg, "--capture-camera")) {
                // yaw,pitch,distance[,target.x,target.y,target.z] - comma-separated numbers
                deren::utility::vessel::array<float, 6> parsed = {};
                std::size_t cursor = 0;
                std::size_t count = 0;
                bool valid = true;
                while (cursor <= value->size()) {
                    std::size_t const comma = value->find(',', cursor);
                    std::string_view const piece = value->substr(cursor, comma == std::string_view::npos ? std::string_view::npos : comma - cursor);
                    std::optional<float> const number = parse_number(piece);
                    if (!number || count == parsed.size()) {
                        valid = false;
                        break;
                    }
                    parsed[count++] = *number;
                    if (comma == std::string_view::npos) {
                        break;
                    }
                    cursor = comma + 1;
                }
                if (valid && (count == 3 || count == 6)) {
                    options.camera = deren::utility::vessel::array<float, 3>{parsed[0], parsed[1], parsed[2]};
                    if (count == 6) {
                        options.target = glm::vec3(parsed[3], parsed[4], parsed[5]);
                    }
                } else {
                    deren::utility::log("capture: ignoring '--capture-camera {}' (expected yaw,pitch,distance[,target.x,target.y,target.z])", *value);
                }
                continue;
            }
            if (std::optional<std::string_view> const value = take_value(i, arg, "--capture-sweep")) {
                // degrees of yaw per presented frame - see capture_options::sweep_yaw_deg_per_frame
                if (std::optional<float> const number = parse_number(*value)) {
                    options.sweep_yaw_deg_per_frame = *number;
                } else {
                    deren::utility::log("capture: ignoring '--capture-sweep {}' (expected degrees of yaw per frame)", *value);
                }
                continue;
            }
            if (std::optional<std::string_view> const value = take_value(i, arg, "--capture-animation-sweep")) {
                // seconds of animation time per presented frame - see capture_options::animation_seconds_per_frame
                if (std::optional<float> const number = parse_number(*value)) {
                    options.animation_seconds_per_frame = *number;
                } else {
                    deren::utility::log("capture: ignoring '--capture-animation-sweep {}' (expected seconds per frame)", *value);
                }
                continue;
            }
            filtered.push_back(argv[i]);
        }
        if (options.frames > 0) {
            deren::utility::log("capture mode: {} frames, then screenshot + quit", options.frames);
            if (options.sweep_yaw_deg_per_frame != 0.0f) {
                deren::utility::log("capture camera sweep: {:.3f} deg of yaw per frame, from whatever pose the scene settled on", options.sweep_yaw_deg_per_frame);
            }
            if (options.animation_seconds_per_frame != 0.0f) {
                deren::utility::log("capture animation sweep: {:.4f} s of animation per frame, from the clip's own start", options.animation_seconds_per_frame);
            }
        }
        return options;
    }

} // namespace

// `int`, NOT `int32_t`: the C++ standard requires main's own signature to use the keyword, and this is the one
// place in the tree where that is a LANGUAGE rule rather than a choice about the project's arithmetic types.
int main(int argc, char** argv) {
    static_cast<void>(deren::utility::diagnostics::install_crash_handler());
    // --version: print the version (single source: project(VERSION) in CMakeLists.txt, injected
    // as DEREN_VERSION_*) and exit before any config / Vulkan init.
    for (int32_t i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--version") {
            deren::utility::println("deren {}.{}.{}", DEREN_VERSION_MAJOR, DEREN_VERSION_MINOR, DEREN_VERSION_PATCH);
            return 0;
        }
    }

    // 1-3. Resolve the startup config in one step (chores): merge the config file (config.toml
    // by default, --config <path> to override) with positional argv overrides (argv[1] = model,
    // argv[2] = grid side (numeric)), then locate the source/shaders/ dir and pick the model file.
    // Panics on any missing configured/located resource. The dev-tool capture flags are removed
    // from argv first (see parse_capture_options) so they cannot land in the positional slots.
    deren::utility::vessel::vector<char*> filtered_argv;
    capture_options const capture = parse_capture_options(argc, argv, filtered_argv);
    auto const& probe = capture.radiance;
    if (!deren::app::radiance_capture::valid(probe) || (probe.enabled && (capture.frames || capture.windowed || capture.camera || capture.sweep_yaw_deg_per_frame || capture.animation_seconds_per_frame))) {
        deren::utility::error("invalid or conflicting radiance capture arguments");
        return 2;
    }
    deren::chores::startup_config config = deren::chores::analyse_config(static_cast<int32_t>(filtered_argv.size()), filtered_argv.data());
    auto& settings = config.settings;
    if (probe.enabled) {
        if (settings.render.character_forward || settings.render.game_toon || settings.render.subtilis || settings.render.software_raster || !settings.render.background_glb.empty() || std::getenv("DEREN_HZB_CULL") || std::getenv("DEREN_PROGRESSIVE_TEXTURES")) {
            deren::utility::error("radiance capture requires ordinary static PBR without background models or experimental culling");
            return 2;
        }
        settings.render.window_width = settings.render.window_height = probe.size;
        settings.render.offscreen = true;
        settings.render.render_scale = 1;
        settings.render.taa = settings.render.ssao = settings.render.unlit = settings.render.gbuffer_debug = false;
        settings.render.visibility_texture_streaming = false;
    }
    std::filesystem::path const& shaders_dir = config.shaders_dir;
    deren::utility::vessel::string const& model_path = config.model_path;
    // Keep the splash younger than its GLFW owner, including early-exit teardown.
    bool const offscreen = settings.render.offscreen || (capture.frames > 0 && !capture.windowed);
    glfw_window_host const window{settings.render.window_width, settings.render.window_height,
                                  settings.render.window_title.c_str(), capture.frames == 0, offscreen};
    deren::app::startup_splash splash{capture.frames == 0 && !settings.render.offscreen};
    splash.attach_owner(window.window);

    // startup banner: version (single source: project(VERSION) in CMakeLists.txt)
    deren::utility::log("deren {}.{}.{}", DEREN_VERSION_MAJOR, DEREN_VERSION_MINOR, DEREN_VERSION_PATCH);

    // 4. Kick off the runtime-independent heavy CPU stages BEFORE constructing the (heavy)
    //    Vulkan runtime, so window/instance/device/swapchain init overlaps the model parse +
    //    texture decode and the base environment cubemap generation. IBL resolutions come from
    //    the [lighting] config (smaller = faster startup, larger = higher quality).
    auto const env_size = settings.lighting.env_size;
    auto const env_mip_count = settings.lighting.env_mip_count;
    auto const irr_size = settings.lighting.irr_size;
    auto const lut_size = settings.lighting.lut_size;
    auto const startup_start = std::chrono::steady_clock::now();

    // ---- THE ENVIRONMENT IMAGE, WHEN THE CONFIG NAMES ONE ([lighting] environment_hdr) ----
    //
    // This is the reference package's own route to an environment: its world is an equirectangular HDRI
    // (`lighting/studio_01_1k.exr`) that its manifest gives a `world_strength` of 0.35, not a procedural
    // sky. The engine's decoder is `stb_image` (see the include note at the top of this file), which reads
    // Radiance `.hdr` and NOT OpenEXR, so the package's file is converted once, offline, into exactly that
    // format - `deren-ab/bg/_hdr_convert.py`, which is also where its own statistics were measured. The
    // engine gains no dependency: it gains a file it could already read.
    //
    // The pixels are MOVED into the async task that bakes the cubemap, so no buffer here has to outlive the
    // statement that loaded it. An empty path means the analytic sky, untouched.
    deren::utility::vessel::vector<float> environment_pixels = {};
    int32_t environment_width = 0;
    int32_t environment_height = 0;
    if (!settings.lighting.environment_hdr.empty()) {
        std::filesystem::path environment_path = settings.lighting.environment_hdr;
        if (environment_path.is_relative()) {
            // relative to the EXECUTABLE's directory, the same rule [render] background_glb follows
            environment_path = deren::utility::executable_directory() / environment_path;
        }
        int32_t channels = 0;
        float* const decoded = stbi_loadf(environment_path.string().c_str(), &environment_width, &environment_height, &channels, 4); // 4 = force RGBA, the layout the bake reads
        if (decoded == nullptr) {
            deren::utility::panic(std::source_location::current(), "failed to load the environment HDRI '{}': {}", environment_path.string(), stbi_failure_reason());
        }
        environment_pixels.assign(decoded, decoded + static_cast<std::size_t>(environment_width) * static_cast<std::size_t>(environment_height) * 4);
        stbi_image_free(decoded);
        // The image's own mean radiance is logged BEFORE the bake, because it turns the multiplier's effect
        // on the ambience into arithmetic instead of a guess - and it is the number the offline conversion
        // script prints for the same file, which is how the two ends are checked against each other.
        double luma_sum = 0.0;
        for (std::size_t i = 0; i + 2 < environment_pixels.size(); i += 4) {
            luma_sum += 0.2126 * environment_pixels[i] + 0.7152 * environment_pixels[i + 1] + 0.0722 * environment_pixels[i + 2];
        }
        double const texel_count = static_cast<double>(environment_width) * static_cast<double>(environment_height);
        double const luma_mean = texel_count > 0.0 ? luma_sum / texel_count : 0.0;
        deren::utility::log("environment: '{}' loaded as {}x{} RGBA float, linear luma mean {:.5f}; intensity {:.2f} (the reference package's own world_strength) scales it to {:.5f}",
                            environment_path.string(),
                            environment_width,
                            environment_height,
                            luma_mean,
                            settings.lighting.environment_intensity,
                            luma_mean * settings.lighting.environment_intensity);
        deren::utility::log("environment: the procedural sky's gradient and its baked sun are off for the IBL; the DIRECT light ([lighting] sun_direction) still drives the shadows, the shading and the visible disc");
    }

    // THE SUN, AS ONE VECTOR FOR EVERYTHING THAT HAS TO AGREE ABOUT IT: the light UBO built from it (the
    // shading's `light_dir` and the shadow cascades), the disc the sky draws - which reads that UBO's
    // `light_dir`, so the sky cannot put its sun where the shadows do not fall - and the env cubemap baked
    // below, whose sun has to sit in the same place for a reflection's glint to match the sky it reflects.
    // (That last consumer is the analytic sky's alone: an environment image carries its own lights, and the
    // direct light is not baked into it a second time.)
    auto const& sun_direction = settings.lighting.sun_direction;
    auto env_future = environment_pixels.empty()
                          ? deren::engine::generate_environment_cubemap_async(env_size, sun_direction)
                          : deren::engine::generate_environment_cubemap_from_equirect_async(std::move(environment_pixels), environment_width, environment_height, env_size, settings.lighting.environment_intensity);
    // WHICH LOADER READS THE MODEL IS THE PATH'S OWN EXTENSION (app_config::model_format_of), decided HERE and
    // before anything is opened, so "what is this file?" has one answer - and a path this build cannot read is
    // NAMED as such rather than handed to the scene importer, which would answer with a parse error about a
    // format the file never claimed to be.
    deren::app_config::model_format const model_format = deren::app_config::model_format_of(model_path);
    if (model_format == deren::app_config::model_format::unsupported) {
        deren::utility::panic(std::source_location::current(),
                              "model '{}' has no extension this build reads: name a .gltf/.glb scene or a .ply gaussian splat cloud",
                              model_path);
    }
    // A PLY renders directly in an empty mesh scene. Shared frame resources are prepared
    // by the runtime independently of geometry; the glTF path keeps its asynchronous load.
    bool const gaussian_only = model_format == deren::app_config::model_format::gaussian_ply;
    auto load_future = gaussian_only ? std::future<std::expected<deren::gltf::scenes, deren::gltf::error_code>>{}
                                     : deren::gltf::load_model_async(model_path, {settings.render.subtilis});

    // 5. THE APPLICATION ACQUIRES THE DEVICE ROOT, THEN BUILDS THE RENDERER FROM IT (batch ⑥). Three
    //    steps, in this order, and the split is the point: the loader is the app's decision (which
    //    backend, from where, with which search rules, and what a failure means for startup) and the
    //    runtime is the renderer, which loads nothing and refuses an empty root loudly.
    //    THE TYPE IS THE RHI CONTRACT'S, and it is the ONLY creation structure there is: the program
    //    fills `deren::promise::rhi::create_info` (source/promise/rhi/rhi.core_desc.cppm), hands it to
    //    `deren::engine::load_api_core()` - which loads deren_vulkan.dll by absolute path and passes
    //    this structure, unchanged except for the window handle it translates for the wire - and then
    //    hands BOTH the root and the same structure to the runtime. There is no backend-side twin to
    //    translate into, so a new field is added in exactly one place.
    deren::promise::rhi::create_info core_options = {};
    core_options.window_width = settings.render.window_width;
    core_options.window_height = settings.render.window_height;
    // the title is a BORROWED `char const*` (a deren::utility::vessel::string cannot cross the boundary): it has to outlive the
    // core's construction. `settings` is a reference to `config.settings`, which lives to the end of main, so
    // this `.c_str()` is valid for the whole run - and it must never be a TEMPORARY's `.c_str()`, whose text
    // would be gone by the time the core reads it.
    core_options.window_title = settings.render.window_title.c_str();
    core_options.vsync = settings.render.vsync;
    core_options.validation_layers = settings.render.validation_layers;
    // the render scale is a CREATION option and not a runtime setter, because it decides the extent every
    // render target is created with (see create_info::render_scale): it has to be in the options the
    // core is constructed from, and it applies from the first frame.
    core_options.render_scale = settings.render.render_scale;
    // ---- A SCRIPTED CAPTURE GETS NO WINDOW, which is the one place a human would have seen one. The
    //      capture flags exist so a visual check needs nobody at the keyboard (see capture_options above),
    //      and the gate / the A/B batches then run this binary twenty to a hundred times in a row: with a
    //      visible window that is a row of flashes on the screen of whoever is using the machine. The
    //      result is unaffected BY CONSTRUCTION - the screenshot is a read-back of the swapchain image the
    //      frame rendered into, not a capture of the window - and the gate's references plus the four
    //      recorded A/B anchors are byte-identical with this line and without it, which is the measurement
    //      behind the claim. The interactive path (`capture.frames == 0`) is untouched: a normal window.
    core_options.window_visible = capture.frames == 0;
    // ---- AND THE WINDOW IS HANDED OVER HERE: the pointer the application created above (and keeps alive for
    //      the whole run) selects the core's caller-window branch - it binds to that window, creates none of
    //      its own and destroys none, so the window is still there when the runtime (and its surface) goes
    //      down. `window_visible` above is now redundant for THIS path (a caller's window is bound as-is),
    //      but it is still filled because the contract carries the field and the fallback path - no window
    //      handed over - reads it; the visibility this run actually gets is the `GLFW_VISIBLE` hint on the
    //      window created above.
    core_options.native_window = window.window;
    // ---- STEP ONE: LOAD THE BACKEND AND ASK IT FOR A DEVICE ROOT. `load_api_core` opens
    //      `deren_vulkan.dll` beside this executable (absolute path, so the narrow LOAD_LIBRARY_SEARCH_*
    //      rules apply and %PATH% is never consulted), resolves its ONE export, keeps the image mapped
    //      for the life of the process, and performs the creation-side handshake with
    //      `rhi::abi_version`. Every failure is a NAMED diagnosis on the log before it returns, so the
    //      empty answer below is the app's decision to make rather than a mystery.
    deren::promise::rhi::shared_object<deren::promise::rhi::api_core> core = deren::engine::load_api_core(core_options);
    if (core == nullptr) {
        // THE APPLICATION DECIDES WHAT A FAILED ACQUISITION MEANS, and without a backend there is no
        // renderer to build: report the same way the rest of the startup path does and exit non-zero
        // (the loader has already named the reason - the log is the diagnosis).
        deren::utility::error("startup: no device root was acquired, so there is no renderer to run "
                              "(see the backend_loader diagnosis above)");
        return 1;
    }
    // ---- STEP TWO: THE RENDERER, FROM THE ROOT AND THE SAME OPTIONS. It loads nothing; it checks
    //      that the object attests `rhi::abi_version` (its own `api_version()`) and refuses an empty
    //      root, both as panics - the consumption-side half of the same handshake.
    deren::engine::runtime runtime{std::move(core), core_options};
    runtime.background_color = glm::vec3(settings.render.clear_color[0], settings.render.clear_color[1], settings.render.clear_color[2]);
    // Shadow setup below initializes the CPU-side lighting configuration before the first frame.
    // per-pass GPU timings (timestamp queries): on by default, reported in the log + overlay
    runtime.set_gpu_timings(settings.render.gpu_timings);
    runtime.set_async_present(settings.render.async_present);
    // Shadow cascades: applied here (BEFORE the scene import) because the shadow map's layered image
    // is created when the first scene set binds it - see runtime::set_shadow_cascades.
    runtime.set_shadow_cascades(static_cast<uint32_t>(settings.render.shadow_cascades));
    runtime.set_shadow_cascade_blend(settings.render.shadow_cascade_blend);
    runtime.set_shadow_depth_bias(settings.render.shadow_bias_constant, settings.render.shadow_bias_slope, 0.0f);
    // shadow map edge length: same startup-only rule as the cascade count (the layered image and its
    // views are created when the first scene set binds them, so this must precede the scene import)
    runtime.set_shadow_map_size(static_cast<uint32_t>(settings.render.shadow_map_size));
    auto const runtime_ready = std::chrono::steady_clock::now();
    deren::utility::log("vulkan runtime initialized: {:.1f} ms (async model load + env generation running in background)", std::chrono::duration<double, std::milli>(runtime_ready - startup_start).count());

    // 6. Pipelines up front (deren::chores::setup_pipeline): the standard PBR pipeline (used by the
    //    imported scene) and the directional shadow pass. The legacy
    //    triangle demo pipeline is no longer created - nothing draws it.
    deren::chores::setup_pipeline(runtime, shaders_dir);
    // THE EXAMPLE'S OWN WIRING: this application's passes are fed by `deren.engine.render_start_demo`, which finds them
    // in the chain the runtime owns and answers the frame's per-stage questions (see the module's header). The
    // runtime holds none of these references itself any more, which is what lets a second application hand it a
    // different chain - and the demo object outlives the frame loop because it lives here, in the app's own scope.
    deren::engine::render_start_demo start_demo;
    static_cast<void>(start_demo.attach(runtime)); // builds this app's chain and hands it over
    // ... and the CREATE step runs over that chain (the shaders above are registered by now): every pass builds what
    // it owns, and the renderer's two jobs - which are not passes - are created with them.
    runtime.create_passes();
    // pass clamps them). OFF by default, so a stock config is the unshadowed path it always was.
    // Same bargain as the ray-traced shadows: a request the runtime grants only on a device with ray
    // queries and a built top level structure - otherwise the GI rays keep marching the depth buffer.
    // The furnace verification mode: an analytic reference rather than another estimator of ours.
    runtime.set_furnace(settings.render.furnace);
    runtime.set_rt_shadows(settings.render.rt_shadows);
    // ... and the alphaMode MASK bake, which is what keeps a masked surface from being SOLID to those rays:
    // a compute pass collapses the triangles the material's alpha cuts out, before the structures are built.
    runtime.set_rt_mask_bake(settings.render.rt_mask_bake);
    // ... and the per-frame skinning pass, which is what keeps an ANIMATED caster's traced shadow where the
    // caster actually is: the structures are built from the bind pose, so without it the ray sees the mesh
    // at rest.
    runtime.set_rt_skin_bake(settings.render.rt_skin_bake);
    // THE VISIBILITY BUFFER'S SWITCH ([render] visibility_buffer): the opaque path's frame gate. Applied here,
    // AFTER create_passes(), because that is what builds the raster and resolve pipelines the gate also asks
    // about - a config that asks for the new path while its pipelines failed logs the refusal once and the frame
    // keeps the geometry lane.
    runtime.set_visibility_buffer(settings.render.visibility_buffer);
    runtime.set_visibility_texture_streaming(settings.render.visibility_texture_streaming);
    runtime.set_subtilis(settings.render.subtilis);
    runtime.set_subtilis_error_px(settings.render.subtilis_error_px);
    runtime.set_subtilis_hysteresis(settings.render.subtilis_hysteresis);
    runtime.set_software_raster(settings.render.software_raster);
    runtime.set_software_raster_threshold_px(settings.render.software_raster_threshold_px);

    // 7. Collect the async startup results. FOR A GAUSSIAN-ONLY RUN THERE IS NO MODEL, so the slot holds an EMPTY
    //    scene: the import, the material resolve, the toon lookup and the animation state below then all work over
    //    nothing, rather than each needing a branch of its own.
    deren::gltf::scenes const no_model{};
    auto scenes = gaussian_only ? std::expected<deren::gltf::scenes, deren::gltf::error_code>{no_model} : load_future.get();
    if (!scenes) {
        deren::utility::panic(std::source_location::current(), "failed to load model '{}': error code {}", model_path, static_cast<int32_t>(scenes.error()));
    }
    if (probe.enabled) {
        bool dynamic = gaussian_only || !scenes->animations.empty() || !scenes->skins.empty();
        for (auto const& [id, node] : scenes->node_by_source)
            for (auto const& mesh : node->meshes)
                for (auto const& primitive : mesh.primitives)
                    dynamic |= !primitive.targets.empty();
        if (dynamic) {
            deren::utility::error("radiance capture refuses animation, skins, morph targets and PLY");
            return 2;
        }
    }
    deren::utility::vessel::vector<float> const env = env_future.get();
    auto const startup_done = std::chrono::steady_clock::now();
    deren::utility::log("model loaded + environment cubemap (startup window incl. runtime init): {:.1f} ms", std::chrono::duration<double, std::milli>(startup_done - startup_start).count());

    // 7b. THE GAUSSIAN CLOUD, WHEN THE MODEL PATH NAMED ONE: read from disk and packed onto the device HERE,
    //     because it is what the scene section below is told to frame. The bounds are taken through the asset's
    //     own CPU side, which is why they outlive the move into it. NO CLOUD IS BUILT ON THE MODEL PATH - there is
    //     none to build, and the stage records nothing without one.
    std::shared_ptr<deren::engine::gaussian_splatting::raster_asset const> gaussian_asset = {};
    glm::vec3 gaussian_min = glm::vec3(0.0f);
    glm::vec3 gaussian_max = glm::vec3(0.0f);
    if (gaussian_only) {
        auto cloud = deren::gaussian::load_ply(model_path);
        if (!cloud) {
            deren::utility::panic(std::source_location::current(), "failed to load the gaussian splat cloud '{}': error code {} at byte {}",
                                  model_path, static_cast<int32_t>(cloud.error().code), cloud.error().byte_offset);
        }
        deren::utility::log("gaussian: read {} splats; uploading the static tables", cloud->particles.size());
        auto asset = runtime.create_gaussian_asset(std::move(*cloud));
        if (!asset) {
            deren::utility::panic(std::source_location::current(), "failed to upload the gaussian splat cloud '{}': error code {}",
                                  model_path, static_cast<int32_t>(asset.error().code));
        }
        gaussian_asset = *asset;
        deren::gaussian::asset const& source = gaussian_asset->source();
        gaussian_min = glm::vec3{source.bounds_min[0], source.bounds_min[1], source.bounds_min[2]};
        gaussian_max = glm::vec3{source.bounds_max[0], source.bounds_max[1], source.bounds_max[2]};
        // THE REAL TOTALS, before a single frame is drawn: the design requires the viewer to report them, and the
        // acceptance cloud is 562,974 splats - not a number to guess at. The table estimate uses the contract's own
        // strides (48-byte geometry record, 192-byte SH record).
        deren::utility::log("gaussian: '{}' - {} splats, SH degree {}, bounds ({:.2f}, {:.2f}, {:.2f})..({:.2f}, {:.2f}, {:.2f}), static tables about {} MB",
                            model_path, source.particles.size(), source.sh_degree, gaussian_min.x, gaussian_min.y, gaussian_min.z, gaussian_max.x, gaussian_max.y, gaussian_max.z,
                            (source.particles.size() * (48u + 192u)) / (1024u * 1024u));
        // The viewer bouquet's flower faces point along local -Y, not along its horizontal axes.
        // Align that face with the same surface-to-sun direction used by the sky, around the cloud centre.
        glm::vec3 const gaussian_front{0.0f, -1.0f, 0.0f};
        glm::vec3 const gaussian_sun = glm::normalize(glm::vec3{sun_direction[0], sun_direction[1], sun_direction[2]});
        glm::vec3 const gaussian_axis = glm::cross(gaussian_front, gaussian_sun);
        float const gaussian_cosine = glm::clamp(glm::dot(gaussian_front, gaussian_sun), -1.0f, 1.0f);
        glm::mat4 gaussian_rotation{1.0f};
        if (glm::dot(gaussian_axis, gaussian_axis) > 1.0e-8f) {
            gaussian_rotation = glm::rotate(gaussian_rotation, std::acos(gaussian_cosine), glm::normalize(gaussian_axis));
        } else if (gaussian_cosine < 0.0f) {
            gaussian_rotation = glm::rotate(gaussian_rotation, glm::radians(180.0f), glm::vec3{1.0f, 0.0f, 0.0f});
        }
        glm::vec3 const gaussian_center = (gaussian_min + gaussian_max) * 0.5f;
        glm::mat4 const gaussian_model = glm::translate(glm::mat4{1.0f}, gaussian_center) * gaussian_rotation * glm::translate(glm::mat4{1.0f}, -gaussian_center);
        runtime.set_gaussian_asset(gaussian_asset, gaussian_model);
        glm::vec3 const gaussian_world_front = glm::vec3{gaussian_rotation * glm::vec4{gaussian_front, 0.0f}};
        deren::utility::log("gaussian: local -Y flower face aligned toward sky sun ({:.4f}, {:.4f}, {:.4f}), direction agreement {:.6f}",
                            gaussian_world_front.x, gaussian_world_front.y, gaussian_world_front.z, glm::dot(gaussian_world_front, gaussian_sun));
    }

    // 8. Whole-model world AABB + loader diagnostics (gltf_loader, pure CPU over the retained
    //    scene data): logs the scene summary (contents, hierarchy, animations/skins/morphs/
    //    cameras/lights) and returns the world bounds that frame the orbit camera and center
    //    the scene before import. Panics when the model has no drawable primitives.
    //    THE CLOUD IS THE SCENE ON THE GAUSSIAN-ONLY PATH, and the diagnostics are skipped rather than run over an
    //    empty scene (an empty scene is exactly "a model with no drawable primitive", which is what they panic on):
    //    the bounds the orbit camera, the shadow fit and the area light frame are the CLOUD'S OWN, so "frame the
    //    scene" means frame the cloud instead of a default model nobody configured.
    deren::gltf::scene_bounds bounds = {};
    if (gaussian_only) {
        // NO MODEL TO DESCRIBE, and the diagnostics panic on a scene with no drawable primitive - which an empty one
        // is. The bounds the shadow fit, the area light and the camera fit frame are the CLOUD'S OWN, which is what
        // makes "frame the scene" mean frame the cloud on this path.
        bounds.valid = true;
        bounds.min = gaussian_min;
        bounds.max = gaussian_max;
        deren::utility::log("gaussian: rendering the cloud without mesh geometry");
    } else {
        bounds = deren::gltf::log_scene_diagnostics(*scenes);
    }
    glm::vec3 const scene_center = bounds.min * 0.5f + bounds.max * 0.5f;
    float const scene_radius = glm::length(bounds.max - bounds.min) * 0.5f;

    // 8a. THE AREA LIGHT, DERIVED ONCE (see `deren::app_config::derive_area_light` and the `[lighting] area_light_*`
    //     notes): the reference package's 30 m soft box, expressed through the frame's ONE directional light.
    //     Its `position`/`target` keys are relative to the AUTHOR'S ORIGIN - the character's feet on the ground -
    //     because that is the frame the manifest was authored in ("the character stands at the origin"), and in
    //     Blender that origin is on the ground under it. This engine does not keep that origin as a quantity: it
    //     sinks the whole model by `scene_radius` (see `scene_sink` below), which lands the feet at
    //     `bounds.min.y + (-scene_center.y - scene_radius)`, with world XZ at 0. That point is what is handed
    //     over here, and it is logged so the number can be audited against the frame. Anchoring on
    //     `scene_center` instead was wrong: the bounding-box centre sits half a body higher, ~0.85 m here, which
    //     moves the emitter's visible elevation by ~6 degrees and its azimuth by ~16 degrees.
    //     Deriving it here rather than in the frame loop keeps the per-frame mirror below free of the maths;
    //     the values themselves are constant for the whole run.
    //
    //     `area_light.enabled == false` (the default: `area_light_size = 0`) means every field is zero, and
    //     the loop then multiplies the sun by 1.0 and writes a zero light_ubo pair - byte for byte the frame
    //     that existed before these keys did.
    //     The `deren::utility::vessel::array` is not decoration: `application_configuration` deliberately does not depend on glm
    //     (the derivation is unit-tested there), so this is the one place the scene's origin crosses over.
    float const scene_floor_y = bounds.min.y - scene_center.y - scene_radius;
    deren::app_config::area_light_derived const area_light =
        deren::app_config::derive_area_light(settings.lighting, deren::utility::vessel::array<float, 3>{0.0f, scene_floor_y, 0.0f});
    // `world_centre - scene_origin`, i.e. the emitter's direction FROM the author's origin, normalised by the
    // derivation: this is the sun's direction while the emitter TAKES OVER the main light, and it is why
    // `[lighting] sun_direction` is not the last word on where the light comes from then.
    //
    // THE EMITTER TAKES OVER ONLY WHEN IT CONTRIBUTES ENERGY (v1.1, the A' ruling): `area_light_irradiance`
    // is that switch, and `area_light_size == 0` (the default) is OFF outright. With `irradiance = false` the
    // area light still widens the shadow into a penumbra, but the sun keeps `gui.sun_direction` and the plain
    // `gui.sun_intensity` - the "penumbra only" mode, which is why `area_light_scale` is 1.0f there too.
    bool const area_light_takes_over = area_light.enabled && settings.lighting.area_light_irradiance;
    float const area_light_scale = area_light_takes_over ? area_light.radiance : 1.0f;
    if (area_light.enabled) {
        deren::utility::log("area light: author origin [0.000, {:.3f}, 0.000], centre [{:.3f}, {:.3f}, {:.3f}] half {:.3f} m, "
                            "axis [{:.3f}, {:.3f}, {:.3f}], radiance {:.5f}{}, penumbra {:.3f} m",
                            scene_floor_y, area_light.world_centre[0], area_light.world_centre[1], area_light.world_centre[2],
                            area_light.half, area_light.axis[0], area_light.axis[1], area_light.axis[2], area_light.radiance,
                            area_light_takes_over ? " (TAKES OVER the main light: x sun_intensity, capped at 3.0 by set_sun_intensity)"
                                                  : " (penumbra only: the sun keeps its own direction and intensity)",
                            area_light.penumbra);
    }

    // Sink the model so it sits near the world horizon (y = 0) and move the camera target with it:
    // the camera then orbits/looks at the model's position instead of the scene origin.
    glm::vec3 const scene_sink(0.0f, -scene_radius, 0.0f);
    runtime.camera.target = scene_sink;

    // 8b. THE TOON MATERIAL SIDECAR, RESOLVED AGAINST THIS MODEL'S OWN IMAGES.
    //
    //     A toon character's materials read a SECOND set of texture slots that glTF has no concept of - a
    //     diffuse ramp, a shadow LUT, a specular ramp, a matcap, a face SDF - and the `.toon.tsv` beside the
    //     model names them. Nothing connects that file to the model except IMAGE NAMES, which is why
    //     `gltf_loader` now carries them (see `texture_data::name` and `scenes::texture_index_by_name`).
    //
    //     THIS BLOCK RESOLVES THE JOIN AND REPORTS IT, which is deliberately all it does so far: it is the one
    //     place the resolution can be SEEN on a real character rather than on a test fixture, and what it
    //     prints is exactly the data the shading stage needs next - each material's family, and for every toon
    //     slot the sidecar declares, whether the artist switched it on and which of the model's images it
    //     names. The three outcomes are each reported distinctly because they are three different situations:
    //     no sidecar at all (the normal case for a model that is not a character), a slot whose flag is off,
    //     and a slot that is ON while the model has no such image - which is the case a consumer must handle by
    //     leaving the feature off rather than by substituting something.
    std::optional<deren::toon::sidecar> toon_sidecar = {}; // kept in scope: the import below is what consumes it
    {
        auto const sidecar = deren::toon::load_sidecar(model_path);
        if (!sidecar.has_value()) {
            deren::utility::log("toon sidecar: NOT READ - {}", sidecar.error());
        } else if (sidecar->empty()) {
            deren::utility::log("toon sidecar: none beside '{}' (the normal case for a model that is not a character)", model_path);
        } else {
            toon_sidecar = *sidecar;
            deren::app::toon_material::log_sidecar(*scenes, *toon_sidecar);
        }
    }

    // 9. IBL stage 2 + material resolve run concurrently via their _async wrappers: the
    //    prefilter (GGX importance sampling), irradiance map, BRDF LUT and the per-material
    //    texture decode + mip chains only depend on what we already have (env, scenes). The
    //    per-stage times are not reported individually: get() orders the waits, so only the
    //    wall-clock of the parallel stage is meaningful (the other tasks hide under the
    //    slowest one).
    deren::utility::log("generating IBL (prefilter/irradiance/BRDF LUT) + resolving materials...");
    auto const stage2_start = std::chrono::steady_clock::now();
    auto prefilter_future = deren::engine::prefilter_environment_async(env, env_size, env_mip_count);
    auto irradiance_future = deren::engine::generate_irradiance_map_async(env, env_size, irr_size);
    auto lut_future = deren::engine::generate_brdf_lut_async(lut_size);
    // Raw decoded pixels are only needed after resolution by a toon sidecar; otherwise move them into
    // the generated mip chains so the import peak does not retain both copies.
    auto resolve_future = deren::gltf::resolve_materials_async(*scenes, !toon_sidecar);

    deren::utility::vessel::vector<float> const prefiltered = prefilter_future.get();
    deren::utility::vessel::vector<float> const irradiance = irradiance_future.get();
    deren::utility::vessel::vector<float> const brdf_lut = lut_future.get();
    auto materials = resolve_future.get();
    if (!toon_sidecar) {
        auto released = deren::gltf::release_texture_pixels(*scenes);
        deren::utility::log("import memory: released {} B of decoded texture pixels; resolved mip owners remain valid", released);
    }
    auto const stage2_done = std::chrono::steady_clock::now();
    deren::utility::log("  IBL (prefilter/irradiance/BRDF LUT) + material resolve, parallel wall: {:.1f} ms", std::chrono::duration<double, std::milli>(stage2_done - stage2_start).count());

    if (!deren::engine::environment_detail::valid_half_radiance(prefiltered) || !deren::engine::environment_detail::valid_half_radiance(irradiance))
        deren::utility::panic(std::source_location::current(), "IBL exceeds finite half-float radiance; reduce environment_intensity");
    deren::utility::vessel::vector<uint8_t> const env_bytes = deren::engine::to_half_rgba(prefiltered);
    deren::utility::vessel::vector<uint8_t> const irr_bytes = deren::engine::to_half_rgba(irradiance);
    deren::utility::vessel::vector<uint8_t> const lut_bytes = deren::engine::to_half_rg(brdf_lut);

    // 10. Upload the scene-wide IBL once: shared by every primitive (bindings 2-4 of the scene block)
    runtime.set_ibl(deren::engine::ibl_input{.prefiltered_env = env_bytes, .irradiance = irr_bytes, .brdf_lut = lut_bytes, .env_size = static_cast<uint32_t>(env_size), .env_mip_count = static_cast<uint32_t>(env_mip_count), .irr_size = static_cast<uint32_t>(irr_size), .lut_size = static_cast<uint32_t>(lut_size)});

    // ---- THE ARTICLE'S POST LUT (`ZmdLutPost.shader`'s `_LutTex`), BAKED NEUTRAL ----
    //
    // A `1024x32` strip of 32 `32x32` tiles holding a `32^3` cube: the layout the article's own addressing walks
    // (blue picks the slice, red and green sit inside the tile, the tile's rows run bottom-up). THE CONTENT IS THE
    // IDENTITY UNDER THAT ADDRESSING rather than a placeholder, and the identity means INVERTING the article's
    // domain mapping: the shader encodes the frame with
    // `log2(c * 5.55555582 + 0.0479959995) * 0.0734997839 + 0.386036009` and looks THAT up, so the texel at address
    // `a` has to hold the colour `c` that encodes to `a` - `decode(a)`, below.
    //
    // AND IT IS STORED sRGB-ENCODED, because the lane is an `R8G8B8A8_SRGB` image and the sampler DECODES what it
    // reads: storing `srgb_encode(decode(a))` is what makes the shader receive `decode(a)` itself. That is the same
    // convention the shadow LUT's bake states for its own lane, and it is the difference between a neutral cube and
    // a brighter-than-neutral one.
    //
    // WHY IT IS BAKED RATHER THAN SHIPPED: the ramps, the shadow LUT and the matcap are all baked here for the same
    // reason - the lane then reads without changing the frame, so the shader's weight can default to the article's
    // 1.0 with no artist's cube present, and an artist's cube replaces the bake the day one exists.
    {
        constexpr uint32_t post_lut_width = 1024u;
        constexpr uint32_t post_lut_height = 32u;
        constexpr uint32_t post_lut_tiles = 32u;
        auto const srgb_encode = [](float const linear) {
            return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        };
        deren::utility::vessel::vector<uint8_t> post_lut(static_cast<std::size_t>(post_lut_width) * post_lut_height * 4u, 255u);
        for (uint32_t row = 0; row < post_lut_height; ++row) {
            for (uint32_t column = 0; column < post_lut_width; ++column) {
                float const address[3] = {
                    // THE AXES ARE THE ARTICLE'S, AND THEY ARE NOT THE SHADOW LUT'S - the trap this bake walked into
                    // first: `xOffset = halfColX + lut_input.r * threshold / colors` puts RED in the tile's own X,
                    // `yOffset = 1 - (halfColY + lut_input.g * threshold)` puts GREEN in the tile's Y (flipped, because
                    // v grows upward while the strip's rows grow downward), and the slice term `slice / colors` puts
                    // BLUE in the TILE INDEX. The shadow LUT's bake next door assigns those axes differently (tile =
                    // red, in-tile X = green, row = blue), and copying its layout wrote a cube whose red and blue were
                    // swapped - measured as 99.45% of the frame moving when the lookup was switched on against what
                    // was supposed to be the identity.
                    static_cast<float>(column % post_lut_tiles) / static_cast<float>(post_lut_tiles - 1u),
                    static_cast<float>(post_lut_tiles - 1u - row) / static_cast<float>(post_lut_tiles - 1u),
                    static_cast<float>(column / post_lut_tiles) / static_cast<float>(post_lut_tiles - 1u),
                };
                uint8_t* const texel = post_lut.data() + (static_cast<std::size_t>(row) * post_lut_width + column) * 4u;
                for (int32_t c = 0; c < 3; ++c) {
                    float const decoded = (std::exp2((address[c] - 0.386036009f) / 0.0734997839f) - 0.0479959995f) / 5.55555582f;
                    texel[c] = static_cast<uint8_t>(std::clamp(srgb_encode(std::clamp(decoded, 0.0f, 1.0f)), 0.0f, 1.0f) * 255.0f + 0.5f);
                }
                texel[3] = 255u;
            }
        }
        runtime.set_post_lut(post_lut, post_lut_width, post_lut_height);
    }

    // ---- THE GOO REFERENCE'S PRE-INTEGRATED FGD LUT: A GLOBAL IMAGE, UPLOADED ONCE, FROM A FILE ----
    //
    // WHY THIS ONE IS LOADED FROM DISK WHEN EVERY OTHER TOON LOOKUP IN THIS FILE IS EITHER BAKED OR INJECTED INTO
    // THE GLB, and the reason is the step-5 spec's §3.4 architecture ruling read the other way round: the bake
    // functions above exist for images THIS PORT CAN COMPUTE (`smoothstep`, an identity cube, black), and the spec
    // proves by measurement that these three FGD numbers are NOT a standard analytic form - the R channel is not
    // Karis' `scale` at any axis mapping (ratio 0.00 to 1.01 and non-monotone), the G channel is 5-35x the fit's
    // `bias` with no overlap in range, and the B channel differs from a 120k-sample Disney diffuse FGD by 2.4x
    // (§3.3). So a bake here would be the port substituting its own invented curve for the reference's authored
    // data - the one thing a port may not do - and the reference's own PNG is what travels.
    //
    // WHY IT DOES NOT GO THROUGH THE SIDECAR / `toon_slot` EITHER: it is ONE image shared by three containers with
    // a coordinate computed from shading parameters, not from a material (see `heap_slots_goo_fgd_lut`), so a lane
    // per material would be eleven rows pointing at one file. This is the "自持的全局纹理 + 自持的 heap 槽" the
    // spec's §3.4 names, and the slot is `core::heap_slots::goo_fgd_lut` (754).
    //
    // THE PATH IS RESOLVED FROM THE EXECUTABLE'S OWN DIRECTORY, not from the process's working directory: a
    // capture runs with `WorkingDirectory` set to its `-WorkDir` (see `scripts/gates/capture.ps1`), so a
    // CWD-relative path would miss. Nothing here is a new path *convention*: this file is a data file that ships
    // beside the build's own `source/shaders/` and `chars/` directories, and `deren-ab/` is where the reference's assets
    // live in this repository.
    //
    // A MISSING FILE IS LOGGED AND NOT FATAL, deliberately: with no LUT uploaded, the slot holds no descriptor and
    // `heap_texel` reads zero, which makes the step-5 arm's FGD terms zero - a frame that is WRONG in a way the log
    // names, rather than a crash in a system whose other 40 features are fine. The three terms only exist inside
    // the `goo_arm` branch, so `goo_toon = false` is unaffected either way.
    {
        deren::utility::vessel::vector<uint8_t> fgd_file = {};
        // `deren::utility::executable_directory()` AND NOT `current_path()`, which is the fix for a measured failure
        // rather than a preference: a capture runs with its working directory set to the harness's `-WorkDir`
        // (see `scripts/gates/capture.ps1`'s launch), so `current_path()` resolved to
        // `...\deren-ab\laevat\deren-ab\gooblender\images\...` and EVERY step-5 frame was rendered with no LUT
        // uploaded at all - the log said so (`goo FGD LUT: NOT uploaded`) and the three FGD terms read zero.
        // The executable's own directory is `build-release-clang64/`, which is where `deren-ab/` and the
        // reference's assets live, so the path is right whatever the process's working directory is.
        std::filesystem::path const fgd_path = deren::utility::executable_directory() / "deren-ab" / "gooblender" / "images" / "PreIntegratedFGD_GGXDisneyDiffuse.png";
        if (!fgd_path.empty()) {
            std::ifstream file(fgd_path, std::ios::binary);
            if (file) {
                fgd_file.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            }
        }
        int32_t fgd_width = 0;
        int32_t fgd_height = 0;
        int32_t fgd_channels = 0;
        stbi_uc* const fgd_pixels = fgd_file.empty()
                                        ? nullptr
                                        : stbi_load_from_memory(fgd_file.data(), static_cast<int32_t>(fgd_file.size()), &fgd_width, &fgd_height, &fgd_channels, 4); // 4 = force RGBA8
        if (fgd_pixels == nullptr) {
            deren::utility::log("goo FGD LUT: NOT uploaded - '{}' is missing or not a readable PNG. The step-5 arm's FGD terms "
                                "will read zero (see heap_slots_goo_fgd_lut); everything else is unaffected.",
                                fgd_path.string());
        } else {
            // THE BYTES ARE HANDED OVER VERBATIM AND THAT IS THE WHOLE POINT OF THE UPLOAD'S FORMAT: the
            // reference's image data-block is `colorspace = 'Non-Color'`, so Blender does not linearize it and the
            // node graph reads the raw texels. `set_goo_fgd_lut` therefore creates an `R8G8B8A8_UNORM` image, and
            // NOT the `_SRGB` one `set_post_lut` next door creates - an sRGB upload would decode all three FGD
            // outputs once and move the highlight and the ambient (spec §3.1 item 1).
            runtime.set_goo_fgd_lut(std::span<uint8_t const>(fgd_pixels, static_cast<std::size_t>(fgd_width) * static_cast<std::size_t>(fgd_height) * 4u),
                                    static_cast<uint32_t>(fgd_width),
                                    static_cast<uint32_t>(fgd_height));
            // The decode is `STB_IMAGE_STATIC`'s own allocation and is freed here rather than kept: the upload
            // copies into a device-local image, so nothing downstream reads these bytes.
            stbi_image_free(fgd_pixels);
        }
    }

    // 11. Batch-import: the runtime drives the traversal itself through two aligned loader
    //     streams — the retained node hierarchy (deren::gltf::scene_node_iterator: DFS pre-order,
    //     transform-only nodes included, name + local transform per node) and the drawables
    //     of those nodes (deren::gltf::drawable_iterator: geometry/material getters, node-aligned).
    //     The runtime rebuilds the scene tree (node per loader node) and attaches each
    //     drawable as a leaf primitive under its node, so whole-group transforms work on the
    //     imported hierarchy. The orbit camera looks at the origin, so center the scene and
    //     pull it back to fit its radius (same framing as the old single-model fit).
    //
    //     The scene tree is CALLER-OWNED: main declares it (AFTER the runtime, so C++ reverse
    //     declaration order destroys it BEFORE the runtime — the leaves' GPU buffers release
    //     through the runtime's vma allocator while it is still alive) and binds it with
    //     set_scene() before any import.
    deren::engine::scene_tree::scene scene;
    runtime.set_scene(scene);
    // Initial camera framing ([render] camera_fit). "exterior" is the historic fit: 2.75 scene radii
    // frames a compact object, and for that it stays the default. It is also useless for a building -
    // the camera ends up well outside its own walls, so the frame is a facade with no interior to
    // bounce light in, which is why GI work needs the other mode. "interior" stands inside and looks
    // along the LONGEST HORIZONTAL AXIS, because that is the axis a hall, nave or corridor runs down
    // (for Sponza: 29.8 x 18.3 in the ground plane, so the look direction is +X). The distance is a
    // fraction of the SMALLER horizontal half-extent, which is what keeps the eye inside even a
    // narrow hall while still leaving enough parallax for the view to read as a space.
    {
        glm::vec3 const half_extent = (bounds.max - bounds.min) * 0.5f;
        if (settings.render.camera_pose_set) {
            // A pinned pose from the config: the same six numbers `--capture-camera` takes, so a view a user
            // reports (printed by the F12 block below) can be pasted straight back in here.
            runtime.camera.yaw = glm::radians(settings.render.camera_pose[0]);
            runtime.camera.pitch = glm::radians(settings.render.camera_pose[1]);
            runtime.camera.distance = settings.render.camera_pose[2];
            runtime.camera.target = glm::vec3(settings.render.camera_pose[3], settings.render.camera_pose[4], settings.render.camera_pose[5]);
        }
        bool const interior = settings.render.camera_fit == "interior";
        if (settings.render.camera_pose_set) {
            // the pinned pose above already decided everything; the fit's numbers would overwrite it
        } else if (interior) {
            runtime.camera.yaw = half_extent.x >= half_extent.z ? glm::radians(90.0f) : 0.0f;
            runtime.camera.pitch = 0.0f;
            runtime.camera.distance = std::min(half_extent.x, half_extent.z) * 0.7f;
        } else {
            runtime.camera.distance = scene_radius * 2.75f;
        }
        deren::utility::log("initial camera: fit={} yaw {:.1f} deg, pitch {:.1f} deg, distance {:.2f} (scene radius {:.2f}, ground half-extent {:.2f} x {:.2f})",
                            settings.render.camera_fit,
                            glm::degrees(runtime.camera.yaw),
                            glm::degrees(runtime.camera.pitch),
                            runtime.camera.distance,
                            scene_radius,
                            half_extent.x,
                            half_extent.z);
    }
    // ONE place that turns the live pose into the six numbers the config and the command line take: the F12
    // screenshot prints it (so a bug report carries its own camera) and so does the exit below.
    auto const log_camera_pose = [&runtime](char const* const why) {
        deren::utility::log("camera pose ({}): {:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f}   -> [render] camera_pose = [...] or --capture-camera",
                            why,
                            glm::degrees(runtime.camera.yaw),
                            glm::degrees(runtime.camera.pitch),
                            runtime.camera.distance,
                            runtime.camera.target.x,
                            runtime.camera.target.y,
                            runtime.camera.target.z);
    };

    deren::gltf::scene_node_iterator const node_first = scenes->nodes_begin();
    deren::gltf::scene_node_iterator const node_last;
    deren::gltf::drawable_iterator scene_first(*scenes, materials);
    deren::gltf::drawable_iterator const scene_last;
    glm::vec3 const scene_import_shift = -scene_center + scene_sink;
    if (probe.enabled)
        deren::utility::log("radiance world position ({},{},{}), scene import shift ({},{},{})", probe.position.x, probe.position.y, probe.position.z, scene_import_shift.x, scene_import_shift.y, scene_import_shift.z);

    // THE TOON LOOKUP joins the loader's sidecar/extras data with the runtime's lookup contract. Its
    // tables, procedural bakes and three callbacks live in source/app/toon_material.cpp; it is constructed
    // here so the object outlives every material import that reads through it.
    deren::app::toon_material::lookup const toon_material_lookup{*scenes, toon_sidecar.has_value() ? &*toon_sidecar : nullptr};
    runtime.set_toon_lookup(toon_material_lookup.runtime_lookup());

    // ---- THE HEAD FRAME the face SDF shades against, resolved once here and published every frame ----
    //
    // THE LOADER IS ASKED FIRST, and the answer is a decision this code makes rather than a constant it never
    // questioned - which matters, because for the models in this repository the two answers happen to coincide.
    //
    // THE FALLBACK IS THE REFERENCE'S OWN FRAME (`EfFaceGetHeadBasis`'s `valid < 0.5` branch), so a model with
    // no usable skeleton is shaded by the answer the reference itself gives for that case rather than by a guess
    // made here. This model is exactly that case: `zhuangfy_scalar.glb` is seven nodes and zero skins, a static
    // pose split by body part, and a model that cannot turn its head has one head frame whether it is read from
    // a bone or from these three constants.
    // WHICH RIG AND WHICH JOINT, kept for the frame loop below: both are needed there to read the bone's world
    // matrix, and the joint index is the SAME number `head_joint_of` returns (an index into `skin::joints`, not
    // an asset node index) because that is what picks a matrix out of the rig's joint block.
    std::size_t head_rig = 0;
    std::optional<std::size_t> head_joint = std::nullopt;
    // ---- THE CAPTURED VIEWER'S FACE FRAME (`Q324`/`Q80`), MEASURED FROM THE FACE MESH AT LOAD ----
    //
    // WHY THIS EXISTS AT ALL: the captured viewer does not read its face frame off the head bone. Its
    // `faceFrame()` takes the FACE MESH's vertices, averages the POSITIONs of the two UV side bands (`uv.x < 0.4`
    // and `uv.x > 0.6`) to get left/right, averages the NORMALs of the middle band for the forward axis, and
    // orthogonalizes right against forward (`tmp/game_toon-studio-audit/final/responses/
    // eadba1af4f-DIaZSo8d.js`, the material manager's `faceFrame`, around byte 248129). The staged Laevatain
    // model's bone/glTF frame is `(0,0,1)` forward while its painted face measures `(-0.131,-0.391,0.911)`, ~26
    // degrees apart, and `Q80` (the UV left/right of the painted face) is not a bone axis at all - it is the axis
    // the SDF atlas is MIRRORED about, which is why the mirror threshold landed at the wrong camera yaw and the
    // face shadow popped there.
    //
    // THE GATE IS THE PUNCTUAL-RIG GATE below (SAME PREDICATE, SAME REASON): the captured frame is only published
    // for the captured GameToon preview model's own render, so every other model in this repository keeps the
    // bone/fallback head frame it had (the game chains read the same `head_ubo`). The `front.w` flag tells the
    // shader which of the two it is looking at (see `head_ubo::front`); when the loader finds no face mesh the
    // block simply is not published and `skin_face.slang` keeps its historical `-head_right` derivation.
    std::optional<deren::gltf::face_frame> captured_face_frame = std::nullopt;
    bool const publish_captured_face_frame = settings.render.game_toon && model_path.find("game_toon") != deren::utility::vessel::string::npos;
    if (publish_captured_face_frame) {
        for (std::size_t scene_index = 0; scene_index < scenes->scene.size() && !captured_face_frame.has_value(); ++scene_index) {
            deren::gltf::face_frame frame = deren::gltf::face_frame_of(*scenes, scene_index);
            if (frame.valid) {
                captured_face_frame = frame;
                deren::utility::log("face frame: captured Q324 ({:.3f} {:.3f} {:.3f}) Q80 ({:.3f} {:.3f} {:.3f}) head_relative {}",
                                    frame.forward.x, frame.forward.y, frame.forward.z, frame.right.x, frame.right.y, frame.right.z,
                                    frame.head_relative);
            }
        }
        if (!captured_face_frame.has_value()) {
            deren::utility::log("face frame: no usable face-family mesh in '{}' - the SDF keeps the bone/fallback head frame", model_path);
        }
    }
    {
        deren::gltf::head_basis head = deren::gltf::head_basis_fallback();
        for (std::size_t scene_index = 0; scene_index < scenes->scene.size() && !head_joint.has_value(); ++scene_index) {
            for (std::size_t skin_index = 0; skin_index < scenes->skins.size(); ++skin_index) {
                if (std::optional<std::size_t> const joint = deren::gltf::head_joint_of(*scenes, scene_index, skin_index); joint.has_value()) {
                    head_rig = skin_index;
                    head_joint = joint;
                    break;
                }
            }
        }
        if (head_joint.has_value()) {
            deren::utility::log("head frame: '{}' HAS a head bone at rig {} joint {} - the frame is read from it every frame", model_path, head_rig, *head_joint);
        } else {
            deren::utility::log("head frame: no head bone in '{}' ({} skin(s)) - shading from the reference's fallback frame", model_path, scenes->skins.size());
        }
        // THE FOURTH MEMBER IS A POSITION AND A FLAG, and the fallback publish leaves BOTH at zero: this branch is
        // the one a model with NO HEAD BONE takes, and for such a model there is no `HC` object to read a centre
        // from. `center.w = 0.0` is what tells the face arm that, and its answer is the socket's own default
        // (`sphereNormal_Strength = 0.0`) rather than a sphere about the world origin.
        //
        // THE CAPTURED FACE FRAME IS PUBLISHED HERE FOR FRAME 0 (the frame loop re-publishes every frame): it wins
        // over the bone/fallback frame when the loader measured one, and `front.w = 1.0` marks it as `Q324`/`Q80`.
        if (captured_face_frame.has_value()) {
            glm::vec3 const world_forward = captured_face_frame->world_forward;
            glm::vec3 const world_right = captured_face_frame->world_right;
            glm::vec3 const up = glm::normalize(glm::cross(world_right, world_forward));
            bool const has_head_position = captured_face_frame->head_relative;
            runtime.set_head_basis(deren::engine::head_ubo{
                .front = glm::vec4(world_forward, 1.0f),
                .right = glm::vec4(world_right, 0.0f),
                .up = glm::vec4(up, 0.0f),
                .center = glm::vec4(has_head_position ? captured_face_frame->world_center : glm::vec3(0.0f),
                                    has_head_position ? 1.0f : 0.0f)});
        } else {
            runtime.set_head_basis(deren::engine::head_ubo{.front = glm::vec4(head.front, 0.0f), .right = glm::vec4(head.right, 0.0f), .up = glm::vec4(head.up, 0.0f), .center = glm::vec4(0.0f)});
        }
    }

    // ---- THE TOON LIGHT RIG, published once, before the frame loop ----
    //
    // THE ONE CONFIG KNOB THE RIG TAKES SO FAR IS THE DAY STRENGTH, and it is the one the two-state lighting is
    // driven by - so it is the one an A/B capture has to be able to move. Every other rig number keeps
    // `deren::engine::toon_rig`'s own default, which is the article's value.
    //
    // PUBLISHING HERE RATHER THAN IN THE FRAME LOOP IS THE BLOCK'S CONTRACT (see core::heap_slots::toon_rig):
    // nothing rewrites it while a frame is in flight, which is what makes a write from outside the frame path
    // safe - and the cost of that is exactly this, that the write has to happen before the loop starts.
    {
        deren::engine::toon_rig rig = {};
        rig.day.x = settings.toon.day_strength;
        rig.other_light.x = settings.toon.head_light_day0;
        rig.other_light.y = settings.toon.head_light_day1;
        rig.other_colour = glm::vec4(settings.toon.head_light_colour[0], settings.toon.head_light_colour[1], settings.toon.head_light_colour[2], 0.0f);
        rig.reference_modes.x = settings.toon.hair_masked_highlights ? 1.0f : 0.0f;
        rig.reference_modes.y = settings.toon.face_angular_sdf ? 1.0f : 0.0f;
        rig.main_dark.x = settings.toon.sun_dark_colour[0];
        rig.main_dark.y = settings.toon.sun_dark_colour[1];
        rig.main_dark.z = settings.toon.sun_dark_colour[2];
        rig.env.x = settings.toon.env_strength;
        rig.env.y = settings.toon.env_rotation;
        rig.env.z = settings.toon.specular_strength;
        rig.env.w = settings.toon.diffuse_blend_effect;
        rig.rim.x = settings.toon.rim_area;
        rig.rim.y = settings.toon.rim_strength;
        rig.rim.z = settings.toon.rim_nolxz_strength;
        rig.misc.x = settings.toon.backlight_strength;
        // THE FACE'S EXPRESSION INDEX rides the head light's colour block's fourth lane, which was unused and is
        // named in `deren::engine::toon_rig` - the same zero-cost reuse the material record's `toon_family` makes.
        rig.other_colour.w = settings.toon.emotion_type;
        // THE TOON CHAIN'S SHADOW SOFTNESS LADDER (`[render] toon_shadow_softness`, already rounded and clamped
        // by `analyse_config`): 0 is the shipped 3x3 PCF and the default, so this line changes nothing unless a
        // config asked for a wider kernel. The lane is read by the shared `toon_diffuse` body in
        // `source/shaders/toon/character_forward.slang`, which is compiled into the `character_forward`, `goo_toon` and
        // `outline` stages (`goo_toon.slang`'s own `rs_shadow` lookup is a different call site and stays 3x3 at
        // every level; `overlay` has no shadow sample at all).
        rig.shadow_softness.x = settings.render.toon_shadow_softness;
        rig.shadow_softness.y = settings.toon.nose_shadow_reduction;
        rig.shadow_softness.z = settings.toon.hair_highlight_width;
        rig.shadow_softness.w = settings.toon.hair_highlight_shift;
        runtime.set_toon_rig(rig);
        deren::utility::log("toon: light rig published - day strength {:.3f}, head light {:.2f} at day 1 of {:.2f} at day 0, env strength {:.2f}, shadow softness {}",
                            rig.day.x,
                            rig.other_light.y,
                            rig.other_light.x,
                            rig.env.x,
                            static_cast<int>(rig.shadow_softness.x));
    }

    deren::engine::scene_import_result const imported = runtime.import_scene(node_first, node_last, scene_first, scene_last, scene_import_shift);
    deren::utility::log("imported {} primitives ({} new materials)", imported.primitive_count, imported.material_count);
    runtime.log_scene_tree();
    scene_first = deren::gltf::drawable_iterator{};
    decltype(materials){}.swap(materials); // streaming/lazy resources retain their shared immutable sources

    // 12b. THE FRAME'S STATIC SURROUND, when the config names one ([render] background_glb): the SAME
    //      importer runs a SECOND time into the SAME scene tree, flagged as environment. The flag is the
    //      whole feature: it takes every leaf this import creates out of the toon character stage and out
    //      of the shadow casters (and out of the shadow fit, see runtime::update_shadow_frustum), while the
    //      scene pass draws them through the ordinary PBR/unlit path - which is where the reference's own
    //      matte ground and emissive backdrop live. Nothing about the subject's own import changes.
    //
    //      WHY A SECOND IMPORT AND NOT A MODEL MERGE: the two files are authored separately, the background
    //      is optional and off by default, and the importer already appends to the scene tree (it was built
    //      to). Merging the GLBs offline would bake one machine's choice of background into the character
    //      asset - and the asset is the artist's, not ours.
    if (!settings.render.background_glb.empty()) {
        std::filesystem::path background_path = settings.render.background_glb;
        if (background_path.is_relative()) {
            // relative to the EXECUTABLE's directory, which is where every other asset of this application
            // is found (the shaders dir comes from the same place, see deren::chores::analyse_config)
            background_path = deren::utility::executable_directory() / background_path;
        }
        auto background_load = deren::gltf::load_model_async(background_path.string(), {settings.render.subtilis});
        auto background_scenes = background_load.get();
        if (!background_scenes) {
            deren::utility::panic(std::source_location::current(), "failed to load background model '{}': error code {}", background_path.string(), static_cast<int32_t>(background_scenes.error()));
        }
        deren::gltf::scene_bounds const background_bounds = deren::gltf::log_scene_diagnostics(*background_scenes);
        auto background_resolve = deren::gltf::resolve_materials_async(*background_scenes, true);
        deren::utility::vessel::vector<deren::gltf::resolved_material> const background_materials = background_resolve.get();
        // THE OFFSET IS DERIVED FROM THE SUBJECT, NOT CONFIGURED: the background's own y = 0 is its author's
        // GROUND plane and the subject's lowest point after ITS centering shift is `bounds.min.y +
        // scene_import_shift.y`, so shifting the background by the subject's shift plus that minimum puts the
        // ground exactly under the subject's feet. The two XZ centers coincide by construction: the subject's
        // own import sends `scene_center` to `scene_sink`, whose XZ is 0, and the background is authored
        // centered on its own origin. A background whose ground is not at its y = 0 does NOT land correctly -
        // that assumption is stated rather than guessed at (see the config key's note).
        glm::vec3 const background_shift = scene_import_shift + glm::vec3(0.0f, bounds.min.y, 0.0f);
        deren::gltf::scene_node_iterator const background_node_first = background_scenes->nodes_begin();
        deren::gltf::scene_node_iterator const background_node_last;
        deren::gltf::drawable_iterator const background_scene_first(*background_scenes, background_materials);
        deren::gltf::drawable_iterator const background_scene_last;
        deren::engine::scene_import_result const background_imported = runtime.import_scene(background_node_first, background_node_last, background_scene_first, background_scene_last, background_shift, true);
        deren::utility::log("background: '{}' imported as the frame's static surround - {} primitives ({} new materials), own bounds y [{:.3f}, {:.3f}], offset ({:.3f}, {:.3f}, {:.3f}); the toon character stage and the shadow casters skip it",
                            background_path.string(),
                            background_imported.primitive_count,
                            background_imported.material_count,
                            background_bounds.min.y,
                            background_bounds.max.y,
                            background_shift.x,
                            background_shift.y,
                            background_shift.z);
    }

    // 12. Enable directional shadow mapping over the imported scene: the shadow frustum frames
    //      the sphere around where the primitives actually sit (they were translated by the import
    //      offset above, so their world-space center is scene_sink) with their original radius
    runtime.enable_shadows(scene_sink, scene_radius);
    // apply the config shadow toggle now that the shadow maps exist (turning it off clears them)
    if (!settings.render.shadow) {
        runtime.set_shadow_enabled(false);
    }

    // 13. Optional instancing stress (deren::chores::add_instancing_grid): grid_side > 1 (config or
    //     argv) draws the first imported primitive as a grid_side x grid_side grid in ONE
    //     instanced draw call (the frame loop is untouched); no-op otherwise.
    deren::chores::add_instancing_grid(runtime, settings.grid_side, scene_radius);

    // 13b. Clustered-light stress ([lighting] demo_lights): spawn N procedural punctual lights on a
    //      helix around the scene bounds, pushed every frame together with the overlay's slots. The
    //      clustered path's whole point is a light count the brute-force loop could not afford, and
    //      the overlay's four slots cannot show that - this is what makes the difference measurable
    //      (and what the [render] clustered_lights A/B is compared against).
    deren::utility::vessel::vector<deren::engine::punctual_light> demo_lights;
    if (settings.lighting.demo_lights > 0) {
        int32_t const total = std::min(settings.lighting.demo_lights, static_cast<int32_t>(deren::app_config::max_demo_lights));
        demo_lights.reserve(static_cast<std::size_t>(total));
        for (int32_t i = 0; i < total; ++i) {
            float const t = static_cast<float>(i) / static_cast<float>(total);
            float const angle = t * 6.2831853f * 3.0f; // three turns around the scene
            float const radius = scene_radius * settings.lighting.demo_light_radius;
            deren::engine::punctual_light light = {};
            light.position = scene_sink + glm::vec3(std::cos(angle) * radius, scene_radius * (t - 0.5f), std::sin(angle) * radius);
            // hue cycle: a warm/cool strip of colors makes the per-cluster lists visible as color
            light.color = glm::vec3(0.5f + 0.5f * std::cos(angle), 0.5f + 0.5f * std::cos(angle + 2.094f), 0.5f + 0.5f * std::cos(angle + 4.188f));
            light.intensity = 12.0f;
            light.range = scene_radius * settings.lighting.demo_light_range; // finite range: what the cluster sphere test culls on
            demo_lights.push_back(light);
        }
        deren::utility::log("demo lights: {} procedural punctual lights around the scene (clustered light stress)", total);
    }

    // 13c. The captured GameToon viewer's character-only punctual rig (the same values as
    //      `tmp/game_toon-studio-audit/final/2fcd9c7ad0-1619b7616e7ad936.json-laevat.json`).
    //      Pushed through the ordinary `punctual_lights` UBO so the GameToon fragment loop sees deren
    //      world-space positions instead of a second, unmatchable coordinate system.
    if (settings.render.game_toon && model_path.find("game_toon") != deren::utility::vessel::string::npos) {
        struct captured_light {
            float p[3];
            float f[3];
            float c[3];
            float intensity;
            float range;
            bool spot;
            float outer_deg;
            float inner_deg;
            float mode;
            float npr0;
            float npr1;
            float npr2;
            float npr3;
        };
        static constexpr captured_light captured[] = {
            {{1.135f, 0.942f, 0.138f}, {-0.8698f, 0.0f, -0.4935f}, {0.7506f, 0.9342f, 1.0f}, 18.0f, 1.15f, true, 71.6229f, 62.2358f, 3.0f, 0.368f, 0.194f, 0.0f, 0.0f},
            {{0.6402f, 1.7082f, -0.1039f}, {-0.9756f, -0.1811f, -0.1244f}, {1.0f, 0.8274f, 0.7078f}, 25.0f, 0.555f, true, 98.307f, 74.8897f, 3.0f, 0.4f, 0.7f, 0.0f, 0.0f},
            {{0.228f, 1.5494f, 0.036f}, {-0.6932f, -0.5692f, 0.4421f}, {1.0f, 0.7321f, 0.5269f}, 1.5f, 0.2635f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
            {{0.0708f, 1.6474f, 0.0601f}, {0.4854f, -0.098f, 0.8688f}, {1.0f, 0.6695f, 0.1536f}, 0.5f, 0.2082f, false, 0.0f, 0.0f, 2.0f, 0.6f, 0.95f, 0.0f, 0.0f},
            {{-0.1233f, 1.6341f, 0.194f}, {-0.8206f, 0.0f, 0.5715f}, {1.0f, 0.0323f, 0.0119f}, 0.3f, 0.2082f, false, 0.0f, 0.0f, 2.0f, 0.6f, 0.95f, 0.0f, 0.0f},
            {{-0.2722f, 1.288f, -0.042f}, {-0.6085f, 0.0f, 0.7936f}, {1.0f, 0.6664f, 0.6969f}, 2.0f, 0.1316f, false, 0.0f, 0.0f, 2.0f, 0.6f, 0.95f, 0.0f, 0.0f},
            {{0.1016f, 1.5468f, 0.051f}, {-0.9947f, 0.0f, -0.1026f}, {1.0f, 0.7565f, 0.5754f}, 0.2f, 0.1716f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
            {{-0.105f, 1.478f, 0.353f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.9053f, 0.7874f}, 0.35f, 0.4596f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
            {{0.0211f, 1.7919f, 0.0002f}, {0.4862f, -0.0728f, 0.8708f}, {1.0f, 0.7943f, 0.3573f}, 3.0f, 0.1491f, false, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
            // FogLight_1: captured `npr:"fog"`; the last active character-only light in the rig.
            {{0.0134f, 1.0117f, -1.3375f}, {0.0f, 0.0f, 1.0f}, {0.2166f, 0.2166f, 0.2603f}, 0.4f, 1.5f, true, 86.6636f, 81.1457f, 4.0f, 0.7f, 1.0f, 0.0f, 0.0f},
        };
        for (captured_light const& c : captured) {
            deren::engine::punctual_light light = {};
            light.position = glm::vec3(c.p[0], c.p[1], c.p[2]);
            light.spot_direction = glm::vec3(c.f[0], c.f[1], c.f[2]);
            light.color = glm::vec3(c.c[0], c.c[1], c.c[2]);
            light.intensity = c.intensity;
            light.range = c.range;
            light.spot = c.spot;
            if (c.spot) {
                light.spot_outer_cos = std::cos(glm::radians(c.outer_deg) * 0.5f);
                light.spot_inner_cos = std::cos(glm::radians(c.inner_deg) * 0.5f);
            }
            light.npr_mode = c.mode;
            light.npr_data = glm::vec4(c.npr0, c.npr1, c.npr2, c.npr3);
            demo_lights.push_back(light);
        }
        deren::utility::log("GameToon rig: {} captured punctual lights pushed through the scene light UBO", std::size(captured));
    }

    // 14. Main render loop: until the window closes or ESC is pressed.
    //     Every Vulkan frame step (fences, acquire, command buffers, render pass, submit, present)
    //     lives inside runtime::render_frame()
    runtime.log_feature_status(); // one line naming every optional feature that could not be created
    deren::utility::log("rendering '{}' with PBR... left-drag to orbit, wheel to zoom, ESC to exit", model_path);

    // Dear ImGui debug overlay on by default ([gui] show)
    bool const use_gui = settings.gui.show && !offscreen;

    // FPS statistics (deren.utility:frame_stats): a rolling one-second window of frame gaps.
    // tick() once per presented frame, on_skipped() on minimized/recreate iterations, and
    // the once-per-second report (log + the overlay's smoothed value) keys off window_rolled().
    deren::utility::frame_stats frame_stats;

    // ---- keyframe animation playback + skinning + morph targets (deren.engine.animation) ----
    // The controller owns playback (sampling + writing node locals), the skin rigs (per-frame
    // joint matrices) and the morph rigs (static deltas + per-frame weights) against the
    // runtime scene tree; initialize it before the first frame (it bakes morph deltas and the
    // identity skin block into every frame slot's buffers). A float mirror of the playback
    // clock feeds the gui time slider (slider_widget binds an external float).
    deren::engine::animation::controller animation;
    // the controller drives the runtime through an injected surface (chores wires the scene,
    // per-slot buffers and task pool), so it never depends on deren::engine::runtime itself
    // ---- MMD motion (--mmd-motion): parsed, retargeted onto this model's own skeleton, and
    // appended to the loaded file's animations.  Appended rather than handed to the controller
    // because the controller takes its clip list from the scene at init() and never grows one
    // afterwards (select() only swaps the active clip), so a runtime clip has to join the
    // scene's list BEFORE init.  Names come from the pool nodes, and target_node stays the ASSET
    // node index (deren::gltf::node::source_index): animations are file-scoped, the pool index is a
    // different numbering.
    {
        deren::utility::vessel::string mmd_motion_path;
        for (int32_t i = 1; i + 1 < argc; ++i) {
            if (std::string_view(argv[i]) == "--mmd-motion") {
                mmd_motion_path = argv[i + 1];
            }
        }
        if (!mmd_motion_path.empty()) {
            std::optional<deren::engine::animation::mmd_motion> motion =
                deren::engine::animation::load_mmd_motion(mmd_motion_path);
            if (!motion.has_value()) {
                deren::utility::panic(std::source_location::current(), "failed to parse MMD motion '{}'", mmd_motion_path);
            }
            // Names come from the asset-level lookup (asset node index -> the loader's node copy),
            // walked over the skin's joints: skin::joints and animation_channel::target_node are
            // BOTH asset node indices, so pairing the two here keeps target_node exact.
            deren::utility::vessel::vector<deren::utility::vessel::string> joint_names;
            deren::utility::vessel::vector<std::size_t> joint_sources;
            for (deren::gltf::skin const& skin : scenes->skins) {
                for (std::size_t const joint_source : skin.joints) {
                    auto const found = scenes->node_by_source.find(joint_source);
                    if (found == scenes->node_by_source.end() || found->second == nullptr) {
                        continue;
                    }
                    joint_names.push_back(found->second->name);
                    joint_sources.push_back(joint_source);
                }
            }
            deren::engine::animation::mmd_retarget const retarget =
                deren::engine::animation::build_mmd_retarget(*motion, joint_names);
            deren::utility::log("mmd motion '{}': {} bones, {} mapped onto this skeleton, {} unmapped", mmd_motion_path,
                                motion->bones.size(), retarget.mapped, retarget.unmapped);
            for (std::size_t const bone : retarget.unresolved_bones) {
                if (bone < 8) {
                    deren::utility::log("  unmapped: {}", deren::engine::animation::escape_mmd_name(motion->bones[bone].name));
                }
            }
            deren::engine::animation::clip const baked = deren::engine::animation::bake_mmd_clip(*motion, retarget);
            deren::gltf::animation converted;
            converted.name = "mmd";
            converted.samplers.reserve(baked.samplers.size());
            for (deren::engine::animation::sampler const& s : baked.samplers) {
                deren::gltf::animation_sampler out;
                out.times = s.times;
                out.values = s.values;
                out.per_key = s.per_key;
                switch (s.interp) {
                case deren::engine::animation::interpolation::step:
                    out.interpolation = deren::gltf::animation_interpolation::step;
                    break;
                case deren::engine::animation::interpolation::cubic_spline:
                    out.interpolation = deren::gltf::animation_interpolation::cubic_spline;
                    break;
                case deren::engine::animation::interpolation::linear:
                default:
                    out.interpolation = deren::gltf::animation_interpolation::linear;
                    break;
                }
                converted.samplers.push_back(std::move(out));
            }
            converted.channels.reserve(baked.channels.size());
            for (deren::engine::animation::channel const& c : baked.channels) {
                deren::gltf::animation_channel out;
                out.sampler = c.sampler;
                out.target_node = joint_sources[c.target_node];
                switch (c.path) {
                case deren::engine::animation::channel_path::rotation:
                    out.path = deren::gltf::animation_path::rotation;
                    break;
                case deren::engine::animation::channel_path::scale:
                    out.path = deren::gltf::animation_path::scale;
                    break;
                case deren::engine::animation::channel_path::weights:
                    out.path = deren::gltf::animation_path::weights;
                    break;
                case deren::engine::animation::channel_path::translation:
                default:
                    out.path = deren::gltf::animation_path::translation;
                    break;
                }
                converted.channels.push_back(out);
            }
            // The controller's translation channel REPLACES a node's local translation, so the node's
            // own rest offset has to be added back: the bake emits an offset from rest, not a position.
            for (deren::gltf::animation_channel const& c : converted.channels) {
                if (c.path != deren::gltf::animation_path::translation) {
                    continue;
                }
                auto const found = scenes->node_by_source.find(c.target_node);
                if (found == scenes->node_by_source.end() || found->second == nullptr) {
                    continue;
                }
                glm::vec3 const rest = found->second->translation;
                deren::gltf::animation_sampler& s = converted.samplers[c.sampler];

                for (std::size_t i = 0; i + 2 < s.values.size(); i += 3) {
                    s.values[i] += rest.x;
                    s.values[i + 1] += rest.y;
                    s.values[i + 2] += rest.z;
                }
            }
            deren::utility::log("mmd motion: baked clip '{}' - {} samplers, {} channels", converted.name,
                                converted.samplers.size(), converted.channels.size());
            scenes->animations.push_back(std::move(converted));
        }
    }
    animation.init(*scenes, deren::chores::make_animation_backend(runtime), scene_import_shift, imported.import_instance);
    // [render] animation_time >= 0 PINS the pose: playback is wall-clock driven, so two captures of an
    // animated scene differ unless the time is fixed - and this is also what makes such a scene usable in
    // a measurement or a regression scenario at all. scrub() is the overlay's time slider, so the pose is a
    // function of the value alone; a negative value (the default) plays as always.
    if (settings.render.animation_time >= 0.0f) {
        animation.set_time(settings.render.animation_time);
        deren::utility::log("animation: pinned at {:.2f}s by [render] animation_time (playback is wall-clock driven, so captures of an animated scene are only reproducible this way)", settings.render.animation_time);
        if (capture.animation_seconds_per_frame != 0.0f) {
            deren::utility::log("capture: --capture-animation-sweep is IGNORED - [render] animation_time pins the pose, so the clock never advances (set animation_time = -1 to play)");
        }
    }
    // Live gui widget state (deren::chores::gui_bindings) is declared after the authored-camera
    // seeding below, right before deren::chores::setup_gui() builds the overlay.

    // ---- authored (glTF) camera selection ----
    // A glTF camera is used as a VIEWPOINT SEED for the orbit camera: picking one places the
    // orbit (target = scene center, distance/yaw/pitch derived from the camera node's world),
    // and from there the mouse keeps working normally (drag orbits, wheel zooms). The gui
    // combo below switches among "orbit" and the scene's cameras; the runtime's external-camera
    // override is not used by the demo (it stays available for exact/animated authored cameras).
    // usable cameras: those whose owning node exists in the imported tree, in scenes.cameras order
    struct authored_camera {
        deren::gltf::camera const* camera = nullptr;
        std::size_t source = 0; // owning loader node (asset node index)
    };
    deren::utility::vessel::vector<authored_camera> authored_cameras;
    for (deren::gltf::camera const& cam : scenes->cameras) {
        // find a node referencing this camera that is present in the imported tree (the loader's
        // asset-level node table + the controller's tree membership test)
        for (auto const& [source, loader_node] : scenes->node_by_source) {
            if (loader_node->camera_index && *loader_node->camera_index == static_cast<std::size_t>(&cam - scenes->cameras.data()) && animation.has_runtime_node(source)) {
                authored_cameras.push_back(authored_camera{&cam, source});
                break;
            }
        }
    }
    // current selection: 0 = orbit, 1..N = authored_cameras[i - 1]; default = the first
    // authored camera when the scene has any (same initial view as before, but now movable)
    int32_t current_camera = authored_cameras.empty() ? 0 : 1;
    // point the orbit camera at the authored pose: target = scene center (frame like the
    // default view), yaw/pitch/distance solved from the camera node's world transform
    auto const seed_orbit_from_camera = [&](int32_t const index) {
        if (index <= 0 || index > static_cast<int32_t>(authored_cameras.size())) {
            return; // "orbit": keep the current free orbit
        }
        authored_camera const& ac = authored_cameras[static_cast<std::size_t>(index - 1)];
        glm::mat4 camera_world = glm::mat4(1.0f);
        if (!deren::engine::scene_tree::find_world(runtime.get_scene(), imported.import_instance, ac.source, camera_world)) {
            return;
        }
        glm::vec3 const eye = glm::vec3(camera_world[3]);
        glm::vec3 const center = scene_center;
        glm::vec3 const dir = center - eye;
        float const dist = glm::length(dir);
        if (dist < 1e-4f) {
            return;
        }
        glm::vec3 const d = dir / dist;
        runtime.camera.target = center;
        runtime.camera.distance = dist;
        runtime.camera.yaw = std::atan2(d.x, d.z);
        runtime.camera.pitch = std::asin(std::clamp(d.y, -1.0f, 1.0f));
        std::string_view const cam_name = ac.camera->name.empty() ? std::string_view("<unnamed>") : std::string_view(ac.camera->name);
        deren::utility::log("camera: starting pose from glTF camera '{}' ({}) - you can still orbit/zoom", cam_name,
                            ac.camera->type == deren::gltf::camera_type::perspective ? "perspective" : "orthographic");
    };
    if (!authored_cameras.empty()) {
        seed_orbit_from_camera(current_camera);
    }

    // ---- authored (glTF) punctual lights: KHR_lights_punctual lights up automatically ----
    // They load straight into the editable gui light slots below (see the gui_bindings setup),
    // so imported lights are adjustable in the overlay like the demo ones.

    // Optional Dear ImGui debug overlay: deren::chores::setup_gui enables it on the runtime (when
    // use_gui) and assembles the whole panel - fps label, frustum-culling / shadow
    // toggles, the camera-target drag, animation playback controls, the camera selector and
    // the shadow-bias sliders. The widgets bind to the live gui_bindings below (checkbox and
    // slider mirrors + the animation mirrors, which the frame loop keeps in sync each frame);
    // authored-camera names and the orbit-seeding callback are passed in, so chores never
    // touches glTF types.
    deren::chores::gui_bindings gui;
    gui.shadow_enabled = settings.render.shadow; // checkbox initial states mirror the config
    gui.fxaa_enabled = settings.render.fxaa;
    gui.bloom_enabled = settings.render.bloom;         // bloom chain initial state (the frame mirrors it below)
    gui.gbuffer_debug = settings.render.gbuffer_debug; // gbuffer debug view initial state (M1)
    gui.gbuffer_channel = settings.render.gbuffer_channel;
    gui.render_mode = settings.render.unlit ? 1 : 0; // render-mode combo (0 = pbr, 1 = unlit)
    gui.taa_enabled = settings.render.taa;           // temporal anti-aliasing (M3)
    // ... and its TWO BLEND WEIGHTS, which the frame loop mirrors into the runtime every frame
    // (start_demo.set_taa below). Without these two lines the config's values never reached the
    // renderer: chores' gui_bindings defaults (0.9 / 0.5) are what set_taa received on every frame,
    // so editing `[render] taa_blend_static` in the file changed NOTHING - measured, the flicker at a
    // pinned close-up was byte-identical at 0.90, 0.95 and 0.98 (38.22% of pixels changing per frame
    // in all three). `taa_enabled` alone happened to look wired because it IS copied here.
    gui.taa_blend_static = settings.render.taa_blend_static;
    gui.taa_blend_min = settings.render.taa_blend_min;
    // THE TOON CHARACTER STAGE: copied here like every other toggle, and that copy is load-bearing rather than
    // tidiness - the frame loop mirrors `gui.character_forward` into the runtime every frame, so a config value
    // that never reached the gui (the trap the two lines above record) would be overwritten by the binding's
    // default on the very first frame.
    gui.character_forward = settings.render.character_forward;
    // ... AND THE GAME-SHADER MODEL, on the same load-bearing copy's terms: the frame loop mirrors the binding
    // into the runtime, so a config value that never reached the binding would be overwritten by its default
    // on the first frame (`runtime::set_game_toon`).
    gui.game_toon = settings.render.game_toon;
    gui.exposure = settings.render.exposure;
    gui.tone_mapping = settings.render.tone_mapping;
    gui.environment_rotation = settings.lighting.environment_rotation;
    gui.environment_strength = settings.lighting.environment_strength;
    gui.environment_background = settings.lighting.environment_background;
    gui.environment_color = {settings.lighting.environment_color[0], settings.lighting.environment_color[1], settings.lighting.environment_color[2]};
    gui.environment_blur = settings.lighting.environment_blur;
    gui.sun_intensity = settings.render.sun_intensity; // a scale on the sun (see [render] sun_intensity)
    // WHICH FILTER RESOLVES A SCALED RENDER CHAIN ([render] upscale), applied ONCE here and deliberately NOT
    // mirrored into `gui` like the toggles above: it selects a shader path rather than a per-frame quantity, so
    // there is nothing to change between frames - and the render scale it works with is a creation option, so
    // the pair is a startup decision by construction.
    start_demo.set_upscale_filter(settings.render.upscale);
    gui.shadow_cascades = settings.render.shadow_cascades - 1;       // cascade combo index (0 = single map)
    gui.shadow_cascade_blend = settings.render.shadow_cascade_blend; // cascaded shadow maps (M4)
    gui.clustered_lights = settings.render.clustered_lights;         // clustered light culling (M5)
    gui.ssao_enabled = settings.render.ssao;                         // screen-space AO (M6)
    gui.ssao_radius = settings.render.ssao_radius;
    gui.ssao_intensity = settings.render.ssao_intensity;
    gui.ssao_samples = static_cast<float>(settings.render.ssao_samples);
    gui.anim_playing = animation.is_playing(); // play checkbox initial state
    gui.current_camera = current_camera;       // combo selection (the pose seeded above)

    // ---- authored (glTF) punctual lights -> the editable gui light slots ----
    // KHR_lights_punctual lights load straight into the gui slots (up to
    // std::size(gui.point_lights)): main pushes the enabled set through
    // deren::chores::apply_point_lights() every frame, so imported lights are adjustable in the overlay
    // like the demo ones. Position comes from the owning node's loader-space world matrix,
    // shifted by the same import offset the geometry got. KHR directional lights are NOT mapped:
    // the engine sun is the shadow-casting analytic light configured by enable_shadows() above -
    // the log keeps that miss visible instead of silent. The lights are fixed to the base pose
    // (an animated light node would need per-frame resolution - not wired yet).
    std::size_t imported_lights = 0;
    std::size_t imported_directional = 0;
    std::size_t imported_truncated = 0;
    for (auto const& [source, loader_node] : scenes->node_by_source) {
        if (!loader_node->light_index.has_value() || !animation.has_runtime_node(source)) {
            continue; // no light, or the node is absent from the imported tree
        }
        deren::gltf::light const& src = scenes->lights[*loader_node->light_index];
        if (src.type == deren::gltf::light_type::directional) {
            ++imported_directional;
            continue;
        }
        if (imported_lights >= std::size(gui.point_lights)) {
            ++imported_truncated;
            continue;
        }
        deren::chores::gui_bindings::light_slot& slot = gui.point_lights[imported_lights++];
        glm::vec3 const position = glm::vec3(loader_node->transform_matrix[3]) + scene_import_shift;
        slot.enabled = true;
        slot.position[0] = position.x;
        slot.position[1] = position.y;
        slot.position[2] = position.z;
        slot.color[0] = src.color.x;
        slot.color[1] = src.color.y;
        slot.color[2] = src.color.z;
        slot.intensity = src.intensity;
        slot.range = src.range.value_or(0.0f); // 0 = infinite falloff (UBO semantics)
        if (src.type == deren::gltf::light_type::spot) {
            slot.spot = true;
            glm::vec3 const dir = glm::mat3(loader_node->transform_matrix) * glm::vec3(0.0f, 0.0f, -1.0f); // glTF spot axis
            glm::vec3 const axis = glm::dot(dir, dir) > 1e-8f ? glm::normalize(dir) : glm::vec3(0.0f, -1.0f, 0.0f);
            slot.direction[0] = axis.x;
            slot.direction[1] = axis.y;
            slot.direction[2] = axis.z;
            float const outer = src.spot_outer_cone.value_or(glm::radians(45.0f)); // KHR default cone
            slot.outer_cone_deg = glm::degrees(outer);
            // inner cone: the KHR angle when authored, else the legacy soft-inner derived in
            // cosine space (mix(outerCos, 1, 0.6)) converted back to degrees for the slider
            float const inner_cos = src.spot_inner_cone.has_value()
                                        ? std::cos(*src.spot_inner_cone)
                                        : 0.6f + 0.4f * std::cos(outer);
            slot.inner_cone_deg = glm::degrees(std::acos(std::clamp(inner_cos, -1.0f, 1.0f)));
        }
    }
    if (imported_lights > 0) {
        deren::utility::log("KHR_lights_punctual: {} point/spot light(s) loaded into the editable gui light slots (base pose; adjustable in the overlay)",
                            imported_lights);
    }
    if (imported_directional > 0) {
        deren::utility::log("KHR_lights_punctual: {} directional light(s) ignored - the engine sun is enable_shadows()'s analytic light",
                            imported_directional);
    }
    if (imported_truncated > 0) {
        deren::utility::log("KHR_lights_punctual: {} additional light(s) dropped (editable light-slot cap = {})", imported_truncated, std::size(gui.point_lights));
    }
    deren::utility::vessel::vector<deren::utility::vessel::string> gui_camera_names; // selector items: authored names (orbit added inside)
    gui_camera_names.reserve(authored_cameras.size());
    for (authored_camera const& ac : authored_cameras) {
        gui_camera_names.push_back(ac.camera->name.empty() ? "<unnamed>" : deren::utility::vessel::string(ac.camera->name));
    }
    // Presets are local files; builds and the render loop never fetch remote assets.
    deren::utility::vessel::vector<std::filesystem::path> environment_paths{{}};
    gui.environment_names.push_back("Procedural sky");
    if (!settings.lighting.environment_hdr.empty()) {
        std::filesystem::path path(settings.lighting.environment_hdr);
        if (path.is_relative())
            path = deren::utility::executable_directory() / path;
        environment_paths.push_back(path);
        gui.environment_names.push_back(deren::utility::vessel::string(path.filename().string()));
        gui.environment_index = 1;
    }
    std::error_code environment_scan_error;
    std::vector<std::filesystem::path> discovered;
    for (auto const& item : std::filesystem::directory_iterator(deren::utility::executable_directory() / "environments", environment_scan_error)) {
        auto extension = item.path().extension().string();
        std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (extension == ".hdr" && item.is_regular_file(environment_scan_error))
            discovered.push_back(item.path());
    }
    std::ranges::sort(discovered);
    for (auto const& path : discovered) {
        if (std::ranges::find(environment_paths, path) != environment_paths.end())
            continue;
        environment_paths.push_back(path);
        gui.environment_names.push_back(deren::utility::vessel::string(path.filename().string()));
    }
    int32_t applied_environment = gui.environment_index;
    int32_t requested_environment = applied_environment;
    std::future<prepared_environment> environment_job;
    gui.environment_status = "Environment ready";
    deren::chores::setup_gui(runtime, use_gui, settings, gui, animation, gui_camera_names, seed_orbit_from_camera);

    // Per-frame cheap clock: stamp() once per presented frame on this (the frame owner) thread,
    // so any other thread can read the current frame time as a plain atomic load. Animation /
    // future parallel workers should prefer frame_clock.last_ns()/delta_ns() over now().
    deren::utility::frame_clock frame_clock;

    // All per-frame decisions (event polling, ESC/close response, minimize skip, swapchain
    // recreation on restore/resize) live inside the runtime's frame phases, which main calls at
    // fine granularity so it can write per-frame data (scene node locals -> culling, skin
    // matrices, morph weights) between pacing and recording.
    int32_t last_render_mode = 0; // gui render-mode combo (0 = pbr); applied between frames below

    // ---- THE CLOUD'S FRAMING, WHEN THE MODEL PATH NAMED ONE --------------------------------------------
    // The asset itself was read from disk and handed over in 7b (that is also where its bounds and its totals come
    // from); what is left HERE is where the camera looks. THE CLOUD IS THE SCENE ON THIS PATH, so the camera takes
    // the cloud's own centre and a distance that holds two and a half of its radii - and because this runs BEFORE
    // the capture-camera override below, an explicit --capture-camera still wins over it.
    if (gaussian_only) {
        runtime.camera.target = (gaussian_min + gaussian_max) * 0.5f;
        runtime.camera.distance = glm::length(gaussian_max - gaussian_min) * 0.5f * 2.5f;
    }
    // scripted capture: apply the camera override LAST, so nothing in the setup above (the orbit
    // framing of the imported scene, an authored glTF camera) can win over the requested view
    if (capture.camera) {
        runtime.camera.yaw = glm::radians((*capture.camera)[0]);
        runtime.camera.pitch = glm::radians((*capture.camera)[1]);
        runtime.camera.distance = (*capture.camera)[2];
        if (capture.target) {
            runtime.camera.target = *capture.target;
        }
        deren::utility::log("capture camera: yaw {:.1f} deg, pitch {:.1f} deg, distance {:.2f}, target ({:.2f}, {:.2f}, {:.2f})",
                            (*capture.camera)[0], (*capture.camera)[1], (*capture.camera)[2],
                            runtime.camera.target.x, runtime.camera.target.y, runtime.camera.target.z);
    }
    // The sweep's BASE pose is whatever the camera ended up as - the scene's own `camera_fit`, an authored
    // glTF camera, or the pinned `--capture-camera` above - so a sweep composes with all three instead of
    // demanding a pinned pose it would have to be told twice.
    float const sweep_base_yaw = runtime.camera.yaw;
    if (settings.reflection_probe.enabled && !probe.enabled) {
        auto const& r = settings.reflection_probe;
        auto vec = [](auto const& a) { return glm::vec3(a[0], a[1], a[2]); };
        auto path = std::filesystem::path(r.asset.c_str());
        if (path.is_relative())
            path = deren::utility::executable_directory() / path;
        std::error_code ec;
        auto length = std::filesystem::file_size(path, ec);
        std::optional<deren::engine::probe_detail::asset> asset;
        if (!ec && length <= 32 + uint64_t(6) * 512 * 512 * 8 * 2) {
            std::vector<std::byte> bytes(length);
            std::ifstream file(path, std::ios::binary);
            if (file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
                asset = deren::engine::probe_detail::decode(bytes);
        }
        if (!asset || !runtime.set_local_probe(asset->half, asset->size, asset->mips, vec(r.position), vec(r.minimum), vec(r.maximum), r.fade, r.gain))
            deren::utility::log("local probe: invalid asset/config/upload; using global IBL ({})", path.string());
        else
            deren::utility::log("local probe loaded: {} ({}px, {} mips), position ({},{},{})", path.string(), asset->size, asset->mips, r.position[0], r.position[1], r.position[2]);
    }
    int32_t captured_frames = 0; // presented frames so far (scripted capture; see --capture-frames)
    // Capture-only transition probe: exercise progressive restoration while uploads are in flight.
    auto* disable_streaming_text = std::getenv("DEREN_BENCH_DISABLE_STREAMING_FRAME");
    int const disable_streaming_frame = disable_streaming_text ? std::atoi(disable_streaming_text) : -1;
    // Opt-in benchmark trace: retain timestamps in memory and write only after rendering ends.
    char const* const frame_trace_path = std::getenv("DEREN_BENCH_FRAME_TRACE");
    deren::utility::vessel::vector<std::pair<double, double>> frame_trace;
    auto trace_previous = std::chrono::steady_clock::now();
    auto const trace_origin = trace_previous;
    double const trace_unix_ms = std::chrono::duration<double, std::milli>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (frame_trace_path)
        frame_trace.reserve(4096);
    // Resource queries walk allocator metadata: enable only in separate memory captures.
    char const* const resource_trace_path = std::getenv("DEREN_BENCH_RESOURCE_TRACE");
    struct memory_sample {
        std::uint64_t frame;
        deren::promise::rhi::resource_memory_statistics gpu;
        deren::utility::memory::allocation_statistics cpu;
    };
    deren::utility::vessel::vector<memory_sample> resource_trace;
    std::uint64_t resource_frame = 0;
    auto sample_resources = [&] {
        auto gpu = runtime.rhi_face().resources->memory_statistics();
        auto cpu = deren::utility::vessel::memory_statistics();
        resource_trace.push_back({resource_frame, gpu, cpu});
    };
    if (resource_trace_path) {
        resource_trace.reserve(128);
        sample_resources();
    }
    auto previous_environment_state = std::tuple(gui.environment_rotation, gui.environment_strength, gui.environment_background, gui.environment_color.x, gui.environment_color.y, gui.environment_color.z, gui.environment_blur);
    unsigned probe_face = 0, probe_frames = 0;
    bool probe_done = false;
    std::vector<float> probe_pixels;
    auto const probe_started = std::chrono::steady_clock::now();
    if (probe.enabled) {
        probe_pixels.resize(std::size_t(6) * probe.size * probe.size * 4);
        start_demo.set_radiance_capture(true);
    }
    while (true) {
        if (probe.enabled) {
            if (std::chrono::steady_clock::now() - probe_started > std::chrono::minutes(5)) {
                deren::utility::error("radiance capture timed out");
                return 3;
            }
            runtime.set_camera_override(deren::engine::make_cube_capture_camera(probe.position, probe_face, probe.near_plane, probe.far_plane));
        }
        // Deterministic capture-only exercise of asynchronous preset publication and error retention.
        if (capture.frames > 0 && std::getenv("DEREN_ENVIRONMENT_TEST_CYCLE") != nullptr &&
            captured_frames > 0 && captured_frames % 60 == 0 && !environment_job.valid() && environment_paths.size() > 1) {
            gui.environment_index = int32_t((captured_frames / 60) % environment_paths.size());
        }
        if (capture.frames > 0 && captured_frames == disable_streaming_frame) {
            runtime.set_visibility_texture_streaming(false);
        }
        // Phase 1: poll window events (ESC / native close -> closed, minimized -> skipped)
        deren::engine::frame_status const polled = runtime.poll_events();
        if (polled == deren::engine::frame_status::closed || deren::engine::is_failure(polled)) {
            break;
        }
        if (polled == deren::engine::frame_status::skipped) {
            // Minimized: skip this frame's CPU work too; refresh the fps baseline so the pause
            // is not counted as one huge rendered frame.
            frame_stats.on_skipped();
            std::this_thread::yield();
            continue;
        }
        runtime.recreate_if_minimized();
        // Upload only between frames. set_ibl waits for prior submissions and rolls back failed writes.
        if (environment_job.valid() && environment_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto prepared = environment_job.get();
            if (requested_environment == gui.environment_index) {
                bool published = prepared.error.empty() && runtime.set_ibl(deren::engine::ibl_input{
                                                               .prefiltered_env = prepared.environment, .irradiance = prepared.irradiance, .brdf_lut = lut_bytes, .env_size = uint32_t(env_size), .env_mip_count = uint32_t(env_mip_count), .irr_size = uint32_t(irr_size), .lut_size = uint32_t(lut_size)});
                if (published) {
                    start_demo.reset_temporal_history();
                    applied_environment = requested_environment;
                    gui.environment_status = "Environment ready: " + gui.environment_names[applied_environment];
                    deren::utility::log("environment switched: {}", gui.environment_names[applied_environment]);
                } else {
                    gui.environment_index = applied_environment;
                    gui.environment_status = prepared.error.empty() ? "Environment upload failed; previous environment retained" : prepared.error;
                    deren::utility::log("environment switch refused: {}", gui.environment_status);
                }
            } else
                gui.environment_status = "Environment ready: " + gui.environment_names[applied_environment];
        }
        if (!environment_job.valid() && gui.environment_index != applied_environment) {
            if (gui.environment_index < 0 || std::size_t(gui.environment_index) >= environment_paths.size())
                gui.environment_index = applied_environment;
            else {
                requested_environment = gui.environment_index;
                auto path = environment_paths[requested_environment];
                float scale = settings.lighting.environment_intensity;
                gui.environment_status = "Preparing environment: " + gui.environment_names[requested_environment];
                environment_job = std::async(std::launch::async, [path, sun_direction, env_size, env_mip_count, irr_size, scale] {
                    return prepare_environment(path, sun_direction, env_size, env_mip_count, irr_size, scale);
                });
            }
        }

        // Scripted capture: the camera sweep, advanced by the number of PRESENTED frames - a frame index,
        // not a clock reading, so two runs of one sweep are byte-identical (the harness's determinism run
        // is what verifies it). It has to happen before pace_and_acquire(), which is the phase that writes
        // the camera UBO.
        if (capture.sweep_yaw_deg_per_frame != 0.0f) {
            runtime.camera.yaw = sweep_base_yaw + glm::radians(capture.sweep_yaw_deg_per_frame) * static_cast<float>(captured_frames);
        }

        runtime.set_environment(gui.environment_rotation, gui.environment_strength, gui.environment_background, gui.environment_color, gui.environment_blur);
        // The captured GameToon viewer's toon path ends in its own tone map (Q332 disabled); tone mode 2 is
        // that transcribed curve. Keep the selected mode for every other chain.
        // The captured character.frag is written for the viewer's own tone map (mode 2); with the staged skin
        // payload accessors fixed, mode 2 no longer shifts skin green, so profile-on uses the captured display
        // path and the selected tone map is left for profile-off.
        runtime.set_tone_mapping(gui.game_toon ? 2 : gui.tone_mapping);
        runtime.set_exposure(gui.exposure);
        auto current_environment_state = std::tuple(gui.environment_rotation, gui.environment_strength, gui.environment_background, gui.environment_color.x, gui.environment_color.y, gui.environment_color.z, gui.environment_blur);
        if (current_environment_state != previous_environment_state) {
            start_demo.reset_temporal_history();
            previous_environment_state = current_environment_state;
        }
        // Phase 2: pace + acquire the next frame slot. After pace_and_acquire() returns
        // proceed, this slot's previous submission has completed, so the per-frame host writes
        // below (scene node locals -> culling, skin matrices, morph weights) cannot race an
        // in-flight frame.
        deren::engine::frame_status const paced = runtime.pace_and_acquire();
        if (paced == deren::engine::frame_status::closed || deren::engine::is_failure(paced)) {
            break;
        }
        if (paced == deren::engine::frame_status::skipped) {
            // The swapchain is not usable this iteration - zero-sized (not sized yet / restored
            // minimized) so there are no attachments to render into, or it was recreated during the
            // acquire. Either way: skip this frame's CPU work too, like the minimized case above.
            // (There used to be a SECOND identical check here with a "recreated during acquire"
            // comment: unreachable, since the first one already covered it - both return the same
            // status. The reasons differ, the handling does not.)
            frame_stats.on_skipped();
            std::this_thread::yield();
            continue;
        }

        // drive the animation controller: sample the active animation into node locals (T/R/S +
        // morph weights) and rebuild the skin matrices, into the frame slot pace_and_acquire()
        // just paced. dt is the WALL CLOCK - clamped, so a pause (minimized / swapchain-recreate
        // gaps that never stamped) does not fast-forward the animation by the whole gap, and
        // playback resumes where it paused - UNLESS --capture-animation-sweep asked for a
        // frame-indexed clock instead, which is the only form a capture of a deforming mesh can
        // be gated on (see capture_options::animation_seconds_per_frame).
        float const dt = capture.animation_seconds_per_frame != 0.0f
                             ? capture.animation_seconds_per_frame
                             : static_cast<float>(std::min(frame_clock.delta_seconds(), 0.25));
        animation.update(dt);
        // ---- THE HEAD FRAME FOLLOWS THE BONE, published here because this is where the pose exists ----
        //
        // IT LANDS ONE FRAME LATE, and that is a consequence of the per-frame-slot arrangement rather than an
        // oversight: the runtime copies `head_state` into the paced slot INSIDE pace_and_acquire(), which has
        // already run by the time this frame's pose is produced. So a pose is shaded with the head frame
        // computed from the previous pose. For a pose that is standing still - which is what a capture gates on,
        // and what this model's rest pose is - the difference is exactly zero, and for a turning head it is one
        // frame of lag: the same trade every other per-frame write in this loop makes.
        //
        // THE EXTRACTION IS THE REFERENCE'S (`EfFaceGetHeadBasis`): `-row3` of the bone's world matrix is the
        // head's forward and `-row1` its right, where "row" is HLSL's and therefore a COLUMN of the
        // column-major matrix this engine stores. `head_basis_from_axes` negates, re-orthogonalises and falls
        // back when the bone is degenerate, so a zero matrix here cannot put NaNs in the sigmoid.
        // ---- THE CAPTURED FACE FRAME (`Q324`/`Q80`) OWNS THE HEAD BASIS WHEN THE LOADER MEASURED ONE ----
        //
        // The captured viewer re-derives `Q324`/`Q80` every frame by `transformDirection(head.matrixWorld)` on
        // the bind-pose axes (its `update()` loop; see the loader's `face_frame_of` for the load-side measurement).
        // THIS REPOSITORY'S CHARACTERS ARE BAKED (zero skins), so there is no head bone to turn and the load-pose
        // axes ARE the frame - the publish below is then the same vector every frame. The `head_relative` branch
        // is the captured transform for a skinned asset, kept so the wiring is the same shape as the viewer's.
        // `front.w = 1.0` marks the block as the captured frame (see `head_ubo::front`); `skin_face.slang` reads
        // that flag and stops deriving `Q80` as `-head_right`.
        if (captured_face_frame.has_value()) {
            glm::vec3 world_forward = captured_face_frame->world_forward;
            glm::vec3 world_right = captured_face_frame->world_right;
            glm::vec3 center(0.0f);
            bool has_center = false;
            if (captured_face_frame->head_relative) {
                if (std::optional<glm::mat4> const bone = animation.joint_world(captured_face_frame->skin_index, captured_face_frame->head_joint); bone.has_value()) {
                    world_forward = glm::normalize(glm::mat3(*bone) * captured_face_frame->forward);
                    world_right = glm::normalize(glm::mat3(*bone) * captured_face_frame->right);
                    center = glm::vec3((*bone)[3]);
                    has_center = true;
                }
            }
            runtime.set_head_basis(deren::engine::head_ubo{
                .front = glm::vec4(world_forward, 1.0f),
                .right = glm::vec4(world_right, 0.0f),
                .up = glm::vec4(glm::normalize(glm::cross(world_right, world_forward)), 0.0f),
                .center = glm::vec4(center, has_center ? 1.0f : 0.0f)});
        } else if (head_joint.has_value()) {
            if (std::optional<glm::mat4> const bone = animation.joint_world(head_rig, *head_joint); bone.has_value()) {
                glm::mat4 const& joint = *bone;
                glm::vec3 const forward_row(joint[0][2], joint[1][2], joint[2][2]); // HLSL `_31_32_33`
                glm::vec3 const right_row(joint[0][0], joint[1][0], joint[2][0]);   // HLSL `_11_12_13`
                deren::gltf::head_basis const basis = deren::gltf::head_basis_from_axes(forward_row, right_row);
                // ---- AND THE HEAD'S OWN POSITION, FROM THE SAME MATRIX'S TRANSLATION COLUMN ----
                //
                // `Recalculate normal` needs `normalize(posWS - headCenter)`, and the reference gets `headCenter`
                // from an OBJECT's `Object Info.Location` (spec §5.1: `存储已命名属性.003 <- 物体信息(HC).Location`).
                // The equivalent here is the head BONE's world matrix translation - `joint[3]`, the same matrix the
                // two rows above come from - which `deren::gltf::head_basis` deliberately does not carry because every
                // consumer before this step wanted a direction. It is published UNCONDITIONALLY OF `basis.from_skeleton`
                // on purpose: the axes falling back to glTF's constants says nothing about where the head IS, and a
                // centre of `(0,0,0)` would make the sphere normal `normalize(posWS)`, i.e. a normal pointing away
                // from the world origin - a face shaded as if its head were at the origin of the scene.
                // `center.w = 1.0` IS THE FLAG THAT SAYS THE SPHERE NORMAL HAS A CENTRE (see `head_ubo::center`):
                // the initial publish below leaves it `0.0`, and on this repository's assets - every one of which
                // is a BAKED pose with `skins = 0` - this branch is never taken, so the face arm keeps the socket's
                // own default instead of building a normal about the world origin.
                glm::vec3 const head_center(joint[3][0], joint[3][1], joint[3][2]);
                runtime.set_head_basis(deren::engine::head_ubo{.front = glm::vec4(basis.front, 0.0f), .right = glm::vec4(basis.right, 0.0f), .up = glm::vec4(basis.up, 0.0f), .center = glm::vec4(head_center, 1.0f)});
                static bool logged_head_probe = false;
                if (!logged_head_probe) {
                    logged_head_probe = true;
                    deren::utility::log("head probe: bone forward row ({:.3f} {:.3f} {:.3f}) right row ({:.3f} {:.3f} {:.3f}) -> basis front ({:.3f} {:.3f} {:.3f}) from_skeleton {} center ({:.3f} {:.3f} {:.3f})",
                                        forward_row.x, forward_row.y, forward_row.z, right_row.x, right_row.y, right_row.z,
                                        basis.front.x, basis.front.y, basis.front.z, basis.from_skeleton,
                                        head_center.x, head_center.y, head_center.z);
                }
            } else {
                static bool logged_head_miss = false;
                if (!logged_head_miss) {
                    logged_head_miss = true;
                    deren::utility::log("head probe: joint_world({}, {}) returned NOTHING", head_rig, *head_joint);
                }
            }
        }
        gui.anim_time = animation.current_time();  // keep the gui time slider in sync
        gui.anim_playing = animation.is_playing(); // reflect controller-side pauses (scrub / select)
        gui.anim_index = static_cast<int32_t>(animation.current());

        if (probe.enabled && probe_frames == 8 && !runtime.request_radiance_capture()) {
            deren::utility::error("radiance request refused");
            return 3;
        }
        // Phase 3: record + submit + present the paced frame
        deren::engine::frame_status const rec = runtime.begin_recording();
        if (rec == deren::engine::frame_status::closed || deren::engine::is_failure(rec)) {
            break;
        }
        runtime.record_main_drawcalls();
        deren::engine::frame_status const ended = runtime.end_recording();
        if (ended == deren::engine::frame_status::closed || deren::engine::is_failure(ended)) {
            break;
        }
        deren::engine::frame_status const result = runtime.submit_and_present();
        if (result == deren::engine::frame_status::closed || deren::engine::is_failure(result)) {
            break;
        }
        if (result == deren::engine::frame_status::skipped && !probe.enabled) {
            // present reported the swapchain out of date / recreated it: retry next iteration
            frame_stats.on_skipped();
            std::this_thread::yield();
            continue;
        }

        if (probe.enabled && ++probe_frames == 9) {
            auto image = runtime.acquire_radiance_image();
            auto const face_values = std::size_t(probe.size) * probe.size * 4;
            if (!image || image->width != static_cast<uint32_t>(probe.size) || image->height != static_cast<uint32_t>(probe.size) ||
                !deren::engine::radiance_detail::canonicalize_face(image->rgba, probe.size, std::span<float>(probe_pixels).subspan(probe_face * face_values, face_values))) {
                deren::utility::error("radiance readback failed");
                return 3;
            }
            probe_frames = 0;
            if (++probe_face == 6) {
                auto written = deren::app::radiance_capture::write_cube(probe.output, probe.size, probe_pixels);
                if (!written) {
                    deren::utility::error(deren::app::radiance_capture::describe(written.error()));
                    return 3;
                }
                probe_done = true;
                break;
            }
            start_demo.reset_temporal_history();
        }
        // A frame was presented: publish its stamp for cheap readers (frame_clock)
        ++resource_frame;
        if (resource_trace_path && (resource_frame % 10 == 0 || resource_frame == static_cast<std::uint64_t>(capture.frames)))
            sample_resources();
        if (frame_trace_path) {
            auto const boundary = std::chrono::steady_clock::now();
            frame_trace.emplace_back(std::chrono::duration<double, std::milli>(boundary - trace_origin).count(),
                                     std::chrono::duration<double, std::milli>(boundary - trace_previous).count());
            trace_previous = boundary;
        }
        splash.close();
        frame_clock.stamp();

        // scripted capture: once the requested number of frames has been PRESENTED, request the
        // screenshot from the runtime - the F12 block below consumes the request in this same
        // iteration, so the capture happens on a fully warmed-up frame
        if (capture.frames > 0 && ++captured_frames >= capture.frames) {
            runtime.request_screenshot();
        }

        // gui "render mode": switch the runtime's default pipeline BETWEEN frames (the pipeline
        // registry must not be mutated while a frame records; this point is after submit, before
        // the next frame's recording). Default-semantics leaves re-shade on the next frame.
        if (gui.render_mode != last_render_mode) {
            last_render_mode = gui.render_mode;
            std::string_view const mode_name = gui.render_mode == 0 ? "pbr" : "unlit";
            runtime.set_default_pipeline(mode_name);
            // the lighting stage cannot switch pipelines per fragment, so tell it that the default
            // pipeline is the flat one - it then writes the stored albedo instead of shading, so
            // "unlit" means the same thing for the opaque scene and for the transparent pass
            start_demo.set_unlit(gui.render_mode == 1);
            deren::utility::log("render mode: {} ({})", mode_name, gui.render_mode == 0 ? "lit" : "unlit / flat");
        }

        // fps statistics: accumulate the frame gap into the rolling window
        frame_stats.tick();
        // punctual lights: push the gui slot set every frame, INDEPENDENT of the overlay being
        // visible - imported model lights were loaded into those slots, so they must stay lit in
        // headless-overlay runs too (the demo slots stay off unless the user enabled them)
        deren::chores::apply_point_lights(runtime, gui, demo_lights);
        if (settings.render.game_toon) {
            // captured hair rim depth: width 0.368, gain 10, modest strength; the contour stage owns
            // the only legal gbuffer_depth sample after the character pass.
            runtime.set_toon_screen_rim(0.368f, 10.0f, 0.25f, 1.0f, 0.85f, 0.95f);
        }
        runtime.set_bloom(gui.bloom_enabled ? gui.bloom_intensity : 0.0f, gui.bloom_threshold); // bloom checkbox + knobs -> post pass
        runtime.set_async_present(config.settings.render.async_present);
        runtime.set_max_fps(config.settings.render.max_fps); // 0 = uncapped (see config.example.toml)
        // FXAA: mirrored every frame like the other post-process values (the runtime clamps them and
        // ignores the flag when no fxaa pipeline was created)
        runtime.set_fxaa(gui.fxaa_enabled, gui.fxaa_subpixel, gui.fxaa_edge_threshold);
        // G-buffer debug view (the G-buffer's stored data): mirrored every frame like the FXAA state,
        // so the config, the overlay checkbox and the channel combo all take effect immediately
        runtime.set_gbuffer_debug(gui.gbuffer_debug);
        start_demo.set_gbuffer_channel(gui.gbuffer_channel);
        // TAA (the engine's anti-aliasing): mirrored like the other render toggles. The jitter
        // follows automatically - it is applied to the projection when TAA is active.
        start_demo.set_taa(gui.taa_enabled, gui.taa_blend_static, gui.taa_blend_min);

        // Order matters for the M5/M6 mirrors: their availability checks read the state the lines
        // above just set (the debug view replaces the lighting stage, clustered lighting only exists
        // when the cluster pipeline does), so mirroring them earlier would report a stale answer for
        // the first frame of every run.
        runtime.set_clustered_lights(gui.clustered_lights);
        // The toon character stage: mirrored like the other render toggles. Its own gate composes this knob
        // with the frame's opaque leaf list (feature_facts::character_forward_pending), so turning it on in a
        // frame with no opaque geometry records nothing rather than an empty instance.
        runtime.set_character_forward(gui.character_forward);
        // ... and WHICH CHAIN that stage shades with, mirrored on the line above's own terms - CPU-side, read while
        // ... and the THIRD model, mirrored on the same terms. The frame's three-way choice gives `game_toon`
        // precedence when both model switches are on, so this pair is never silently combined into a fourth state.
        runtime.set_game_toon(gui.game_toon);
        start_demo.set_ssao(gui.ssao_enabled, gui.ssao_radius, gui.ssao_intensity, static_cast<uint32_t>(std::max(gui.ssao_samples, 0.0f) + 0.5f));
        // cel shading: the combo picks a discrete band count (index 0 = off); every entry is a
        // visibly different look, unlike a continuous strength that had dead zones between bands
        constexpr deren::utility::vessel::array<float, 7> toon_band_counts = {0.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f};
        auto const toon_index = static_cast<std::size_t>(std::clamp(gui.toon_bands_index, 0, static_cast<int32_t>(toon_band_counts.size()) - 1));
        runtime.set_toon_shading(toon_band_counts[toon_index], gui.toon_softness);
        // The area light's radiance MULTIPLIES the sun, but ONLY while the emitter contributes energy
        // (`area_light_takes_over` = `area_light_size > 0 && area_light_irradiance`), so `area_light_scale` is
        // 1.0f for the default config and for the penumbra-only mode - i.e. exactly the line it always was.
        // IT MUST STAY ON THIS LINE, IN THIS BLOCK: the mirror is replayed EVERY FRAME, so scaling once at
        // startup would be overwritten by `gui.sun_intensity` from frame 2 onwards - and a capture at frame 40
        // would not show it, because every frame but the first would be back to 1.0x. `set_sun_intensity`
        // clamps to 0..3, so a radiance above 3.0 is a value the sun cannot express (see the
        // `area_light_intensity` note).
        runtime.set_sun_intensity(gui.sun_intensity * area_light_scale);
        // The emitter's geometry goes to the two appended light-UBO lanes. Idempotent, and deliberately
        // AFTER the shadow setup above: `enable_shadows` rebuilds the whole light UBO from the sun alone.
        runtime.set_area_light(glm::vec3(area_light.world_centre[0], area_light.world_centre[1], area_light.world_centre[2]),
                               area_light.half,
                               settings.lighting.area_light_irradiance,
                               glm::vec3(area_light.axis[0], area_light.axis[1], area_light.axis[2]),
                               area_light.penumbra);
        // The sun's DIRECTION is mirrored the same way, from the config (`[lighting] sun_direction`). It is
        // safe to set every frame: the runtime compares against the direction it already has and only
        // invalidates the shadow cascade fit when it actually moved.
        //
        // WITH AN AREA LIGHT CONTRIBUTING ENERGY (`area_light_takes_over`), THE EMITTER DECIDES THAT DIRECTION,
        // because "the soft box is the frame's main light" is the whole point: the shading, the cascades and
        // the sky's disc all follow it. With `irradiance = false` (penumbra only) the direction stays
        // `gui.sun_direction`, and with the default `size = 0` this is the line it always was. A consequence
        // worth knowing: this scene also carries a studio HDRI and a background dome, so the analytic sky's own
        // sun ends up behind the dome and is invisible - documented rather than worked around, and why the
        // environment stays the only other light in the frame.
        runtime.set_sun_direction(area_light_takes_over
                                      ? glm::vec3(area_light.to_light_dir[0], area_light.to_light_dir[1], area_light.to_light_dir[2])
                                      : glm::vec3(sun_direction[0], sun_direction[1], sun_direction[2]));
        // F12 screenshot: the runtime reports the request (edge-triggered in poll_events), main
        // captures the presented swapchain image and writes it as a PNG (dependency-free encoder)
        if (runtime.consume_screenshot_request()) {
            auto const image = runtime.acquire_current_frame_image();
            if (!image) {
                deren::utility::log("screenshot failed: {}", image.error());
            } else {
                // base directory from [paths] screenshot_dir (empty = the current working
                // directory); created on demand so a fresh checkout can capture immediately
                std::filesystem::path directory(settings.paths.screenshot_dir);
                if (!directory.empty()) {
                    std::error_code ec;
                    std::filesystem::create_directories(directory, ec);
                    if (ec) {
                        deren::utility::log("screenshot: cannot create '{}' - saving to the working directory", directory.string());
                        directory.clear();
                    }
                }
                std::filesystem::path const path = directory / deren::utility::vessel::format("screenshot_{:%Y%m%d_%H%M%S}.png", std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
                auto const written = deren::utility::write_png(path, image->width, image->height, image->rgba);
                if (written) {
                    deren::utility::log("screenshot saved: {} ({}x{})", path.string(), image->width, image->height);
                } else {
                    deren::utility::log("screenshot save failed: {}", written.error());
                }
                // ... AND THE POSE THAT FRAME WAS RENDERED WITH, in the form both the config and the
                // command line take. A screenshot without its camera is a picture nobody can reproduce,
                // which is how a user's "the face goes grey at some angle" cost a day of guessing.
                log_camera_pose("F12 screenshot");
            }
            // scripted capture: the frame is captured (saved or not - do not spin forever on a
            // failing writer), so leave the render loop and shut down cleanly
            if (capture.frames > 0) {
                break;
            }
        }
        // overlay fps mirror: updated unconditionally - the overlay can be hidden with F1 and
        // shown again at runtime, so its data must stay fresh even while it is not drawn
        gui.fps = frame_stats.smoothed_fps();
        if (frame_stats.window_rolled()) {
            // once per second: the fps log line stays for headless / non-gui runs; the overlay
            // shows the same number via smoothed_fps()
            deren::utility::log("fps: {:.1f} ({:.2f} ms/frame)", frame_stats.window_fps(), frame_stats.window_frame_ms());
            if (animation.has_active()) {
                // report the playback clock + the first animated node's evaluated translation
                // (proves the keyframes are actually moving the tree)
                std::string_view const node_name = animation.get_debug_node_name().empty()
                                                       ? std::string_view("<no target in scene>")
                                                       : animation.get_debug_node_name();
                deren::utility::log("  anim '{}': t={:.3f}s/{:.2f}s, '{}' at ({:.3f}, {:.3f}, {:.3f})",
                                    animation.active_name(), animation.current_time(), animation.loop_duration(), node_name,
                                    animation.get_debug_translation().x, animation.get_debug_translation().y, animation.get_debug_translation().z);
            }
            if (animation.is_skin_debug_valid()) {
                deren::utility::log("  skin '{}': last joint world x-axis ({:.3f}, {:.3f}, {:.3f})", animation.get_skin_debug_name(),
                                    animation.get_skin_debug_translation().x, animation.get_skin_debug_translation().y, animation.get_skin_debug_translation().z);
            }
        }
    }

    // The pose the session ENDED on, so a view reached by orbiting can be written down without
    // re-deriving it from the fit. Printed whether the loop ended by closing the window or by a
    // scripted capture finishing.
    log_camera_pose("exit");
    // 15. Wait for the GPU to finish; primitives and pipelines are released by the runtime destructor
    runtime->wait_idle();
    if (resource_trace_path) {
        std::ofstream trace(resource_trace_path, std::ios::binary | std::ios::out);
        trace << "frame,supported,gpu_buffer_local,gpu_image_local,gpu_total_local,gpu_peak_local,gpu_total,gpu_reserved,gpu_reserved_local,cpu_current,cpu_peak\n";
        for (auto const& sample : resource_trace)
            trace << deren::utility::vessel::format("{},{},{},{},{},{},{},{},{},{},{}\n", sample.frame, sample.gpu.supported ? 1 : 0,
                                                    sample.gpu.buffers.current_device_local_bytes, sample.gpu.images.current_device_local_bytes,
                                                    sample.gpu.total.current_device_local_bytes, sample.gpu.total.peak_device_local_bytes, sample.gpu.total.current_bytes,
                                                    sample.gpu.allocator_reserved_bytes, sample.gpu.allocator_device_local_reserved_bytes,
                                                    sample.cpu.current_bytes, sample.cpu.peak_bytes);
        if (!trace)
            deren::utility::error("benchmark resource trace could not be written");
    }
    if (frame_trace_path) {
        std::ofstream trace(frame_trace_path, std::ios::binary | std::ios::out);
        trace << "frame,unix_ms,interval_ms\n";
        for (std::size_t i = 0; i < frame_trace.size(); ++i)
            trace << deren::utility::vessel::format("{},{:.6f},{:.6f}\n", i + 1, trace_unix_ms + frame_trace[i].first, frame_trace[i].second);
        if (!trace)
            deren::utility::error("benchmark frame trace could not be written");
    }
    deren::utility::log("render loop finished");
    return probe.enabled && !probe_done ? 3 : 0;
}