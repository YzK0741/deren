module;
#include <expected>
#include <memory>
#include <mutex>
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.promise.rhi;
import deren.utility.shared;
import :declarations;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;

    bool core::frame_snapshot::current() const noexcept {
        return owner && owner->frame_in_flight && owner->frame_acquired &&
               epoch == owner->frame_acquire_epoch && native_handle != VK_NULL_HANDLE &&
               native_handle == owner->frame_image_view.handle();
    }

    core::frame_snapshot::~frame_snapshot() noexcept {
        if (owner) {
            std::lock_guard const lock(owner->contract_images_mutex);
            owner->frame_snapshots.erase(this);
        }
    }

    std::shared_ptr<rhi::image> core::frame_resources_view::retain_image() noexcept {
        if (!owner || !owner->frame_in_flight || !owner->frame_acquired)
            return {};
        auto keep_core = owner->weak_from_this().lock();
        if (!keep_core || owner->frame_image_view.handle() == VK_NULL_HANDLE)
            return {};
        if (auto cached = owner->frame_snapshot_cache.lock(); cached && cached->current())
            return cached;
        auto result = std::allocate_shared<frame_snapshot>(deren::utility::memory::allocator<frame_snapshot>{});
        result->owner = std::move(keep_core);
        result->native_handle = owner->frame_image_view.handle();
        result->captured_extent = owner->frame_image_view.extent();
        result->captured_format = owner->frame_image_view.format();
        result->epoch = owner->frame_acquire_epoch;
        {
            std::lock_guard const lock(owner->contract_images_mutex);
            owner->frame_snapshots.insert(result.get());
        }
        owner->frame_snapshot_cache = result;
        return result;
    }

    rhi::object_ptr<rhi::image_view> core::frame_snapshot::make_view(rhi::image_view_desc const& desc) {
        if (!current())
            return nullptr;
        rhi::object_ptr<rhi::image_view> result = owner->frame_image_view.make_view(desc);
        if (result)
            static_cast<owned_image_view*>(result.get())->parent_image = weak_from_this().lock();
        return result;
    }

    std::expected<rhi::image_content, rhi::error> core::frame_snapshot::get_content(rhi::image_copy_region const&) const {
        return std::unexpected(rhi::error::unsupported);
    }

    void core::frame_snapshot::release() noexcept {
        // THE SNAPSHOT IS HANDED OUT AS A shared_ptr (`frame_resources::retain_image()`), so the caller's
        // lifetime handle is that container, never `release()`. Reaching here is a precondition violation.
        deren::utility::panic("rhi: release() on a frame-resource snapshot - it is shared-owner; reset its shared_ptr instead");
    }
} // namespace deren::vulkan
