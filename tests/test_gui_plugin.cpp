// Plugin acquisition must initialize or refuse, rather than hand out an inactive overlay.
#include "vk_test.h"

#include <string>

import deren.promise.gui;

import deren.utility.dynamic_link;

#include "../promise/gui/gui_entry.hpp"

static_assert(deren::gui::gui_abi_version == 2u, "GUI recording must use the RHI command buffer boundary");

int main() {
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
