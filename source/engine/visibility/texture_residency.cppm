module;
#include <array>
#include <cstdint>
#include <expected>
#include <span>
export module deren.engine.texture_residency;
export import deren.utility.shared;
import deren.engine.visibility;

export namespace deren::engine::texture_residency {
    inline constexpr std::uint32_t material_capacity = 16384;
    inline constexpr std::uint32_t feedback_words = material_capacity / 32;
    struct control {
        bool enabled = false, restore_pending = false;
        void set(bool next) noexcept {
            if (!next && enabled)
                restore_pending = true;
            enabled = next;
        }
    };
    inline bool pin_material_texture(std::size_t slot, bool mask, bool blend, std::uint32_t toon, std::uint32_t overlay) noexcept {
        return toon != 0 || overlay != 0 || blend || slot >= 5 || (slot == 0 && mask);
    }
    struct feedback {
        std::array<std::uint32_t, feedback_words> materials{};
        std::uint32_t invalid = 0;
    };
    inline bool mark(feedback& result, std::uint32_t packed, std::span<visibility::draw_item const> items) noexcept {
        visibility::id value{packed};
        if (!value.valid())
            return true;
        if (value.item() >= items.size() || items[value.item()].material_index >= material_capacity) {
            result.invalid = 1;
            return false;
        }
        auto material = items[value.item()].material_index;
        result.materials[material / 32] |= 1u << (material % 32);
        return true;
    }
    using material_textures = utility::vessel::vector<utility::vessel::vector<std::uint32_t>>;
    enum class error { invalid_feedback,
                       invalid_texture };
    inline std::expected<utility::vessel::vector<std::uint8_t>, error> demand(
        feedback const& visible, material_textures const& edges, std::span<std::uint8_t const> pinned) {
        if (visible.invalid)
            return std::unexpected(error::invalid_feedback);
        utility::vessel::vector<std::uint8_t> result(pinned.begin(), pinned.end());
        for (std::uint32_t m = 0; m < material_capacity; ++m) {
            if (!(visible.materials[m / 32] & (1u << (m % 32))))
                continue;
            if (m >= edges.size())
                return std::unexpected(error::invalid_feedback);
            for (auto id : edges[m]) {
                if (id >= result.size())
                    return std::unexpected(error::invalid_texture);
                result[id] = 1;
            }
        }
        return result;
    }
} // namespace deren::engine::texture_residency
