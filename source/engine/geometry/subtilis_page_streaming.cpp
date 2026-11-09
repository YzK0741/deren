module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
module deren.engine.subtilis.page_streaming;
import deren.vstd;
import deren.asset.subtilis.types;
import deren.engine.subtilis.residency;

namespace deren::engine::subtilis {
    std::expected<page_arena_layout, residency_error>
    page_arena_layout::create(std::span<page_entry const> pages, std::uint32_t alignment) {
        if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) {
            return std::unexpected(residency_error::invalid_page_table);
        }
        page_arena_layout layout;
        layout.alignment_ = alignment;
        layout.offsets_.reserve(pages.size());
        std::uint64_t cursor = 0;
        for (auto const& page : pages) {
            if (page.count == 0u || page.bytes == 0u || page.bytes > deren::asset::subtilis::page_bytes) {
                return std::unexpected(residency_error::invalid_page_table);
            }
            auto const mask = static_cast<std::uint64_t>(alignment) - 1u;
            auto const aligned = (cursor + mask) & ~mask;
            if (aligned > std::numeric_limits<std::uint32_t>::max() ||
                static_cast<std::uint64_t>(page.bytes) > std::numeric_limits<std::uint32_t>::max() - aligned) {
                return std::unexpected(residency_error::capacity);
            }
            layout.offsets_.push_back(static_cast<std::uint32_t>(aligned));
            cursor = aligned + page.bytes;
        }
        layout.bytes_ = cursor;
        return layout;
    }

    std::expected<page_arena_payload, residency_error>
    pack_page_arena(std::span<page_entry const> pages, std::span<page_section_view const> sections,
                    std::uint32_t alignment) {
        if (sections.size() != 6u) {
            return std::unexpected(residency_error::invalid_page_table);
        }
        auto layout = page_arena_layout::create(pages, alignment);
        if (!layout) {
            return std::unexpected(layout.error());
        }
        auto const arena_bytes = layout->bytes();
        page_arena_payload payload{std::move(*layout), utility::vessel::vector<std::byte>(arena_bytes, std::byte{0})};
        for (std::size_t i = 0; i < pages.size(); ++i) {
            auto const& page = pages[i];
            if (page.section == 0u || page.section > 6u) {
                return std::unexpected(residency_error::invalid_page_table);
            }
            auto const& section = sections[page.section - 1u];
            auto const stride = section.stride;
            auto const elements = stride == 0u ? 0u : section.bytes.size() / stride;
            if (stride == 0u || static_cast<std::uint64_t>(page.count) * stride != page.bytes ||
                page.bytes > deren::asset::subtilis::page_bytes ||
                page.first > elements || page.count > elements - page.first) {
                return std::unexpected(residency_error::invalid_page_table);
            }
            auto const offset = payload.layout.page_offset(static_cast<std::uint32_t>(i));
            if (!offset) {
                return std::unexpected(residency_error::invalid_page_table);
            }
            std::memcpy(payload.bytes.data() + *offset, section.bytes.data() + static_cast<std::size_t>(page.first) * stride, page.bytes);
        }
        return payload;
    }

    std::expected<utility::vessel::vector<gpu_page_table_entry>, residency_error>
    make_gpu_page_table(std::span<page_entry const> pages, page_arena_layout const& layout, page_manager const& manager) {
        if (layout.page_count() != pages.size() || manager.pages().size() != pages.size()) {
            return std::unexpected(residency_error::invalid_page_table);
        }
        utility::vessel::vector<gpu_page_table_entry> table;
        table.reserve(pages.size());
        for (std::uint32_t i = 0; i < pages.size(); ++i) {
            auto offset = layout.page_offset(i);
            if (!offset) {
                return std::unexpected(residency_error::invalid_page_table);
            }
            auto const& page = pages[i];
            table.push_back({
                .page = i,
                .section = page.section,
                .first = page.first,
                .count = page.count,
                .offset = *offset,
                .bytes = page.bytes,
                .state = static_cast<std::uint32_t>(manager.resident(i) ? gpu_page_state::resident : gpu_page_state::absent),
                .flags = page.flags,
            });
        }
        return table;
    }

    page_request_buffer::page_request_buffer(std::uint32_t capacity)
        : capacity_(capacity)
        , storage_(sizeof(gpu_page_request_header) + static_cast<std::size_t>(capacity) * sizeof(gpu_page_request), std::byte{0}) {
        clear();
    }

    gpu_page_request_header page_request_buffer::header() const noexcept {
        gpu_page_request_header result{};
        std::memcpy(&result, storage_.data(), sizeof(result));
        return result;
    }

    std::optional<gpu_page_request> page_request_buffer::request(std::uint32_t index) const noexcept {
        auto const state = header();
        if (index >= state.count || index >= capacity_) {
            return std::nullopt;
        }
        gpu_page_request result{};
        std::memcpy(&result, storage_.data() + sizeof(gpu_page_request_header) + static_cast<std::size_t>(index) * sizeof(result), sizeof(result));
        return result;
    }

    bool page_request_buffer::push(gpu_page_request const& request) noexcept {
        auto state = header();
        if (state.count >= capacity_) {
            state.overflow = 1;
            std::memcpy(storage_.data(), &state, sizeof(state));
            return false;
        }
        std::memcpy(storage_.data() + sizeof(gpu_page_request_header) + static_cast<std::size_t>(state.count) * sizeof(request), &request, sizeof(request));
        ++state.count;
        std::memcpy(storage_.data(), &state, sizeof(state));
        return true;
    }

    void page_request_buffer::clear(std::uint32_t frame) noexcept {
        gpu_page_request_header const state{.count = 0, .capacity = capacity_, .overflow = 0, .frame = frame};
        std::memcpy(storage_.data(), &state, sizeof(state));
    }

    page_streamer::page_streamer(page_manager manager, std::uint32_t capacity)
        : manager_(std::move(manager))
        , capacity_(capacity) {
        queue_.reserve(capacity);
    }

    std::expected<std::uint64_t, residency_error>
    page_streamer::request(std::uint32_t page, std::uint32_t priority, std::uint32_t item, std::uint32_t frame) {
        if (page >= manager_.pages().size()) {
            return std::unexpected(residency_error::invalid_page);
        }
        ++stats_.requests;
        if (active_ && active_->page == page) {
            ++stats_.coalesced;
            return active_->id;
        }
        for (auto const& queued : queue_) {
            if (queued.page == page) {
                ++stats_.coalesced;
                return queued.id;
            }
        }
        if (queue_.size() >= capacity_) {
            return std::unexpected(residency_error::capacity);
        }
        auto const id = next_id_++;
        queue_.push_back({.id = id, .page = page, .priority = priority, .item = item, .frame = frame});
        return id;
    }

    std::expected<std::optional<page_transfer>, residency_error> page_streamer::begin() {
        if (active_) {
            return std::optional<page_transfer>{};
        }
        if (queue_.empty()) {
            return std::optional<page_transfer>{};
        }
        auto const request = queue_.front();
        queue_.erase(queue_.begin());
        auto planned = manager_.plan_request(request.page);
        if (!planned) {
            ++stats_.failures;
            return std::unexpected(planned.error());
        }
        active_ = request;
        active_plan_ = std::move(*planned);
        ++stats_.starts;
        return std::optional<page_transfer>{page_transfer{
            .id = request.id,
            .page = request.page,
            .bytes = manager_.pages()[request.page].bytes,
        }};
    }

    std::expected<void, residency_error> page_streamer::complete() {
        if (!active_ || !active_plan_) {
            return std::unexpected(residency_error::invalid_page);
        }
        auto applied = manager_.apply(std::move(*active_plan_));
        active_plan_.reset();
        active_.reset();
        if (!applied) {
            ++stats_.failures;
            return applied;
        }
        ++stats_.completions;
        return {};
    }

    bool page_streamer::pending(std::uint32_t page) const noexcept {
        if (active_ && active_->page == page) {
            return true;
        }
        return std::ranges::any_of(queue_, [page](auto const& queued) { return queued.page == page; });
    }

    streaming_stats page_streamer::stats() const noexcept {
        auto result = stats_;
        result.queued = static_cast<std::uint32_t>(queue_.size());
        return result;
    }
} // namespace deren::engine::subtilis
