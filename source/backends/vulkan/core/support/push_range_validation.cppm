// -*- C++ -*-
// ============================================================================
// module: deren.vulkan.core_support:push_range_validation
//
// The sorted-union helper for recorded push-constant ranges. It is a partition
// of the standalone support module so its unit test can import it without
// linking the backend.
// ============================================================================
export module deren.vulkan.core_support:push_range_validation;

import deren.vstd;

namespace deren::vulkan::detail {
    // Sorted union of half-open written-byte ranges, stored as (offset, size).
    // Metadata is already bounded by push_constants validation before recording.
    export template <class Ranges>
    void merge_push_range(Ranges& ranges, std::uint64_t offset, std::uint64_t size) {
        if (!size)
            return;
        auto begin = offset;
        auto end = offset + size;
        auto first = ranges.begin();
        while (first != ranges.end() && first->first + first->second < begin)
            ++first;
        auto last = first;
        while (last != ranges.end() && last->first <= end) {
            begin = std::min(begin, last->first);
            end = std::max(end, last->first + last->second);
            ++last;
        }
        if (first == last) {
            ranges.emplace(first, begin, end - begin);
        } else {
            first->first = begin;
            first->second = end - begin;
            ranges.erase(first + 1, last);
        }
    }
} // namespace deren::vulkan::detail
