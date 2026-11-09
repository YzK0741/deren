#pragma once

#include <list>

namespace deren::vulkan::gui::detail {
    // Hosts keep pointing at an adapter while other panels are added and removed.
    // Each list node keeps its address until that panel is removed.
    template <typename Adapter>
    using panel_adapter_storage = deren::utility::vessel::list<Adapter>;
} // namespace deren::vulkan::gui::detail
