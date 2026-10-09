// C entry name for a GUI plugin. Include after importing deren.promise.gui.
#pragma once
#include "../../utility/abi_export.hpp"
#include <cstdint>
#include <memory>

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
