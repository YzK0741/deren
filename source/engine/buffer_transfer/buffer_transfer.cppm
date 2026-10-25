module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <utility>
export module deren.engine.buffer_transfer;
export import deren.engine.resource_cluster;
import deren.promise.rhi;
import deren.utility.shared;

namespace deren::engine::transfer_detail {
    namespace rhi = deren::promise::rhi;
    namespace sync = rhi::synchronization;
    struct operation {
        resource_lease<rhi::buffer> resource, staging;
        std::uint64_t offset, size;
        sync::scope before, after;
        bool download;
    };
    struct submission {
        rhi::shared_object<rhi::api_core> core;
        std::shared_ptr<resource_cluster> allocations;
        deren::utility::vessel::vector<operation> operations;
        deren::utility::vessel::vector<resource_lease<rhi::buffer>> outputs;
        rhi::shared_object<rhi::command_buffer> commands;
        rhi::submission_token token{};
        bool completed = false;
        explicit submission(rhi::shared_object<rhi::api_core> device)
            : core(std::move(device)) {
        }
        rhi::error observe(bool wait, std::uint64_t timeout = 0) {
            if (completed)
                return rhi::error::ok;
            auto* completion = core ? rhi::query_extension<rhi::command_submission>(*core) : nullptr;
            if (!completion || token.sequence == 0)
                return rhi::error::not_ready;
            auto result = wait ? completion->wait(token, timeout) : completion->poll(token);
            if (result == rhi::error::ok) {
                completed = true;
                commands.reset();
                operations.clear();
                allocations.reset();
            }
            return result;
        }
    };
    inline rhi::scoped_buffer_barrier transition(operation const& op, bool after) {
        sync::scope transfer{sync::stage::transfer, op.download ? sync::access::read : sync::access::write};
        return {op.resource.get(), {after ? transfer : op.before, after ? op.after : transfer, op.offset, op.size}};
    }
} // namespace deren::engine::transfer_detail
export namespace deren::engine {
    namespace transfer_rhi = deren::promise::rhi;
    class buffer_transfer_service;
    /// Copyable completed-data owner. Poll/read never block; returned spans borrow this ticket.
    class buffer_transfer_ticket {
        friend class buffer_transfer_service;
        std::shared_ptr<transfer_detail::submission> value;
        explicit buffer_transfer_ticket(std::shared_ptr<transfer_detail::submission> state)
            : value(std::move(state)) {
        }

    public:
        buffer_transfer_ticket() = default;
        [[nodiscard]] transfer_rhi::submission_token receipt() const noexcept {
            return value ? value->token : transfer_rhi::submission_token{};
        }
        [[nodiscard]] transfer_rhi::error poll() {
            return value ? value->observe(false) : transfer_rhi::error::invalid_argument;
        }
        [[nodiscard]] transfer_rhi::error wait(std::uint64_t timeout_ns) {
            return value ? value->observe(true, timeout_ns) : transfer_rhi::error::invalid_argument;
        }
        [[nodiscard]] std::expected<std::span<std::byte const>, transfer_rhi::error> readback(std::size_t index) {
            namespace rhi = transfer_rhi;
            if (!value || index >= value->outputs.size())
                return std::unexpected(rhi::error::invalid_argument);
            if (auto result = poll(); result != rhi::error::ok)
                return std::unexpected(result);
            auto bytes = value->outputs[index]->mapped();
            if (bytes.size() != value->outputs[index]->size())
                return std::unexpected(rhi::error::operation_failed);
            return std::span<std::byte const>(bytes);
        }
    };
    /// Move-only descriptions. Accepted upload input is copied before returning; state scopes are explicit.
    class buffer_transfer_batch {
        friend class buffer_transfer_service;
        std::shared_ptr<transfer_detail::submission> value;
        explicit buffer_transfer_batch(transfer_rhi::shared_object<transfer_rhi::api_core> core)
            : value(std::allocate_shared<transfer_detail::submission>(deren::utility::memory::allocator<transfer_detail::submission>{}, std::move(core))) {
        }
        std::expected<std::size_t, transfer_rhi::error> append(resource_lease<transfer_rhi::buffer> resource,
                                                               std::uint64_t offset, std::uint64_t size, std::span<std::byte const> bytes,
                                                               transfer_rhi::synchronization::scope before, transfer_rhi::synchronization::scope after, bool download) {
            namespace rhi = transfer_rhi;
            namespace sync = rhi::synchronization;
            if (!value || !value->core)
                return std::unexpected(rhi::error::not_ready);
            auto* scopes = rhi::query_extension<rhi::scoped_synchronization>(*value->core);
            if (!scopes || !rhi::query_extension<rhi::command_submission>(*value->core))
                return std::unexpected(rhi::error::unsupported);
            if (!resource || resource.device() != value->core.get() || size == 0 ||
                after.stages == sync::stage::none || after.accesses == sync::access::none)
                return std::unexpected(rhi::error::invalid_argument);
            rhi::scoped_buffer_info info{};
            if (auto result = scopes->describe(*resource.get(), info); result != rhi::error::ok)
                return std::unexpected(result);
            auto usage = download ? sync::usage::transfer_source : sync::usage::transfer_destination;
            if (offset > info.byte_size || size > info.byte_size - offset ||
                !(info.usage_mask & static_cast<std::uint32_t>(usage)))
                return std::unexpected(rhi::error::invalid_argument);
            transfer_detail::operation op{resource, {}, offset, size, before, after, download};
            rhi::scoped_buffer_barrier transitions[] = {transfer_detail::transition(op, false), transfer_detail::transition(op, true)};
            if (auto result = scopes->validate({}, transitions); result != rhi::error::ok)
                return std::unexpected(result);
            if (!value->allocations)
                value->allocations = resource_cluster::create(value->core, "buffer transfers");
            auto allocation = value->allocations->create_buffer({.size = size, .usage = download ? rhi::buffer_usage::readback_coherent : rhi::buffer_usage::staging_source});
            if (!allocation)
                return std::unexpected(allocation.error());
            auto mapped = (*allocation)->mapped();
            if (mapped.size() != size)
                return std::unexpected(rhi::error::operation_failed);
            if (!download)
                std::memcpy(mapped.data(), bytes.data(), bytes.size());
            op.staging = *allocation;
            auto index = value->outputs.size();
            if (download)
                value->outputs.push_back(*allocation);
            value->operations.push_back(std::move(op));
            return index;
        }

    public:
        buffer_transfer_batch(buffer_transfer_batch const&) = delete;
        buffer_transfer_batch& operator=(buffer_transfer_batch const&) = delete;
        buffer_transfer_batch(buffer_transfer_batch&&) noexcept = default;
        buffer_transfer_batch& operator=(buffer_transfer_batch&&) noexcept = default;
        [[nodiscard]] transfer_rhi::error upload(resource_lease<transfer_rhi::buffer> destination, std::uint64_t offset,
                                                 std::span<std::byte const> bytes, transfer_rhi::synchronization::scope before, transfer_rhi::synchronization::scope after) {
            auto result = append(std::move(destination), offset, bytes.size(), bytes, before, after, false);
            return result ? transfer_rhi::error::ok : result.error();
        }
        [[nodiscard]] std::expected<std::size_t, transfer_rhi::error> readback(resource_lease<transfer_rhi::buffer> source,
                                                                               std::uint64_t offset, std::uint64_t size, transfer_rhi::synchronization::scope before, transfer_rhi::synchronization::scope after) {
            return append(std::move(source), offset, size, {}, before, after, true);
        }
    };
    /// Caller-serialized service. Owns pending dependencies even after ticket destruction.
    /// Destruction waits for this service's tokens only; a failed final drain is fatal.
    class buffer_transfer_service {
        transfer_rhi::shared_object<transfer_rhi::api_core> core;
        deren::utility::vessel::vector<std::shared_ptr<transfer_detail::submission>> pending;

    public:
        explicit buffer_transfer_service(transfer_rhi::shared_object<transfer_rhi::api_core> device)
            : core(std::move(device)) {
        }
        buffer_transfer_service(buffer_transfer_service const&) = delete;
        buffer_transfer_service& operator=(buffer_transfer_service const&) = delete;
        ~buffer_transfer_service() noexcept {
            if (drain(UINT64_MAX) != transfer_rhi::error::ok)
                deren::utility::fast_panic("buffer transfer final drain failed");
        }
        [[nodiscard]] buffer_transfer_batch make_batch() const {
            return buffer_transfer_batch(core);
        }
        [[nodiscard]] std::size_t pending_count() const noexcept {
            return std::count_if(pending.begin(), pending.end(), [](auto const& state) { return !state->completed; });
        }
        [[nodiscard]] std::expected<buffer_transfer_ticket, transfer_rhi::error> submit(buffer_transfer_batch&& batch) {
            namespace rhi = transfer_rhi;
            if (!core || !batch.value)
                return std::unexpected(rhi::error::not_ready);
            if (batch.value->core.get() != core.get() || batch.value->operations.empty())
                return std::unexpected(rhi::error::invalid_argument);
            auto* completion = rhi::query_extension<rhi::command_submission>(*core);
            auto* scopes = rhi::query_extension<rhi::scoped_synchronization>(*core);
            if (!completion || !scopes)
                return std::unexpected(rhi::error::unsupported);
            auto state = std::exchange(batch.value, {});
            state->commands = core->commands->make_command_buffer({});
            if (!state->commands)
                return std::unexpected(rhi::error::operation_failed);
            auto& commands = *state->commands;
            auto result = commands.begin_recording({});
            if (result != rhi::error::ok)
                return std::unexpected(result);
            for (auto const& op : state->operations) {
                auto pre = transfer_detail::transition(op, false), post = transfer_detail::transition(op, true);
                result = scopes->record(commands, {}, std::span(&pre, 1));
                if (result == rhi::error::ok) {
                    result = op.download ? commands.copy_buffer(*op.staging.get(), *op.resource.get(), op.size, op.offset, 0)
                                         : commands.copy_buffer(*op.resource.get(), *op.staging.get(), op.size, 0, op.offset);
                }
                if (result == rhi::error::ok)
                    result = scopes->record(commands, {}, std::span(&post, 1));
                if (result == rhi::error::ok && op.download) {
                    rhi::buffer_barrier host{.resource = op.staging.get(), .from = rhi::buffer_use::transfer_destination, .to = rhi::buffer_use::host_read};
                    result = commands.barrier(rhi::barrier_group{.buffers = std::span(&host, 1)});
                }
                if (result != rhi::error::ok) {
                    static_cast<void>(commands.end_recording());
                    return std::unexpected(result);
                }
            }
            result = commands.end_recording();
            if (result != rhi::error::ok)
                return std::unexpected(result);
            // All service ownership is registered before native submit can succeed.
            pending.push_back(state);
            auto submitted = completion->submit(commands);
            if (!submitted) {
                pending.pop_back();
                return std::unexpected(submitted.error());
            }
            state->token = *submitted;
            return buffer_transfer_ticket(std::move(state));
        }
        [[nodiscard]] transfer_rhi::error collect() {
            for (auto it = pending.begin(); it != pending.end();) {
                auto result = (*it)->observe(false);
                if (result == transfer_rhi::error::ok)
                    it = pending.erase(it);
                else if (result == transfer_rhi::error::not_ready)
                    ++it;
                else
                    return result;
            }
            return transfer_rhi::error::ok;
        }
        /// timeout_ns applies to each pending token; errors/timeouts retain uncompleted records.
        [[nodiscard]] transfer_rhi::error drain(std::uint64_t timeout_ns = UINT64_MAX) {
            for (auto const& state : pending) {
                auto result = state->observe(true, timeout_ns);
                if (result != transfer_rhi::error::ok)
                    return result;
            }
            pending.clear();
            return transfer_rhi::error::ok;
        }
    };
} // namespace deren::engine
