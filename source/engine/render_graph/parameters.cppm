module;
#include <array>
#include <concepts>
#include <optional>
#include <span>
export module deren.engine.render_graph:parameters;
export import :types;
export namespace deren::engine::render_graph {
    template <content_access Mode>
    struct texture_parameter {
        static_assert(Mode >= content_access::read && Mode <= content_access::invalidate);
        texture_ref resource;
        texture_range range = {};
        access_scope scope = {};
    };
    template <content_access Mode>
    struct buffer_parameter {
        static_assert(Mode >= content_access::read && Mode <= content_access::invalidate);
        buffer_ref resource;
        buffer_range range = {};
        access_scope scope = {};
    };
    using texture_read = texture_parameter<content_access::read>;
    using texture_discard_write = texture_parameter<content_access::discard_write>;
    using texture_preserve_write = texture_parameter<content_access::preserve_write>;
    using texture_read_write = texture_parameter<content_access::read_write>;
    using texture_invalidate = texture_parameter<content_access::invalidate>;
    using buffer_read = buffer_parameter<content_access::read>;
    using buffer_discard_write = buffer_parameter<content_access::discard_write>;
    using buffer_preserve_write = buffer_parameter<content_access::preserve_write>;
    using buffer_read_write = buffer_parameter<content_access::read_write>;
    using buffer_invalidate = buffer_parameter<content_access::invalidate>;

    class parameter_visitor;
    template <typename T>
    concept pass_parameters = requires(T const& value, parameter_visitor& visit) {
        { value.visit_resources(visit) } noexcept -> std::same_as<void>;
    };

    /// Explicit resource enumeration, without reflection or implicit scalar handling.
    class parameter_visitor {
        friend class builder;
        deren::utility::vessel::vector<resource_access>& accesses;
        explicit parameter_visitor(deren::utility::vessel::vector<resource_access>& output) noexcept
            : accesses(output) {
        }

    public:
        template <content_access Mode>
        void operator()(texture_parameter<Mode> const& field) noexcept {
            accesses.emplace_back(field.resource, Mode, field.range, field.scope);
        }
        template <content_access Mode>
        void operator()(buffer_parameter<Mode> const& field) noexcept {
            accesses.emplace_back(field.resource, Mode, field.range, field.scope);
        }
        template <pass_parameters T>
        void operator()(T const& nested) noexcept {
            nested.visit_resources(*this);
        }
        template <typename T>
        void operator()(std::optional<T> const& field) noexcept {
            if (field)
                (*this)(*field);
        }
        template <typename T, std::size_t N>
        void operator()(std::array<T, N> const& fields) noexcept {
            for (auto const& field : fields)
                (*this)(field);
        }
        template <typename T, std::size_t N>
        void operator()(std::span<T, N> fields) noexcept {
            for (auto const& field : fields)
                (*this)(field);
        }
        template <typename First, typename Second, typename... Rest>
        void operator()(First const& first, Second const& second, Rest const&... rest) noexcept {
            (*this)(first);
            (*this)(second);
            ((*this)(rest), ...);
        }
    };
} // namespace deren::engine::render_graph
