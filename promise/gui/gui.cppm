module;

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
struct GLFWwindow;

export module deren.promise.gui;
export import deren.promise.rhi;

export namespace deren::gui {
    /// The boundary's own version: an INCOMPATIBLE change to the interface or to `create_info` moves it, and
    /// the entry refuses a caller that hands over another number (the backend's abi handshake, one level up).
    inline constexpr std::uint32_t gui_abi_version = 2u;

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

    /// The device root stays borrowed for the overlay's lifetime; the runtime owns it.
    /// No API-specific handles or format numbers cross this boundary.
    struct create_info {
        std::uint32_t struct_size = sizeof(create_info);
        api_type api = api_type::none;
        deren::promise::rhi::api_core* core = nullptr;
        GLFWwindow* window = nullptr;
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
        /// Record inside an open rendering instance on the supplied contract target.
        virtual void record(deren::promise::rhi::command_buffer& commands) = 0;
        /// the swapchain was recreated: let the backend adapt (its per-frame ring, its min-image hint)
        virtual void on_swapchain_recreated() const = 0;
    };

    /// The one symbol a GUI plugin exports. An EMPTY pointer is "refused" (with the plugin's own log line as
    /// the diagnosis); `info` is borrowed only until the call returns.
    ///
    /// THE HOST'S VIEW OF IT, as a type: what `deren::utility::dynamic_link`'s resolved address is cast to.
    using make_gui_fn = std::shared_ptr<overlay> (*)(std::uint32_t abi_version, create_info const* info);
} // namespace deren::gui

