// Plugin acquisition must initialize or refuse, rather than hand out an inactive overlay.
#include "../backends/vulkan/graphical_user_interface/panel_adapter_storage.hpp"
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <string>

import deren.promise.gui;

import deren.utility.dynamic_link;

#include "../promise/gui/gui_entry.hpp"

static_assert(deren::gui::gui_abi_version == 2u, "GUI recording must use the RHI command buffer boundary");

// Exercise the overlay's actual storage without needing a graphics device. Hosts
// retain panel references, so deleting a different panel must preserve its address.
static void retained_panel_addresses_survive_middle_removal() {
    struct panel_identity {
        int id;
    };
    constexpr int count = 7;
    for (int removed : {2, 4}) {
        deren::vulkan::gui::detail::panel_adapter_storage<panel_identity> panels;
        std::array<panel_identity const*, count> retained{};
        for (int id = 0; id < count; ++id) {
            panels.emplace_back(id);
            retained[id] = &panels.back();
        }
        panels.erase(std::next(panels.begin(), removed));
        CHECK(panels.size() == count - 1);
        for (int id = 0; id < count; ++id) {
            if (id == removed) {
                continue;
            }
            auto current = std::find_if(panels.begin(), panels.end(),
                                        [id](panel_identity const& panel) { return panel.id == id; });
            CHECK(current != panels.end());
            if (current != panels.end()) {
                // Never dereference a retained pointer: the broken storage can
                // invalidate it, and this regression must not rely on UB.
                CHECK_MSG(&*current == retained[id], "removing another panel changed a retained panel address");
            }
        }
    }
}

int main() {
    retained_panel_addresses_survive_middle_removal();
    auto library = deren::utility::dynamic_link::load(VR_GUI_PLUGIN_DLL);
    CHECK(library.has_value());
    if (!library) {
        return deren::vk_test::finish("test_gui_plugin");
    }
    auto symbol = library->symbol("deren_make_gui");
    CHECK(symbol.has_value());
    if (!symbol) {
        return deren::vk_test::finish("test_gui_plugin");
    }
    auto entry = reinterpret_cast<deren::gui::make_gui_fn>(const_cast<void*>(*symbol));
    // Graphics plugins stay loaded for the process lifetime, just as gui_loader requires.
    static_cast<void>(library->detach());
    deren::gui::create_info info{};
    CHECK(!entry(deren::gui::gui_abi_version + 1u, &info));
    CHECK(!entry(deren::gui::gui_abi_version, nullptr));
    // Both inputs must be refused without touching a device. Merely allocating the adapter
    // makes these assertions fail, exposing a factory that forgot to call init().
    info.api = deren::gui::api_type::d3d12;
    CHECK(!entry(deren::gui::gui_abi_version, &info));
    info.api = deren::gui::api_type::vulkan;
    CHECK(!entry(deren::gui::gui_abi_version, &info)); // no window
    CHECK(info.core == nullptr);
    info.window = reinterpret_cast<decltype(info.window)>(std::uintptr_t{1});
    CHECK(!entry(deren::gui::gui_abi_version, &info)); // reject absent device before using the window
    info.struct_size = sizeof(info) - 1;
    CHECK(!entry(deren::gui::gui_abi_version, &info)); // truncated ABI2 descriptor
    return deren::vk_test::finish("test_gui_plugin");
}
