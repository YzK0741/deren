// ============================================================================
// THE VULKAN GUI PLUGIN'S C ENTRY (promise/gui/gui_entry.hpp).
//
// WHAT THIS FILE IS: the adapter between the plugin BOUNDARY (an abstract `deren::gui::overlay` + the
// `deren_make_gui` C symbol) and the overlay this module has always had (`gui_content`, `debug_panel`, the
// widget classes). The port is deliberately a WRAP and not a rewrite: every class below this line is
// unchanged, and the host sees verbs instead of objects.
//
// WHY THE HANDLES IN `create_info` ARE `void*`: see the entry header's note. This file is the Vulkan side, so
// it is the one place that casts them back to `VkInstance`/`VkDevice`/`VkQueue`/`VkFormat` - which is exactly
// the kind of code that belongs inside a plugin and not in the engine half.
// ============================================================================
module;

#include <GLFW/glfw3.h>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

// THE BOUNDARY HEADER BELONGS TO THE GLOBAL MODULE FRAGMENT, and that is not a style choice: it declares
// `struct GLFWwindow;` itself, so including it INSIDE the module purview makes the module redeclare a name
// `<GLFW/glfw3.h>` already declared in the global module - clang refuses exactly that. It names only its own
// types (no module types), which is what lets it sit here and be used from the module below.
#include "../../promise/gui/gui_entry.hpp"

module deren.vulkan.graphical_user_interface;

import deren.utility;

namespace deren::vulkan::gui {
    namespace {
        /// ONE PANEL, AS THE BOUNDARY SPEAKS IT: every verb forwards to the `debug_panel` the overlay already
        /// owns, and each widget verb CONSTRUCTS the class this plugin already had.
        class panel_adapter final : public deren::gui::panel {
        public:
            explicit panel_adapter(debug_panel& target) noexcept
                : panel(&target) {
            }

            [[nodiscard]] debug_panel* target() const noexcept {
                return this->panel;
            }

            void add_label(std::string text) override {
                this->panel->push_back(std::make_unique<label_widget>(std::move(text)));
            }
            void add_label(std::function<std::string()> text) override {
                this->panel->push_back(std::make_unique<label_widget>(std::move(text)));
            }
            void add_checkbox(std::string label, bool* value, std::function<void(bool)> on_change) override {
                this->panel->push_back(std::make_unique<checkbox_widget>(std::move(label), value, std::move(on_change)));
            }
            void add_slider(std::string label, float* value, float min, float max, std::function<void(float)> on_change) override {
                this->panel->push_back(std::make_unique<slider_widget>(std::move(label), value, min, max, std::move(on_change)));
            }
            void add_vec3(std::string label, float* value, float speed, std::function<void()> on_change) override {
                this->panel->push_back(std::make_unique<vec3_widget>(std::move(label), value, speed, std::move(on_change)));
            }
            void add_combo(std::string label, std::vector<std::string> items, std::int32_t* current_item, std::function<void(std::int32_t)> on_change) override {
                this->panel->push_back(std::make_unique<combo_widget>(std::move(label), std::move(items), current_item, std::move(on_change)));
            }

            void clear() override {
                this->panel->clear();
            }
            [[nodiscard]] bool empty() const noexcept override {
                return this->panel->empty();
            }
            [[nodiscard]] std::size_t size() const noexcept override {
                return this->panel->size();
            }

            [[nodiscard]] std::string const& title() const noexcept override {
                return this->panel->get_title();
            }
            void set_open(bool open) noexcept override {
                this->panel->set_open(open);
            }
            [[nodiscard]] bool open() const noexcept override {
                return this->panel->open();
            }
            void set_default_size(float width, float height) noexcept override {
                this->panel->set_default_size(width, height);
            }

        private:
            debug_panel* panel = nullptr;
        };

        /// THE OVERLAY, AS THE BOUNDARY SPEAKS IT: one `gui_content` (unchanged) plus the panel adapters this
        /// plugin hands out. THE ADAPTERS LIVE IN A DEQUE because a `deren::gui::panel&` the host keeps has to
        /// stay valid across later `add_panel` calls (a vector would move it).
        class overlay_adapter final : public deren::gui::overlay {
        public:
            [[nodiscard]] bool init(deren::gui::create_info const& info) override {
                if (info.api != deren::gui::api_type::vulkan) {
                    deren::utility::log("gui plugin: this is the VULKAN gui plugin and it was asked for api_type {} - refusing BY NAME",
                                        static_cast<std::uint32_t>(info.api));
                    return false;
                }
                if (info.window == nullptr) {
                    deren::utility::log("gui plugin: create_info carried no window");
                    return false;
                }
                gui_create_info translated = {};
                translated.window = info.window;
                translated.instance = static_cast<VkInstance>(info.instance);
                translated.physical_device = static_cast<VkPhysicalDevice>(info.physical_device);
                translated.device = static_cast<VkDevice>(info.device);
                translated.graphics_queue_family = info.graphics_queue_family;
                translated.graphics_queue = static_cast<VkQueue>(info.graphics_queue);
                translated.color_format = static_cast<VkFormat>(info.color_format);
                translated.depth_format = static_cast<VkFormat>(info.depth_format);
                translated.frames_in_flight = info.frames_in_flight;
                return this->content.init(translated);
            }

            void shutdown() override {
                this->panels.clear();
                this->content.shutdown();
            }
            [[nodiscard]] bool is_active() const noexcept override {
                return this->content.is_active();
            }
            [[nodiscard]] bool wants_mouse() const noexcept override {
                return this->content.wants_mouse();
            }

            deren::gui::panel& add_panel(std::string title) override {
                this->panels.emplace_back(this->content.add_panel(std::move(title)));
                return this->panels.back();
            }
            void remove_panel(deren::gui::panel const& target) override {
                for (auto it = this->panels.begin(); it != this->panels.end(); ++it) {
                    if (static_cast<deren::gui::panel const*>(&*it) == &target) {
                        if (debug_panel* const owned = it->target(); owned != nullptr) {
                            this->content.remove_panel(*owned);
                        }
                        this->panels.erase(it);
                        return;
                    }
                }
            }
            void set_panel_visible(deren::gui::panel const& target, bool visible) override {
                for (panel_adapter const& adapter : this->panels) {
                    if (static_cast<deren::gui::panel const*>(&adapter) == &target) {
                        if (debug_panel* const owned = adapter.target(); owned != nullptr) {
                            this->content.set_panel_visible(*owned, visible);
                        }
                        return;
                    }
                }
            }
            [[nodiscard]] std::size_t panel_count() const noexcept override {
                return this->content.panel_count();
            }

            void new_frame() const override {
                this->content.new_frame();
            }
            void record(void* native_command_buffer) override {
                this->content.record(static_cast<VkCommandBuffer>(native_command_buffer));
            }
            void on_swapchain_recreated() const override {
                this->content.on_swapchain_recreated();
            }

        private:
            gui_content content;
            /// the adapters, one per `add_panel` (see the type's note: a deque, because the host keeps
            /// references past later calls)
            std::deque<panel_adapter> panels;
        };
    } // namespace
} // namespace deren::vulkan::gui

extern "C" DEREN_API_EXPORT std::shared_ptr<deren::gui::overlay> deren_make_gui(std::uint32_t const abi_version, deren::gui::create_info const* const info) {
    if (abi_version != deren::gui::gui_abi_version) {
        deren::utility::log("gui plugin: abi mismatch - the caller compiled {} and this plugin is {}", abi_version, deren::gui::gui_abi_version);
        return {};
    }
    if (info == nullptr) {
        deren::utility::log("gui plugin: the create_info was null");
        return {};
    }
    std::shared_ptr<deren::gui::overlay> overlay = std::make_shared<deren::vulkan::gui::overlay_adapter>();
    deren::utility::log("gui plugin: deren_gui_vulkan is up (api_type {}, frames in flight {})",
                        static_cast<std::uint32_t>(info->api), info->frames_in_flight);
    return overlay;
}
