#pragma once
#include <vulkan/vulkan.h>

namespace deren::vulkan::detail {
    struct upload_result {
        VkResult status;
        bool may_be_pending;
    };
    // Never wait after failed submit. An unsuccessful wait other than device loss does not prove
    // completion: the caller must preserve resources or terminate rather than recycle/free them.
    template <class Begin, class Record, class End, class Submit, class Wait, class Reset>
    [[nodiscard]] upload_result upload_sequence(Begin&& begin, Record&& record, End&& end,
                                                Submit&& submit, Wait&& wait, Reset&& reset) {
        auto status = begin();
        if (status != VK_SUCCESS) return {status, false};
        record();
        status = end();
        if (status != VK_SUCCESS) return {status, false};
        status = submit();
        if (status != VK_SUCCESS) return {status, false};
        status = wait();
        if (status != VK_SUCCESS) return {status, status != VK_ERROR_DEVICE_LOST};
        return {reset(), false};
    }
} // namespace deren::vulkan::detail
