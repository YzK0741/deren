// The two filtered views over a device root: the application's (what `runtime::operator->` returns) and a
// pass's init view. The application's forwards one contract call; the pass's serves the owner's published
// resources. Neither manages a frame, and the measured reason this file is now ten accessors smaller is in
// the header.

module;

#include <cstdint>
#include <memory>
#include <utility>
#include <vulkan/vulkan.h>

module deren.vulkan.core.filters;

namespace deren::vulkan {

    // =============================================================================================
    // the application's view
    // =============================================================================================

    user_filter::user_filter(std::shared_ptr<deren::promise::rhi::api_core> owner) noexcept
        : owner_share(std::move(owner))
        , core_face(this->owner_share.get()) {
    }

    void user_filter::wait_idle() const noexcept {
        // A contract virtual: this call emits no backend symbol, whichever side of the boundary the object
        // lives on - the property that makes the runtime's `operator->` usable before and after the flip.
        this->core_face->wait_idle();
    }

    // =============================================================================================
    // the pass's view: the resources the owner published - nothing that manages a frame, and no
    // per-generation handle
    // =============================================================================================

    void pass_filter::register_resource(render_resource::resource_id const id, uint32_t const element, resource_handles const handles) noexcept {
        // The key packs the declaration's identity: which resource, and which element of its family. A SECOND
        // registration of the same key REPLACES the first, because the owner re-publishing a resource after a
        // rebuild is the same statement as publishing it.
        uint32_t const key = (static_cast<uint32_t>(id) << 8u) | (element & 0xFFu);
        for (auto& [existing, existing_handles] : this->registered) {
            if (existing == key) {
                existing_handles = handles;
                return;
            }
        }
        this->registered.emplace_back(key, handles);
    }

    resource_handles pass_filter::resource(render_resource::resource_id const id, uint32_t const element) const noexcept {
        uint32_t const key = (static_cast<uint32_t>(id) << 8u) | (element & 0xFFu);
        for (auto const& [existing, handles] : this->registered) {
            if (existing == key) {
                return handles;
            }
        }
        return {}; // the owner has none: a pass's "cannot build" branch, which every pass already has
    }

} // namespace deren::vulkan
