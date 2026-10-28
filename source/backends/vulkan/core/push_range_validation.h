#pragma once
#include <algorithm>
#include <cstdint>

namespace deren::vulkan::detail {
    // Sorted union of half-open written-byte ranges, stored as (offset, size).
    // Metadata is already bounded by push_constants validation before recording.
    template<class Ranges>
    void merge_push_range(Ranges& ranges, std::uint64_t offset, std::uint64_t size) {
        if (!size) return;
        auto begin = offset;
        auto end = offset + size;
        auto first = ranges.begin();
        while (first != ranges.end() && first->first + first->second < begin) ++first;
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
}
