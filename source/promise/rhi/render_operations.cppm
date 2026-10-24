module;
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <variant>
export module deren.promise.render_operations;
import deren.utility.shared;
import deren.promise.rhi;
export namespace deren::promise::rhi::recording {
    enum class operation_kind : std::uint8_t { pipeline,
                                               vertex,
                                               index,
                                               resource_group,
                                               texture_group,
                                               push,
                                               viewport,
                                               scissor,
                                               cull,
                                               depth_write,
                                               depth_bias,
                                               draw,
                                               draw_indexed,
                                               dispatch,
                                               mesh,
                                               trace,
                                               draw_indirect,
                                               dispatch_indirect,
                                               mesh_indirect,
                                               count };
    using words = deren::utility::vessel::array<std::uint64_t, 16>;
    inline constexpr std::uint64_t bit(operation_kind kind) noexcept {
        return 1ull << static_cast<unsigned>(kind);
    }
    inline constexpr std::uint64_t state_mask = (1ull << static_cast<unsigned>(operation_kind::draw)) - 1;
    struct render_operation {
        words data{};
        deren::utility::vessel::vector<std::byte> bytes;
        resource_group_ref resources;
        texture_group_ref textures;
        virtual ~render_operation() = default;
        [[nodiscard]] virtual operation_kind kind() const noexcept = 0;
        [[nodiscard]] bool is_unique() const noexcept {
            return kind() >= operation_kind::draw;
        }
        [[nodiscard]] bool operator==(render_operation const& other) const noexcept {
            return kind() == other.kind() && data == other.data && bytes == other.bytes &&
                   resources == other.resources && textures == other.textures;
        }
        [[nodiscard]] std::uint64_t reads() const noexcept {
            switch (kind()) {
            case operation_kind::resource_group:
            case operation_kind::texture_group:
            case operation_kind::push:
                return bit(operation_kind::pipeline);
            case operation_kind::dispatch:
            case operation_kind::dispatch_indirect:
            case operation_kind::trace:
                return bit(operation_kind::pipeline) | bit(operation_kind::resource_group) |
                       bit(operation_kind::texture_group) | bit(operation_kind::push);
            default:
                return is_unique() ? state_mask : 0;
            }
        }
    };
    template <operation_kind Kind>
    struct operation final : render_operation {
        [[nodiscard]] operation_kind kind() const noexcept override {
            return Kind;
        }
    };
    using operation_value = std::variant<operation<operation_kind::pipeline>, operation<operation_kind::vertex>, operation<operation_kind::index>, operation<operation_kind::resource_group>, operation<operation_kind::texture_group>, operation<operation_kind::push>, operation<operation_kind::viewport>, operation<operation_kind::scissor>, operation<operation_kind::cull>, operation<operation_kind::depth_write>, operation<operation_kind::depth_bias>, operation<operation_kind::draw>, operation<operation_kind::draw_indexed>, operation<operation_kind::dispatch>, operation<operation_kind::mesh>, operation<operation_kind::trace>, operation<operation_kind::draw_indirect>, operation<operation_kind::dispatch_indirect>, operation<operation_kind::mesh_indirect>>;
    [[nodiscard]] inline render_operation& base(operation_value& value) noexcept {
        return std::visit([](auto& item) -> render_operation& { return item; }, value);
    }
    [[nodiscard]] inline render_operation const& base(operation_value const& value) noexcept {
        return std::visit([](auto const& item) -> render_operation const& { return item; }, value);
    }
    template <typename T>
    [[nodiscard]] words pack(T const& value) noexcept {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(words));
        words result{};
        std::memcpy(result.data(), &value, sizeof(T));
        return result;
    }
    template <typename T>
    [[nodiscard]] T unpack(words const& value) noexcept {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(words));
        T result{};
        std::memcpy(&result, value.data(), sizeof(T));
        return result;
    }
    class operation_stream {
        // Contiguous command-owned slots; no virtual object is allocated separately.
        deren::utility::vessel::vector<operation_value> storage;

    public:
        template <operation_kind Kind>
        render_operation& append(words data = {}, std::span<std::byte const> bytes = {},
                                 resource_group_ref resources = {}, texture_group_ref textures = {}) {
            auto& value = storage.emplace_back(std::in_place_type<operation<Kind>>);
            auto& item = base(value);
            item.data = data;
            item.bytes.assign(bytes.begin(), bytes.end());
            item.resources = std::move(resources);
            item.textures = std::move(textures);
            return item;
        }
        void append_copy(render_operation const& value) {
            switch (value.kind()) {
            case operation_kind::pipeline:
                append<operation_kind::pipeline>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::vertex:
                append<operation_kind::vertex>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::index:
                append<operation_kind::index>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::resource_group:
                append<operation_kind::resource_group>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::texture_group:
                append<operation_kind::texture_group>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::push:
                append<operation_kind::push>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::viewport:
                append<operation_kind::viewport>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::scissor:
                append<operation_kind::scissor>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::cull:
                append<operation_kind::cull>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::depth_write:
                append<operation_kind::depth_write>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::depth_bias:
                append<operation_kind::depth_bias>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::draw:
                append<operation_kind::draw>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::draw_indexed:
                append<operation_kind::draw_indexed>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::dispatch:
                append<operation_kind::dispatch>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::mesh:
                append<operation_kind::mesh>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::trace:
                append<operation_kind::trace>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::draw_indirect:
                append<operation_kind::draw_indirect>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::dispatch_indirect:
                append<operation_kind::dispatch_indirect>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::mesh_indirect:
                append<operation_kind::mesh_indirect>(value.data, value.bytes, value.resources, value.textures);
                break;
            case operation_kind::count:
                break;
            }
        }
        [[nodiscard]] bool empty() const noexcept {
            return storage.empty();
        }
        [[nodiscard]] std::size_t size() const noexcept {
            return storage.size();
        }
        void clear() noexcept {
            storage.clear();
        }
        [[nodiscard]] deren::utility::vessel::vector<render_operation const*> simplify(bool final, bool optimize = true) const {
            deren::utility::vessel::vector<render_operation const*> result;
            result.reserve(storage.size());
            for (auto const& value : storage)
                result.push_back(&base(value));
            if (!optimize)
                return result;
            // Coarse equality filtering never crosses another kind or a protected operation.
            for (std::size_t i = 1; i < result.size(); ++i)
                if (!result[i]->is_unique() && result[i - 1] && *result[i] == *result[i - 1])
                    result[i - 1] = nullptr;
            std::uint64_t live = final ? 0 : state_mask;
            // Covered push words since the latest push consumer. Preserve partial overlaps.
            deren::utility::vessel::vector<std::uint32_t> covered;
            for (std::size_t i = result.size(); i > 0; --i) {
                auto* op = result[i - 1];
                if (!op)
                    continue;
                if (op->is_unique()) {
                    live |= op->reads();
                    if (op->reads() & bit(operation_kind::push))
                        covered.clear();
                    continue;
                }
                auto key = bit(op->kind());
                if (!(live & key)) {
                    result[i - 1] = nullptr;
                    continue;
                }
                if (op->kind() == operation_kind::push) {
                    bool overwritten = true;
                    auto begin = static_cast<std::uint32_t>(op->data[0] / 4);
                    auto end = begin + static_cast<std::uint32_t>((op->bytes.size() + 3) / 4);
                    for (auto word = begin; word < end; ++word) {
                        if (std::find(covered.begin(), covered.end(), word) == covered.end()) {
                            overwritten = false;
                            covered.push_back(word);
                        }
                    }
                    if (overwritten) {
                        result[i - 1] = nullptr;
                        continue;
                    }
                } else
                    live &= ~key;
                live |= op->reads();
            }
            result.erase(std::remove(result.begin(), result.end(), nullptr), result.end());
            return result;
        }
    };
} // namespace deren::promise::rhi::recording
