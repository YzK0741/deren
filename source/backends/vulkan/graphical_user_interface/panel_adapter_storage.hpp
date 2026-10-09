#pragma once

#include <list>

namespace deren::vulkan::gui::detail {
    // Hosts retain adapter references across additions and removals of other
    // panels. Each owned node keeps its address until that panel is removed.
    template <typename Adapter>
    using panel_adapter_storage = std::list<Adapter>;
}
