module;
#include <vulkan/vulkan.h>
module deren.vulkan.core;
import deren.vstd;
import deren.promise.rhi;
import deren.utility.shared;
import :declarations;

namespace deren::vulkan {
    namespace rhi = deren::promise::rhi;

    bool core::frame_snapshot::current() const noexcept {
        return owner && owner->frame.frame_in_flight && owner->frame.frame_acquired &&
               epoch == owner->frame.frame_acquire_epoch && native_handle != VK_NULL_HANDLE &&
               native_handle == owner->frame.frame_image_view.handle();
    }

    core::frame_snapshot::~frame_snapshot() noexcept {
        if (owner) {
            std::lock_guard const lock(owner->resource.contract_images_mutex);
            owner->frame.frame_snapshots.erase(this);
        }
    }

    rhi::shared_object<rhi::image> core::frame_resources_view::retain_image() noexcept {
        if (!owner || !owner->frame.frame_in_flight || !owner->frame.frame_acquired) {
            return {};
        }
        auto keep_core = owner->self_owner.lock();
        if (!keep_core || owner->frame.frame_image_view.handle() == VK_NULL_HANDLE) {
            return {};
        }
        if (auto cached = owner->frame.frame_snapshot_cache.lock(); cached && cached->current()) {
            return cached;
        }
        rhi::shared_object<frame_snapshot> result{new frame_snapshot()};
        result->owner = owner;
        result->keep_alive = keep_core;
        result->self = result;
        result->native_handle = owner->frame.frame_image_view.handle();
        result->captured_extent = owner->frame.frame_image_view.extent();
        result->captured_format = owner->frame.frame_image_view.format();
        result->epoch = owner->frame.frame_acquire_epoch;
        {
            std::lock_guard const lock(owner->resource.contract_images_mutex);
            owner->frame.frame_snapshots.insert(result.get());
        }
        owner->frame.frame_snapshot_cache = result;
        return result;
    }

    rhi::unique_object<rhi::image_view> core::frame_snapshot::make_view(rhi::image_view_desc const& desc) {
        if (!current()) {
            return nullptr;
        }
        rhi::unique_object<rhi::image_view> result = owner->frame.frame_image_view.make_view(desc);
        if (result) {
            auto keep = this->self.lock();
            if (keep) {
                static_cast<owned_image_view*>(result.get())->parent_image = keep;
            }
        }
        return result;
    }

    std::expected<rhi::image_content, rhi::error> core::frame_snapshot::get_content(rhi::image_copy_region const&) const {
        return std::unexpected(rhi::error::unsupported);
    }

    void core::frame_snapshot::release() noexcept {
        // THE SNAPSHOT IS HANDED OUT AS A `shared_object` (`frame_resources::retain_image()`), and this IS
        // the drop its control block performs at the last copy - the same one-reference rule every other
        // RHI handle follows. The destructor unregisters the snapshot from the core's set.
        delete this;
    }
} // namespace deren::vulkan