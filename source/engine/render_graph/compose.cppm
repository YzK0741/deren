module;
#include <concepts>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
export module deren.engine.render_graph.compose;
export import deren.engine.render_graph.execute;
export namespace deren::engine::render_graph::execution {
    template <class T>
    concept ownable = std::is_object_v<T> && std::same_as<T, std::remove_cvref_t<T>> &&
                      std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>;
    namespace detail {
        template <class T>
        struct graph_append_result : std::false_type {};
        template <class T>
        struct graph_append_result<std::expected<T, graph_error>>
            : std::bool_constant<std::is_void_v<T> || ownable<T>> {};
    } // namespace detail
    template <class F>
    concept graph_appender = requires(F& fn, render_graph::builder& graph) {
        { std::invoke(fn, graph) } noexcept;
        requires detail::graph_append_result<decltype(std::invoke(fn, graph))>::value;
    };
    template <class F, class Parameters>
    concept pass_recorder = ownable<F> && ownable<Parameters> && pass_parameters<Parameters> &&
                            requires(F const& fn, Parameters const& parameters, rhi::command_buffer& commands, record_context const& context) {
                                { std::invoke(fn, parameters, commands, context) } noexcept -> std::same_as<rhi::error>;
                            };
    enum class build_error_code { graph,
                                  execution,
                                  wrong_graph_ref,
                                  duplicate_recorder,
                                  missing_recorder,
                                  parameter_type_mismatch,
                                  sealed_builder,
                                  invalid_argument };
    struct build_error {
        build_error_code code = build_error_code::invalid_argument;
        std::optional<graph_error> graph{};
        std::optional<execution_error> execution{};
        uint32_t pass = invalid_index, resource = invalid_index;
        deren::utility::vessel::string pass_name{}, resource_name{};
    };
    struct import_state {
        initial_contents contents = initial_contents::undefined;
        resource_state state{};
    };
    namespace detail {
        template <class P, class F>
        struct typed_recorder final : recorder {
            F function;
            explicit typed_recorder(F value) noexcept
                : function(std::move(value)) {
            }
            bool matches(parameter_snapshot const& snapshot) const noexcept override {
                return snapshot.template get<P>();
            }
            rhi::error record(rhi::command_buffer& commands, record_context const& context) const noexcept override {
                return std::invoke(function, *context.pass().parameters.template get<P>(), commands, context);
            }
        };
        struct barrier_recorder final : recorder {
            bool matches(parameter_snapshot const&) const noexcept override {
                return true;
            }
            rhi::error record(rhi::command_buffer&, record_context const&) const noexcept override {
                return rhi::error::ok;
            }
        };
    } // namespace detail
    class builder final {
        rhi::shared_object<rhi::api_core> core_;
        render_graph::builder graph_;
        bool sealed_ = false;
        std::optional<build_error> error_;
        deren::utility::vessel::vector<resource_binding> bindings_;
        deren::utility::vessel::vector<std::shared_ptr<detail::recorder const>> recorders_;
        deren::utility::vessel::vector<std::shared_ptr<void const>> retained_;
        build_error latch(build_error error) {
            if (!error_) {
                error_ = std::move(error);
            }
            return *error_;
        }
        build_error graph_error_of(graph_error error) {
            build_error result{};
            result.code = build_error_code::graph;
            result.pass = error.pass;
            result.resource = error.resource;
            result.pass_name = error.pass_name;
            result.resource_name = error.resource_name;
            result.graph = std::move(error);
            return latch(std::move(result));
        }
        build_error execution_error_of(execution_error error, std::string_view resource_name = {}) {
            build_error result{};
            result.code = build_error_code::execution;
            result.pass = error.pass;
            result.resource = error.resource;
            result.execution = error;
            result.resource_name = resource_name;
            return latch(std::move(result));
        }
        std::optional<build_error> blocked() const {
            if (error_) {
                return error_;
            }
            if (sealed_) {
                return build_error{.code = build_error_code::sealed_builder};
            }
            return {};
        }
        template <class T>
        auto import_resource(resource_lease<T> lease, import_state initial, std::string_view name) {
            using ref_type = std::conditional_t<std::is_same_v<T, rhi::image>, texture_ref, buffer_ref>;
            using result_type = std::expected<ref_type, build_error>;
            if (auto error = blocked()) {
                return result_type(std::unexpected(*error));
            }
            auto identity = identify(lease);
            if (!core_ || !identity.object || lease.device() != core_.get()) {
                return result_type(std::unexpected(execution_error_of({rhi::error::invalid_argument}, name)));
            }
            auto sync = rhi::query_extension<rhi::scoped_synchronization>(*core_);
            auto ref = [&]() -> std::expected<ref_type, graph_error> {
                if constexpr (std::is_same_v<T, rhi::image>) {
                    rhi::scoped_image_info info{};
                    if (sync->describe(*lease.get(), info) != rhi::error::ok) {
                        return std::unexpected(graph_error{.code = error_code::invalid_identity, .pass_name = {}, .resource_name = {}});
                    }
                    return graph_.import_texture(identity, initial.contents, name,
                                                 {info.mip_levels, info.array_layers, static_cast<texture_aspect>(info.aspects)}, initial.state);
                } else {
                    rhi::scoped_buffer_info info{};
                    if (sync->describe(*lease.get(), info) != rhi::error::ok) {
                        return std::unexpected(graph_error{.code = error_code::invalid_identity, .pass_name = {}, .resource_name = {}});
                    }
                    return graph_.import_buffer(identity, initial.contents, name, {info.byte_size}, initial.state);
                }
            }();
            if (!ref) {
                return result_type(std::unexpected(graph_error_of(ref.error())));
            }
            bool found = false;
            for (auto const& binding : bindings_) {
                if (std::visit([](auto const& value) { return identify(value); }, binding) == identity) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                bindings_.push_back(std::move(lease));
            }
            return result_type(*ref);
        }
        std::expected<void, build_error> check_binding(pass_ref ref) {
            if (auto error = blocked()) {
                return std::unexpected(*error);
            }
            if (!graph_.contains(ref)) {
                return std::unexpected(latch({.code = build_error_code::wrong_graph_ref, .pass = ref.index()}));
            }
            if (ref.index() < recorders_.size() && recorders_[ref.index()]) {
                return std::unexpected(latch({.code = build_error_code::duplicate_recorder, .pass = ref.index()}));
            }
            if (recorders_.size() <= ref.index()) {
                recorders_.resize(ref.index() + 1);
            }
            return {};
        }

    public:
        explicit builder(rhi::shared_object<rhi::api_core> core)
            : core_(std::move(core)) {
        }
        builder(builder const&) = delete;
        builder& operator=(builder const&) = delete;
        auto import_texture(resource_lease<rhi::image> lease, import_state state, std::string_view name) {
            return import_resource(std::move(lease), state, name);
        }
        auto import_buffer(resource_lease<rhi::buffer> lease, import_state state, std::string_view name) {
            return import_resource(std::move(lease), state, name);
        }
        template <class F>
            requires graph_appender<F>
        auto append(F&& function) {
            using result_type = std::expected<typename std::invoke_result_t<F&, render_graph::builder&>::value_type, build_error>;
            if (auto error = blocked()) {
                return result_type(std::unexpected(*error));
            }
            if constexpr (std::is_pointer_v<std::remove_cvref_t<F>> || std::is_member_pointer_v<std::remove_cvref_t<F>>) {
                if (function == nullptr) {
                    return result_type(std::unexpected(latch({.code = build_error_code::invalid_argument})));
                }
            }
            auto result = std::invoke(function, graph_);
            if (!result) {
                return result_type(std::unexpected(graph_error_of(result.error())));
            }
            if constexpr (std::is_void_v<typename decltype(result)::value_type>) {
                return result_type{};
            } else {
                return result_type(std::move(*result));
            }
        }
        std::expected<void, build_error> export_resource(texture_ref ref, texture_range range = {}) {
            if (auto error = blocked()) {
                return std::unexpected(*error);
            }
            auto result = graph_.export_resource(ref, range);
            if (!result) {
                return std::unexpected(graph_error_of(result.error()));
            }
            return {};
        }
        std::expected<void, build_error> export_resource(buffer_ref ref, buffer_range range = {}) {
            if (auto error = blocked()) {
                return std::unexpected(*error);
            }
            auto result = graph_.export_resource(ref, range);
            if (!result) {
                return std::unexpected(graph_error_of(result.error()));
            }
            return {};
        }
        std::expected<void, build_error> order_after(pass_ref after, pass_ref before) {
            if (auto error = blocked()) {
                return std::unexpected(*error);
            }
            auto result = graph_.order_after(after, before);
            if (!result) {
                return std::unexpected(graph_error_of(result.error()));
            }
            return {};
        }
        template <class P, class F>
            requires pass_recorder<std::remove_cvref_t<F>, P> && std::is_nothrow_constructible_v<std::remove_cvref_t<F>, F&&>
        std::expected<void, build_error> bind_pass(pass_ref ref, F&& fn) {
            auto valid = check_binding(ref);
            if (!valid) {
                return valid;
            }
            if constexpr (std::is_pointer_v<std::remove_cvref_t<F>> || std::is_member_pointer_v<std::remove_cvref_t<F>>) {
                if (fn == nullptr) {
                    return std::unexpected(latch({.code = build_error_code::invalid_argument, .pass = ref.index()}));
                }
            }
            using storage = detail::typed_recorder<P, std::remove_cvref_t<F>>;
            recorders_[ref.index()] = std::allocate_shared<storage>(deren::utility::memory::allocator<storage>{}, std::forward<F>(fn));
            return {};
        }
        std::expected<void, build_error> bind_barrier_only(pass_ref ref) {
            auto valid = check_binding(ref);
            if (!valid) {
                return valid;
            }
            recorders_[ref.index()] = std::allocate_shared<detail::barrier_recorder>(deren::utility::memory::allocator<detail::barrier_recorder>{});
            return {};
        }
        template <class T>
            requires std::convertible_to<std::shared_ptr<T>, std::shared_ptr<void const>>
        std::expected<void, build_error> retain(std::shared_ptr<T> owner) {
            if (auto error = blocked()) {
                return std::unexpected(*error);
            }
            if (!owner) {
                return std::unexpected(latch({.code = build_error_code::invalid_argument}));
            }
            retained_.push_back(std::move(owner));
            return {};
        }
        /// a SHARED RHI HANDLE is retained too (the post chain keeps the LUT view alive this way)
        template <typename T>
        std::expected<void, build_error> retain(deren::promise::rhi::shared_object<T> owner) {
            if (auto error = blocked()) {
                return std::unexpected(*error);
            }
            if (!owner) {
                return std::unexpected(latch({.code = build_error_code::invalid_argument}));
            }
            retained_.push_back(std::make_shared<deren::promise::rhi::shared_object<T>>(std::move(owner)));
            return {};
        }
        std::expected<prepared_graph, build_error> prepare() {
            if (auto error = blocked()) {
                return std::unexpected(*error);
            }
            sealed_ = true;
            auto plan = graph_.compile({true});
            if (!plan) {
                return std::unexpected(graph_error_of(plan.error()));
            }
            recorders_.resize(plan->passes.size());
            for (auto pass : plan->execution_order) {
                auto const& action = recorders_[pass];
                auto code = !action ? build_error_code::missing_recorder : build_error_code::parameter_type_mismatch;
                if (!action || !action->matches(plan->passes[pass].parameters)) {
                    return std::unexpected(latch({.code = code, .pass = pass, .pass_name = plan->passes[pass].name}));
                }
            }
            for (uint32_t i = 0; i < plan->passes.size(); ++i) {
                if (!plan->passes[i].live) {
                    recorders_[i].reset();
                }
            }
            auto prepared = detail::composition_access::prepare(core_, std::move(*plan), bindings_, std::move(recorders_), std::move(retained_));
            if (!prepared) {
                return std::unexpected(execution_error_of(prepared.error()));
            }
            bindings_.clear();
            return std::move(*prepared);
        }
    };
} // namespace deren::engine::render_graph::execution
