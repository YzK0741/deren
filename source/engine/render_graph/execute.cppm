module;
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
export module deren.engine.render_graph.execute;
export import deren.engine.render_graph.rhi;
export import deren.engine.resource_cluster;
export import deren.promise.rhi;
export namespace deren::engine::render_graph::execution {
    namespace rhi = deren::promise::rhi;
    namespace sync = rhi::synchronization;
    using resource_binding = std::variant<resource_lease<rhi::image>, resource_lease<rhi::buffer>>;
    class record_context;
    template <typename T>
    resource_identity identify(resource_lease<T> const& value) noexcept {
        static_assert(std::is_same_v<T, rhi::image> || std::is_same_v<T, rhi::buffer>);
        if (!value || !value.device() || value.device()->api_version() != rhi::abi_version ||
            !rhi::has_ability(value.device()->abilities(), rhi::extension_kind::scoped_synchronization))
            return {};
        auto extension = rhi::query_extension<rhi::scoped_synchronization>(*value.device());
        if (!extension)
            return {};
        std::conditional_t<std::is_same_v<T, rhi::image>, rhi::scoped_image_info, rhi::scoped_buffer_info> info{};
        if (extension->describe(*value.get(), info) != rhi::error::ok || !info.canonical_object)
            return {};
        return {reinterpret_cast<uintptr_t>(value.device()), info.canonical_object};
    }
    struct execution_error {
        rhi::error code;
        uint32_t pass = invalid_index, resource = invalid_index;
    };
    namespace detail {
        struct composition_access;
        struct recorder {
            virtual ~recorder() = default;
            virtual bool matches(parameter_snapshot const&) const noexcept = 0;
            virtual rhi::error record(rhi::command_buffer&, record_context const&) const noexcept = 0;
        };
        struct batch {
            deren::utility::vessel::vector<rhi::scoped_image_barrier> images;
            deren::utility::vessel::vector<rhi::scoped_buffer_barrier> buffers;
        };
        struct state {
            std::shared_ptr<rhi::api_core> core;
            compiled_graph graph;
            deren::utility::vessel::vector<resource_binding> bindings;
            deren::utility::vessel::vector<batch> batches;
            rhi::scoped_synchronization* extension = nullptr;
            bool bound_actions = false;
            deren::utility::vessel::vector<std::shared_ptr<recorder const>> recorders;
            deren::utility::vessel::vector<std::shared_ptr<void const>> retained;
        };
        constexpr uint32_t usage_bit(resource_usage usage) noexcept {
            auto value = static_cast<uint32_t>(usage);
            return value >= 1 && value <= 12 ? 1U << (value - 1) : 0;
        }
    } // namespace detail
    /// Borrowed only for the callback invocation; copying does not retain resources.
    class record_context {
        detail::state const& state_;
        uint32_t pass_;
        friend class prepared_graph;
        record_context(detail::state const& state, uint32_t pass) noexcept
            : state_(state)
            , pass_(pass) {
        }
        bool declared(uint32_t index) const noexcept {
            for (auto const& access : pass().accesses)
                if (access.resource == index)
                    return true;
            return false;
        }

    public:
        uint32_t pass_index() const noexcept {
            return pass_;
        }
        pass_record const& pass() const noexcept {
            return state_.graph.passes[pass_];
        }
        rhi::image* image(uint32_t index) const noexcept {
            if (index >= state_.bindings.size() || !declared(index))
                return nullptr;
            auto lease = std::get_if<resource_lease<rhi::image>>(&state_.bindings[index]);
            return lease ? lease->get() : nullptr;
        }
        rhi::buffer* buffer(uint32_t index) const noexcept {
            if (index >= state_.bindings.size() || !declared(index))
                return nullptr;
            auto lease = std::get_if<resource_lease<rhi::buffer>>(&state_.bindings[index]);
            return lease ? lease->get() : nullptr;
        }
    };
    class prepared_graph {
        friend struct detail::composition_access;
        std::shared_ptr<detail::state> state_;

    public:
        prepared_graph() noexcept = default;
        static std::expected<prepared_graph, execution_error> prepare(
            std::shared_ptr<rhi::api_core> core, compiled_graph graph, std::span<resource_binding const> bindings) {
            auto fail = [](rhi::error code, uint32_t pass = invalid_index, uint32_t resource = invalid_index) {
                return std::unexpected(execution_error{code, pass, resource});
            };
            if (!core || core->api_version() != rhi::abi_version)
                return fail(rhi::error::invalid_argument);
            if (!rhi::has_ability(core->abilities(), rhi::extension_kind::scoped_synchronization))
                return fail(rhi::error::unsupported);
            auto base = core->query_extension(rhi::extension_kind::scoped_synchronization);
            if (!base || base->kind() != rhi::extension_kind::scoped_synchronization)
                return fail(rhi::error::unsupported);
            auto extension = static_cast<rhi::scoped_synchronization*>(base);
            auto lowered = rhi_bridge::lower_scopes(graph);
            if (!lowered)
                return fail(rhi::error::invalid_argument, invalid_index, lowered.error().resource);
            auto state = std::allocate_shared<detail::state>(deren::utility::memory::allocator<detail::state>{});
            state->core = std::move(core);
            state->extension = extension;
            state->graph = std::move(graph);
            auto const& plan = state->graph;
            state->bindings.resize(plan.resources.size());
            state->batches.resize(plan.passes.size());
            deren::utility::vessel::vector<bool> needed(plan.resources.size(), false), found(plan.resources.size(), false);
            for (auto pass : plan.execution_order)
                for (auto const& access : plan.passes[pass].accesses) {
                    if (access.resource >= needed.size())
                        return fail(rhi::error::invalid_argument, pass, access.resource);
                    needed[access.resource] = true;
                }
            for (auto const& output : plan.outputs) {
                if (output.resource >= needed.size())
                    return fail(rhi::error::invalid_argument);
                needed[output.resource] = true;
            }
            for (auto const& packet : lowered->barriers)
                needed[packet.resource] = true;
            for (auto const& binding : bindings) {
                auto identity = std::visit([](auto const& lease) { return identify(lease); }, binding);
                if (!identity.object || identity.device != reinterpret_cast<uintptr_t>(state->core.get()))
                    return fail(rhi::error::invalid_argument);
                bool matched = false;
                for (uint32_t i = 0; i < plan.resources.size(); ++i) {
                    if (plan.resources[i].identity != identity)
                        continue;
                    matched = true;
                    if (found[i] || (plan.resources[i].kind == resource_kind::texture) != (binding.index() == 0))
                        return fail(rhi::error::invalid_argument, invalid_index, i);
                    found[i] = true;
                    if (needed[i])
                        state->bindings[i] = binding;
                }
                if (!matched)
                    return fail(rhi::error::invalid_argument);
            }
            deren::utility::vessel::vector<uint32_t> usages(plan.resources.size());
            for (uint32_t i = 0; i < plan.resources.size(); ++i) {
                if (!needed[i])
                    continue;
                if (!found[i])
                    return fail(rhi::error::invalid_argument, invalid_index, i);
                auto const& resource = plan.resources[i];
                if (resource.kind == resource_kind::texture) {
                    rhi::scoped_image_info info{};
                    auto error = extension->describe(*std::get<0>(state->bindings[i]).get(), info);
                    if (error != rhi::error::ok)
                        return fail(error, invalid_index, i);
                    auto shape = std::get_if<texture_shape>(&resource.shape);
                    if (!shape || shape->mip_levels != info.mip_levels || shape->array_layers != info.array_layers || static_cast<uint32_t>(shape->aspects) != static_cast<uint32_t>(info.aspects))
                        return fail(rhi::error::invalid_argument, invalid_index, i);
                    usages[i] = info.usage_mask;
                } else {
                    rhi::scoped_buffer_info info{};
                    auto error = extension->describe(*std::get<1>(state->bindings[i]).get(), info);
                    if (error != rhi::error::ok)
                        return fail(error, invalid_index, i);
                    auto shape = std::get_if<buffer_shape>(&resource.shape);
                    if (!shape || shape->byte_size != info.byte_size)
                        return fail(rhi::error::invalid_argument, invalid_index, i);
                    usages[i] = info.usage_mask;
                }
            }
            auto stages = static_cast<uint32_t>(extension->supported_stages());
            for (auto pass : plan.execution_order)
                for (auto const& access : plan.passes[pass].accesses) {
                    auto usage = detail::usage_bit(access.scope.usage);
                    if (!usage || !(usages[access.resource] & usage) || (static_cast<uint32_t>(access.scope.stages) & ~stages))
                        return fail(rhi::error::unsupported, pass, access.resource);
                }
            for (auto const& packet : lowered->barriers) {
                auto& batch = state->batches[packet.before_pass];
                if (auto image = std::get_if<sync::image_transition>(&packet.transition))
                    batch.images.push_back({std::get<0>(state->bindings[packet.resource]).get(), *image});
                else
                    batch.buffers.push_back({std::get<1>(state->bindings[packet.resource]).get(), std::get<sync::buffer_transition>(packet.transition)});
            }
            for (auto pass : plan.execution_order) {
                auto const& batch = state->batches[pass];
                auto error = extension->validate(batch.images, batch.buffers);
                if (error != rhi::error::ok)
                    return fail(error, pass);
            }
            prepared_graph result;
            result.state_ = std::move(state);
            return result;
        }

    private:
        static std::expected<prepared_graph, execution_error> prepare_bound(
            std::shared_ptr<rhi::api_core> core, compiled_graph graph, std::span<resource_binding const> bindings,
            deren::utility::vessel::vector<std::shared_ptr<detail::recorder const>> recorders,
            deren::utility::vessel::vector<std::shared_ptr<void const>> retained) {
            for (auto pass : graph.execution_order)
                if (pass >= graph.passes.size() || pass >= recorders.size() || !recorders[pass] || !recorders[pass]->matches(graph.passes[pass].parameters))
                    return std::unexpected(execution_error{rhi::error::invalid_argument, pass});
            auto result = prepare(std::move(core), std::move(graph), bindings);
            if (!result)
                return result;
            result->state_->bound_actions = true;
            result->state_->recorders = std::move(recorders);
            result->state_->retained = std::move(retained);
            return result;
        }

        template <typename F>
            requires std::is_nothrow_invocable_r_v<rhi::error, F const&, record_context const&>
        rhi::error record_impl(rhi::command_buffer& commands, F const& callback) const noexcept {
            auto state = state_;
            if (!state)
                return rhi::error::invalid_argument;
            // Frame snapshots may outlive their acquire but must never record against a later one.
            // Check even resources with no barrier, before emitting any command from this plan.
            for (std::size_t i = 0; i < state->bindings.size(); ++i) {
                auto identity = std::visit([](auto const& lease) { return identify(lease); }, state->bindings[i]);
                if (identity.object && identity != state->graph.resources[i].identity)
                    return rhi::error::invalid_argument;
                bool bound = std::visit([](auto const& lease) { return static_cast<bool>(lease); }, state->bindings[i]);
                if (bound && !identity.object)
                    return rhi::error::not_ready;
            }
            if (state->graph.execution_order.empty())
                return state->extension->record(commands, {}, {});
            for (auto pass : state->graph.execution_order) {
                for (auto const& access : state->graph.passes[pass].accesses) {
                    auto identity = std::visit([](auto const& lease) { return identify(lease); }, state->bindings[access.resource]);
                    if (identity != state->graph.resources[access.resource].identity)
                        return rhi::error::not_ready;
                }
                auto const& batch = state->batches[pass];
                auto error = state->extension->record(commands, batch.images, batch.buffers);
                if (error != rhi::error::ok)
                    return error;
                error = callback(record_context{*state, pass});
                if (error != rhi::error::ok)
                    return error;
            }
            return rhi::error::ok;
        }

    public:
        template <typename F>
            requires std::is_nothrow_invocable_r_v<rhi::error, F const&, record_context const&>
        rhi::error record(rhi::command_buffer& commands, F const& callback) const noexcept {
            if (!state_ || state_->bound_actions)
                return rhi::error::invalid_argument;
            return record_impl(commands, callback);
        }
        rhi::error record(rhi::command_buffer& commands) const noexcept {
            if (!state_ || !state_->bound_actions)
                return rhi::error::invalid_argument;
            auto state = state_;
            return record_impl(commands, [&](record_context const& context) noexcept {
                return state->recorders[context.pass_index()]->record(commands, context);
            });
        }
        std::shared_ptr<void const> retention() const noexcept {
            return state_;
        }
    };
    namespace detail {
        // Internal bridge: the public prepared_graph API does not expose erased actions.
        struct composition_access {
            static std::expected<prepared_graph, execution_error> prepare(
                std::shared_ptr<rhi::api_core> core, compiled_graph graph, std::span<resource_binding const> bindings,
                deren::utility::vessel::vector<std::shared_ptr<recorder const>> recorders,
                deren::utility::vessel::vector<std::shared_ptr<void const>> retained) {
                return prepared_graph::prepare_bound(std::move(core), std::move(graph), bindings, std::move(recorders), std::move(retained));
            }
        };
    } // namespace detail
} // namespace deren::engine::render_graph::execution
