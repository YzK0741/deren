// -*- C++ -*-
// ============================================================================
// file: promise/gui/gui_entry.hpp
//
// THE GUI PLUGIN'S C ABI SURFACE: the one `extern "C"` symbol a `deren_gui_<api>.dll` exports and the host
// resolves, plus the abstract interface it hands back. Same shape as `promise/rhi/backend_entry.hpp` and for
// the same reasons:
//
//   * ONE ENTRY POINT, resolved by NAME (`LoadLibrary` + `GetProcAddress`), so the executable does not link
//     the GUI at all - no import library, nothing bound before `main`, and a missing DLL is a NAMED loader
//     panic instead of a load-time failure with no diagnostic.
//   * THE OBJECT CROSSES AS AN ABSTRACT INTERFACE (a virtual base compiled by both sides independently), not
//     as C++ classes with an ABI: the host derives from nothing, it CALLS.
//   * THE OWNERSHIP CROSSES AS A `shared_ptr`, whose control block carries the plugin's own deleter - so the
//     overlay is destroyed inside the image that made it, however the last reference goes.
//
// WHY ONE DLL PER API (the user's ruling): the GUI's Vulkan backend is what drags `vulkan-1.dll` into the
// program (every one of those imports comes from `imgui_impl_vulkan.cpp`). Giving each API its own GUI DLL
// confines that to the plugin that serves it: `deren_gui_vulkan.dll` privately links `vulkan-1` and `glfw`,
// and a `deren_gui_d3d12.dll` would link neither. `api_type` in `create_info` is then the DLL's OWN SANITY
// CHECK - a Vulkan GUI plugin asked for `d3d12` refuses BY NAME rather than trying and failing later.
//
// THE HANDLES ARE `void*` ON PURPOSE: `create_info` carries what the plugin needs and the CONTRACT does not
// have yet (an instance, a device, a queue, two formats), and typing them as Vulkan handles here would put
// `VkDevice` in a header the engine half includes - the boundary this whole effort exists to hold. The
// Vulkan plugin casts them back, and it is the only side that knows what they are.
//
// Include this AFTER any modules the host needs: the declaration below names only this header's own types,
// which is why it can be a plain header sitting outside every module's file set (the backend entry's rule).
// ============================================================================
#pragma once

#include "../../utility/abi_export.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

namespace deren::gui {
    /// The boundary's own version: an INCOMPATIBLE change to the interface or to `create_info` moves it, and
    /// the entry refuses a caller that hands over another number (the backend's abi handshake, one level up).
    inline constexpr std::uint32_t gui_abi_version = 1u;

    /// WHICH GRAPHICS API THE HOST IS ASKING THIS GUI DLL TO SERVE. It is the host's hint AND the plugin's
    /// check: `deren_gui_vulkan.dll` serves `vulkan` and refuses anything else by name.
    enum class api_type : std::uint32_t {
        none = 0,
        vulkan = 1,
        d3d12 = 2,
    };

    /// The file-name suffix a plugin for @p api carries: `deren_gui_vulkan.dll` / `deren_gui_d3d12.dll`.
    [[nodiscard]] constexpr char const* api_suffix(api_type const api) noexcept {
        switch (api) {
        case api_type::vulkan:
            return "vulkan";
        case api_type::d3d12:
            return "d3d12";
        case api_type::none:
            break;
        }
        return "none";
    }

    /// The DLL a host looks for, without the extension or the directory.
    inline constexpr char const* gui_dll_prefix = "deren_gui_";

    /// Everything a GUI plugin needs to come up: the window it attaches to, the device it will draw with,
    /// and the two formats its pipeline has to match.
    ///
    /// `struct_size` is the append-only guard the contract's descriptors use: a plugin reads only the prefix
    /// it was handed, so an older host can talk to a newer plugin and the reverse.
    struct create_info {
        std::uint32_t struct_size = sizeof(create_info);
        api_type api = api_type::none; ///< which API this plugin is being asked to serve
        GLFWwindow* window = nullptr;  ///< the window the overlay attaches to (its callbacks chain)
        void* instance = nullptr;      ///< the graphics API's instance (VkInstance for the Vulkan plugin)
        void* physical_device = nullptr;
        void* device = nullptr;
        std::uint32_t graphics_queue_family = 0;
        void* graphics_queue = nullptr;
        std::uint32_t color_format = 0; ///< the format the overlay's pipeline renders into (0 = undefined)
        std::uint32_t depth_format = 0; ///< ... and the depth attachment's (0 = none)
        std::uint32_t frames_in_flight = 2;
    };

    /// ONE WIDGET, as the HOST can talk about it: it exists so a caller can ATTACH A VISIBILITY PREDICATE to
    /// what it just added (`add_checkbox(...).set_visible_when([&]{ return ...; })`), which is the one thing
    /// the app's panel building does with a widget after creating it. Everything else about a widget is the
    /// plugin's business.
    struct widget {
        virtual ~widget() = default;
        /// draw this widget only while @p predicate is true (the panel's own visibility is separate)
        virtual void set_visible_when(std::function<bool()> predicate) = 0;
    };

    /// ONE DEBUG PANEL: a titled window whose contents the HOST declares and the PLUGIN draws.
    ///
    /// The widgets are verbs rather than objects because the widget classes live inside the plugin: the host
    /// hands over a callback and a pointer to its own state, and never sees what draws it. Each verb returns
    /// the `widget` handle so a predicate can be attached to it.
    struct panel {
        virtual ~panel() = default;

        virtual widget& add_label(std::string text) = 0;
        virtual widget& add_label(std::function<std::string()> text) = 0;
        virtual widget& add_checkbox(std::string label, bool* value, std::function<void(bool)> on_change = {}) = 0;
        virtual widget& add_slider(std::string label, float* value, float min, float max, std::function<void(float)> on_change = {}) = 0;
        /// a three-component drag control bound to three consecutive floats (a glm::vec3's x member)
        virtual widget& add_vec3(std::string label, float* value, float speed = 0.05f, std::function<void()> on_change = {}) = 0;
        virtual widget& add_combo(std::string label, std::vector<std::string> items, std::int32_t* current_item, std::function<void(std::int32_t)> on_change = {}) = 0;

        virtual void clear() = 0;
        [[nodiscard]] virtual bool empty() const noexcept = 0;
        [[nodiscard]] virtual std::size_t size() const noexcept = 0;

        [[nodiscard]] virtual std::string const& title() const noexcept = 0;
        virtual void set_open(bool open) noexcept = 0;
        [[nodiscard]] virtual bool open() const noexcept = 0;
        virtual void set_default_size(float width, float height) noexcept = 0;
    };

    /// THE OVERLAY ITSELF: what the host holds for the life of the session.
    ///
    /// The verbs mirror the ones the in-engine overlay has (and had before it moved here), so the runtime's
    /// call sites and the app's panel building keep their shape: `enable_debug_gui()` calls `init()`, the
    /// frame calls `new_frame()` and `record()`, and `chores.cpp` builds panels.
    struct overlay {
        virtual ~overlay() = default;

        /// Bring the overlay up on the device `info` describes. False = it stays inactive, with the plugin's
        /// own named log saying why (a device without the features its backend needs, or a bad `api`).
        [[nodiscard]] virtual bool init(create_info const& info) = 0;
        virtual void shutdown() = 0;
        [[nodiscard]] virtual bool is_active() const noexcept = 0;
        /// true while the overlay owns the mouse (the runtime suppresses camera orbit/zoom with it)
        [[nodiscard]] virtual bool wants_mouse() const noexcept = 0;

        virtual panel& add_panel(std::string title) = 0;
        virtual void remove_panel(panel const& target) = 0;
        virtual void set_panel_visible(panel const& target, bool visible) = 0;
        [[nodiscard]] virtual std::size_t panel_count() const noexcept = 0;

        /// begin a frame's UI (once per frame, after the acquire and before any content)
        virtual void new_frame() const = 0;
        /// draw every visible panel and render the UI's draw data into `native_command_buffer`, which must be
        /// inside an OPEN rendering instance whose colour attachment matches the pipeline's format.
        ///
        /// THE PARAMETER IS A `void*` FOR NOW (the Vulkan plugin casts it to `VkCommandBuffer`): the recording
        /// face's contract handle is not what the overlay's backend is written against yet, and pretending
        /// otherwise here would put the conversion in the host instead of in the plugin that needs it.
        virtual void record(void* native_command_buffer) = 0;
        /// the swapchain was recreated: let the backend adapt (its per-frame ring, its min-image hint)
        virtual void on_swapchain_recreated() const = 0;
    };

    /// The one symbol a GUI plugin exports. An EMPTY pointer is "refused" (with the plugin's own log line as
    /// the diagnosis); `info` is borrowed only until the call returns.
    ///
    /// THE HOST'S VIEW OF IT, as a type: what `deren::utility::dynamic_link`'s resolved address is cast to.
    using make_gui_fn = std::shared_ptr<overlay> (*)(std::uint32_t abi_version, create_info const* info);
} // namespace deren::gui

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif

extern "C" {

/// THE one producer of a GUI overlay, and the only symbol a `deren_gui_<api>.dll` exports.
///
/// `abi_version` is the CALLER's `deren::gui::gui_abi_version`: a mismatch is refused (an empty pointer) with
/// the plugin's own number in its log line. A null `info` is a caller bug, refused for the same reason the
/// backend refuses a null descriptor.
DEREN_API_EXPORT std::shared_ptr<deren::gui::overlay> deren_make_gui(std::uint32_t abi_version, deren::gui::create_info const* info);
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
