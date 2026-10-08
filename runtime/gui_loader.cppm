// -*- C++ -*-
// ============================================================================
// file: runtime/gui_loader.cppm
//
// THE GUI PLUGIN'S LOADER - `deren.vulkan.backend_loader`'s shape, one level up.
//
// WHAT IT DOES: resolves `deren_gui_<api>.dll` BESIDE THE EXECUTABLE, finds the one symbol it exports
// (`deren_make_gui`), calls it with the caller's `create_info`, and hands back the overlay. The executable
// therefore LINKS NOTHING of the GUI: no import library, nothing bound before `main`, and a missing DLL is a
// NAMED diagnostic instead of a load-time failure with no story.
//
// WHY ONE DLL PER API (plan X2): the GUI is what drags `vulkan-1.dll` into the program, and each API's GUI
// backend belongs in the plugin that serves it. `api_type` picks the FILE - and the plugin checks it again,
// refusing BY NAME if it was handed an API it does not serve.
// ============================================================================
module;

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

// THE BOUNDARY HEADER BELONGS TO THE GLOBAL MODULE FRAGMENT, for the reason the plugin's own file records:
// it declares `GLFWwindow` itself and it pulls standard headers in, so including it inside the module purview
// attaches all of that to the module (clang reports both as errors - "declaration ... follows declaration in
// the global module" and "attaches the declarations to the named module"). It names only its own types, which
// is what lets it sit here.
#include "../promise/gui/gui_entry.hpp"

export module deren.vulkan.gui_loader;

import deren.utility;              // executable_directory() + the named-diagnosis channel
import deren.utility.dynamic_link; // the mechanism: open the image, resolve the symbol

namespace {
    /// The ONE symbol a GUI plugin exports (promise/gui/gui_entry.hpp).
    constexpr std::string_view gui_entry_symbol = "deren_make_gui";
} // namespace

namespace deren::vulkan {
    /// Load the GUI plugin that serves @p info.api and bring it up.
    ///
    /// An EMPTY pointer is the refusal, and it is always the result of a NAMED log line: an unknown `api`, a
    /// DLL that is not beside the executable, a library without the entry point, or the plugin's own refusal
    /// (an abi mismatch, or an `api_type` it does not serve). The plugin NEVER UNLOADS: the resolved address
    /// has to stay valid for the process's life, which is why the loader object is detached below.
    export [[nodiscard]] std::shared_ptr<deren::gui::overlay> load_gui(deren::gui::create_info const& info) {
        if (info.api == deren::gui::api_type::none) {
            deren::utility::error("gui_loader: the caller asked for no graphics API (gui::api_type::none) - there is no plugin to load");
            return {};
        }
        std::filesystem::path const name = std::string{deren::gui::gui_dll_prefix} + std::string{deren::gui::api_suffix(info.api)} + ".dll";
        std::filesystem::path const dll = deren::utility::executable_directory() / name;
        auto loaded = deren::utility::dynamic_link::load(dll.string());
        if (!loaded.has_value()) {
            deren::utility::error("gui_loader: the {} plugin could not be loaded from {} - {} (code {})", name.string(), dll.string(),
                                  loaded.error().message, loaded.error().code);
            return {};
        }
        auto const symbol = loaded->symbol(gui_entry_symbol);
        if (!symbol.has_value()) {
            deren::utility::error("gui_loader: {} does not export {} - {} (code {})", name.string(), gui_entry_symbol,
                                  symbol.error().message, symbol.error().code);
            return {};
        }
        auto const entry = reinterpret_cast<deren::gui::make_gui_fn>(const_cast<void*>(*symbol));
        static_cast<void>(loaded->detach()); // never unload (see the note above)
        return entry(deren::gui::gui_abi_version, &info);
    }
} // namespace deren::vulkan
