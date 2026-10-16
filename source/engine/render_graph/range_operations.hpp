#pragma once
// Internal CPU range operations; include after the primary module declaration.
namespace deren::engine::render_graph::detail {
    inline uint32_t bits(texture_aspect aspect) noexcept {
        return static_cast<uint32_t>(aspect);
    }
    inline bool valid_shape(resource_shape const& shape) noexcept {
        if (auto t = std::get_if<texture_shape>(&shape))
            return t->mip_levels && t->array_layers && bits(t->aspects) && !(bits(t->aspects) & ~7U);
        return std::get<buffer_shape>(shape).byte_size != 0;
    }
    inline resource_range full_range(resource_shape const& shape) {
        if (auto t = std::get_if<texture_shape>(&shape))
            return texture_range{0, t->mip_levels, 0, t->array_layers, t->aspects};
        return buffer_range{0, std::get<buffer_shape>(shape).byte_size};
    }
    inline std::expected<resource_range, error_code> normalize(resource_shape const& shape, resource_range input) {
        if (shape.index() != input.index())
            return std::unexpected(error_code::invalid_range);
        if (auto r = std::get_if<texture_range>(&input)) {
            auto const& s = std::get<texture_shape>(shape);
            if (r->base_mip >= s.mip_levels || r->base_layer >= s.array_layers)
                return std::unexpected(error_code::invalid_range);
            if (r->mip_count == remaining_subresources)
                r->mip_count = s.mip_levels - r->base_mip;
            if (r->layer_count == remaining_subresources)
                r->layer_count = s.array_layers - r->base_layer;
            if (r->aspects == texture_aspect::all)
                r->aspects = s.aspects;
            if (!r->mip_count || r->mip_count > s.mip_levels - r->base_mip || !r->layer_count || r->layer_count > s.array_layers - r->base_layer || !bits(r->aspects) || (bits(r->aspects) & ~bits(s.aspects)))
                return std::unexpected(error_code::invalid_range);
        } else {
            auto& bytes = std::get<buffer_range>(input);
            auto size = std::get<buffer_shape>(shape).byte_size;
            if (bytes.offset >= size)
                return std::unexpected(error_code::invalid_range);
            if (bytes.size == remaining_bytes)
                bytes.size = size - bytes.offset;
            if (!bytes.size || bytes.size > size - bytes.offset)
                return std::unexpected(error_code::invalid_range);
        }
        return input;
    }
    inline std::optional<resource_range> intersection(resource_range const& a, resource_range const& b) {
        if (a.index() != b.index())
            return std::nullopt;
        if (auto x = std::get_if<buffer_range>(&a)) {
            auto const& y = std::get<buffer_range>(b);
            auto begin = std::max(x->offset, y.offset), end = std::min(x->offset + x->size, y.offset + y.size);
            if (begin >= end)
                return std::nullopt;
            return buffer_range{begin, end - begin};
        }
        auto const& x = std::get<texture_range>(a);
        auto const& y = std::get<texture_range>(b);
        auto aspects = bits(x.aspects) & bits(y.aspects);
        auto mip_begin = std::max(x.base_mip, y.base_mip), mip_end = std::min(x.base_mip + x.mip_count, y.base_mip + y.mip_count);
        auto layer_begin = std::max(x.base_layer, y.base_layer), layer_end = std::min(x.base_layer + x.layer_count, y.base_layer + y.layer_count);
        if (!aspects || mip_begin >= mip_end || layer_begin >= layer_end)
            return std::nullopt;
        return texture_range{mip_begin, mip_end - mip_begin, layer_begin, layer_end - layer_begin, static_cast<texture_aspect>(aspects)};
    }
    struct partition_result {
        std::array<resource_range, 5> outside;
        std::size_t count = 0;
        std::optional<resource_range> inside;
    };
    inline partition_result partition(resource_range const& whole, resource_range const& requested) {
        partition_result result{};
        result.inside = intersection(whole, requested);
        if (!result.inside) {
            result.outside[result.count++] = whole;
            return result;
        }
        if (auto x = std::get_if<buffer_range>(&whole)) {
            auto const& in = std::get<buffer_range>(*result.inside);
            if (x->offset < in.offset)
                result.outside[result.count++] = buffer_range{x->offset, in.offset - x->offset};
            auto end = x->offset + x->size, inside_end = in.offset + in.size;
            if (inside_end < end)
                result.outside[result.count++] = buffer_range{inside_end, end - inside_end};
            return result;
        }
        auto const& x = std::get<texture_range>(whole);
        auto const& in = std::get<texture_range>(*result.inside);
        auto remaining = bits(x.aspects) & ~bits(in.aspects);
        if (remaining)
            result.outside[result.count++] = texture_range{x.base_mip, x.mip_count, x.base_layer, x.layer_count, static_cast<texture_aspect>(remaining)};
        auto emit = [&](uint32_t mip, uint32_t mip_count, uint32_t layer, uint32_t layer_count) {
            if (mip_count && layer_count)
                result.outside[result.count++] = texture_range{mip, mip_count, layer, layer_count, in.aspects};
        };
        emit(x.base_mip, in.base_mip - x.base_mip, x.base_layer, x.layer_count);
        emit(in.base_mip + in.mip_count, x.base_mip + x.mip_count - in.base_mip - in.mip_count, x.base_layer, x.layer_count);
        emit(in.base_mip, in.mip_count, x.base_layer, in.base_layer - x.base_layer);
        emit(in.base_mip, in.mip_count, in.base_layer + in.layer_count, x.base_layer + x.layer_count - in.base_layer - in.layer_count);
        return result;
    }
    template <typename State>
    deren::utility::vessel::vector<std::size_t> isolate(deren::utility::vessel::vector<State>& states, resource_range const& requested) {
        deren::utility::vessel::vector<State> next;
        deren::utility::vessel::vector<std::size_t> touched;
        next.reserve(states.size());
        for (auto& state : states) {
            auto parts = partition(state.range, requested);
            if (!parts.inside) {
                next.push_back(std::move(state));
                continue;
            }
            for (std::size_t i = 0; i < parts.count; ++i) {
                auto outside = state;
                outside.range = parts.outside[i];
                next.push_back(std::move(outside));
            }
            touched.push_back(next.size());
            state.range = *parts.inside;
            next.push_back(std::move(state));
        }
        states = std::move(next);
        return touched;
    }
} // namespace deren::engine::render_graph::detail
