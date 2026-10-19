// ============================================================================
// THE VULKAN GUI PLUGIN'S C ENTRY (source/promise/gui/gui_entry.hpp).
//
// WHAT THIS FILE IS: the adapter between the plugin BOUNDARY (an abstract `deren::gui::overlay` + the
// `deren_make_gui` C symbol) and the overlay this module has always had (`gui_content`, `debug_panel`, the
// widget classes). The port is deliberately a WRAP and not a rewrite: every class below this line is
// unchanged, and the host sees verbs instead of objects.
//
// The boundary carries a borrowed PLATFORM WINDOW and the RHI's opaque main-thread context (abi 39).
// The context is cast HERE, through the shared Vulkan layout (`denen.vulkan.main_context`), because the
// native handles and the presentation format are this API's vocabulary and must not cross the contract.
// abi 43 deleted the escape: `record()` receives the RHI `command_buffer`, checks its concrete tag and casts
// it to the shared `denen.vulkan.command_buffer` head to read the `VkCommandBuffer` ImGui records into.
// ============================================================================
module;

#include <GLFW/glfw3.h>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

module deren.vulkan.graphical_user_interface;
import deren.utility.shared;

import deren.promise.gui;

// THE SHARED CONCRETE LAYOUT of the contract's opaque main context (abi 39): the backend/plugin pair
// compiles the same interface unit, so the cast below is to the layout the backend actually filled.
import deren.vulkan.main_context;
import deren.vulkan.command_buffer;

import deren.utility;

#include "../../../promise/gui/gui_entry.hpp"
#include "panel_adapter_storage.hpp"

namespace deren::vulkan::gui {
    namespace {
        /// ONE PANEL, AS THE BOUNDARY SPEAKS IT: every verb forwards to the `debug_panel` the overlay already
        /// owns, and each widget verb CONSTRUCTS the class this plugin already had.
        /// ONE WIDGET, AS THE BOUNDARY SPEAKS IT: it holds the class the plugin already had, so the only verb
        /// the host can call on it - a visibility predicate - writes the member that class already has.
        class widget_adapter final : public deren::gui::widget {
        public:
            /// THE PLUGIN'S OWN widget CLASS, qualified: inside this adapter widget would name the BOUNDARY
            /// interface it derives from, and the member has to be the class that actually has isible_when.
            explicit widget_adapter(deren::vulkan::gui::widget& target) noexcept
                : item(&target) {
            }
            void set_visible_when(std::function<bool()> predicate) override {
                this->item->visible_when = std::move(predicate);
            }

        private:
            deren::vulkan::gui::widget* item = nullptr;
        };

        class panel_adapter final : public deren::gui::panel {
        public:
            explicit panel_adapter(debug_panel& target) noexcept
                : panel(&target) {
            }

            [[nodiscard]] debug_panel* target() const noexcept {
                return this->panel;
            }

            deren::gui::widget& add_label(deren::utility::vessel::string text) override {
                return this->keep(std::make_unique<label_widget>(std::move(text)));
            }
            deren::gui::widget& add_label(std::function<deren::utility::vessel::string()> text) override {
                return this->keep(std::make_unique<label_widget>(std::move(text)));
            }
            deren::gui::widget& add_checkbox(deren::utility::vessel::string label, bool* value, std::function<void(bool)> on_change) override {
                return this->keep(std::make_unique<checkbox_widget>(std::move(label), value, std::move(on_change)));
            }
            deren::gui::widget& add_slider(deren::utility::vessel::string label, float* value, float min, float max, std::function<void(float)> on_change) override {
                return this->keep(std::make_unique<slider_widget>(std::move(label), value, min, max, std::move(on_change)));
            }
            deren::gui::widget& add_vec3(deren::utility::vessel::string label, float* value, float speed, std::function<void()> on_change) override {
                return this->keep(std::make_unique<vec3_widget>(std::move(label), value, speed, std::move(on_change)));
            }
            deren::gui::widget& add_combo(deren::utility::vessel::string label, deren::utility::vessel::vector<deren::utility::vessel::string> items, std::int32_t* current_item, std::function<void(std::int32_t)> on_change) override {
                return this->keep(std::make_unique<combo_widget>(std::move(label), std::move(items), current_item, std::move(on_change)));
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

            [[nodiscard]] deren::utility::vessel::string const& title() const noexcept override {
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
            /// Append @p item to the panel and hand back a STABLE adapter for it: the adapters live in a deque
            /// because the host keeps the returned reference across later `add_*` calls.
            deren::gui::widget& keep(std::unique_ptr<deren::vulkan::gui::widget> item) {
                widget* const raw = item.get();
                this->panel->push_back(std::move(item));
                this->widgets.emplace_back(*raw);
                return this->widgets.back();
            }

            debug_panel* panel = nullptr;
            deren::utility::vessel::deque<widget_adapter> widgets;
        };

        /// THE OVERLAY, AS THE BOUNDARY SPEAKS IT: one `gui_content` (unchanged) plus the panel adapters this
        /// plugin hands out. Each panel adapter lives in its own owned list node, so a retained
        /// `deren::gui::panel&` stays valid when other panels are added or removed.
        class overlay_adapter final : public deren::gui::overlay {
        public:
            ~overlay_adapter() override {
                this->shutdown();
            }

            [[nodiscard]] bool init(deren::gui::create_info const& info) override {
                if (info.struct_size < sizeof(info)) {
                    deren::utility::log("gui plugin: the create_info prefix is too short for GUI ABI4");
                    return false;
                }
                if (this->content.is_active()) {
                    return true;
                }
                if (info.api != deren::gui::api_type::vulkan) {
                    deren::utility::log("gui plugin: this is the VULKAN gui plugin and it was asked for api_type {} - refusing BY NAME",
                                        static_cast<std::uint32_t>(info.api));
                    return false;
                }
                if (info.window == nullptr) {
                    deren::utility::log("gui plugin: create_info carried no window");
                    return false;
                }
                namespace rhi = deren::promise::rhi;
                // THE OPAQUE CONTEXT IS THE DEVICE ROOT NOW (abi 39): this plugin is the VULKAN one, so it
                // knows its own concrete layout and casts - the contract only carries the tag. A context this
                // plugin does not recognise is refused by name, exactly like a wrong api_type.
                if (info.main_context == nullptr || !deren::vulkan::is_main_context(*info.main_context)) {
                    deren::utility::log("gui plugin: the create_info carried no main context this Vulkan plugin serves");
                    return false;
                }
                auto const* const context = rhi::do_concrete<deren::vulkan::main_context>(*info.main_context, deren::vulkan::main_context_name);
                if (context->core == nullptr || context->core->api_version() != rhi::abi_version) {
                    deren::utility::log("gui plugin: no compatible RHI device root in the main context");
                    return false;
                }
                gui_create_info translated = {};
                translated.window = info.window;
                translated.instance = context->instance;
                translated.physical_device = context->physical_device;
                translated.device = context->device;
                translated.graphics_queue_family = context->graphics_queue_family;
                translated.graphics_queue = context->graphics_queue;
                translated.color_format = static_cast<VkFormat>(context->color_format);
                translated.depth_format = VK_FORMAT_UNDEFINED;
                translated.frames_in_flight = context->frames_in_flight;
                // GLFW is linked statically into each image. The host's glfwInit() does not
                // initialize this plugin's copy, whose queries otherwise return zero sizes/scale.
                if (!glfwInit()) {
                    deren::utility::log("gui plugin: GLFW initialization failed");
                    return false;
                }
                this->platform_ready = true;
                if (!this->content.init(translated)) {
                    this->shutdown();
                    return false;
                }
                return true;
            }

            void shutdown() override {
                this->panels.clear();
                this->content.shutdown();
                if (this->platform_ready) {
                    glfwTerminate();
                    this->platform_ready = false;
                }
            }
            [[nodiscard]] bool is_active() const noexcept override {
                return this->content.is_active();
            }
            [[nodiscard]] bool wants_mouse() const noexcept override {
                return this->content.wants_mouse();
            }

            deren::gui::panel& add_panel(deren::utility::vessel::string title) override {
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
            void record(deren::promise::rhi::command_buffer& commands) override {
                // abi 43: the recording target arrives as the RHI handle and is converted HERE, on this side
                // of the boundary: the contract names no native type, the plugin checks the concrete tag and
                // casts the handle to the shared Vulkan layout. The same rule the device root follows.
                if (!deren::vulkan::is_command_buffer(commands)) {
                    deren::utility::log("gui plugin: the command buffer does not belong to this device");
                    return;
                }
                auto const native = deren::vulkan::native_command_buffer(commands);
                if (native == VK_NULL_HANDLE) {
                    deren::utility::log("gui plugin: the recording target names no frame in flight");
                    return;
                }
                this->content.record(native);
            }
            void on_swapchain_recreated() const override {
                this->content.on_swapchain_recreated();
            }

        private:
            gui_content content;
            bool platform_ready = false;
            /// One stable owned node per panel; erasure destroys only the removed adapter.
            detail::panel_adapter_storage<panel_adapter> panels;
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
    if (info->struct_size < sizeof(deren::gui::create_info)) {
        deren::utility::log("gui plugin: the create_info prefix is too short for GUI ABI4");
        return {};
    }
    std::shared_ptr<deren::gui::overlay> overlay = std::make_shared<deren::vulkan::gui::overlay_adapter>();
    if (!overlay->init(*info)) {
        return {};
    }
    deren::utility::log("gui plugin: deren_gui_vulkan is up (api_type {})", static_cast<std::uint32_t>(info->api));
    return overlay;
}
