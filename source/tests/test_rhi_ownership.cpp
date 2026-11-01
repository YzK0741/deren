#include "../backends/vulkan/core/lazy_loading.h"
#include "vk_test.h"

#include <atomic>
#include <barrier>
#include <concepts>
#include <latch>
#include <thread>
import deren.promise.rhi;
namespace rhi = deren::promise::rhi;
#include "lazy_loading_contract.hpp"
static_assert(requires(rhi::resource_factory& resources, rhi::resource_group_desc const& desc) {
    resources.create_resource_group(desc);
});
static_assert(requires(rhi::command_recorder& commands, rhi::resource_group_ref const& group) {
    commands.load_resource_group(group);
});
static_assert(rhi::validate_resource_group_layout({}) == rhi::error::ok);

static_assert(requires(rhi::command_recorder& commands, std::span<std::byte const> bytes) {
    { commands.push_constants(0u, bytes) } noexcept -> std::same_as<rhi::error>;
});
template <class T>
concept heap_push_entry = requires { &T::push_data; };
static_assert(!heap_push_entry<rhi::descriptor_heap>);
template <class T>
concept heap_push_limit = requires { &T::max_push_data; };
static_assert(!heap_push_limit<rhi::descriptor_heap_properties>);
static_assert(rhi::validate_push_constants_range(0, 4, 4) == rhi::error::ok);
static_assert(rhi::validate_push_constants_range(4, UINT64_MAX, 8) == rhi::error::invalid_argument);
template <class T>
concept reserved_query_factory = requires { &T::create_query; };
template <class T>
concept reserved_swapchain_factory = requires { &T::create_swapchain; };
static_assert(!reserved_query_factory<rhi::resource_factory>);
static_assert(!reserved_swapchain_factory<rhi::frame_context>);
static_assert(rhi::abi_version == 59u);
static_assert(!rhi::has_ability(rhi::all_abilities(), rhi::extension_kind::mesh_shader));
static_assert(rhi::all_extension_kinds().size() == 12);
namespace {
    static_assert(std::is_trivially_copyable_v<rhi::submission_token>);
    static_assert(sizeof(rhi::submission_token) == 2 * sizeof(std::uint64_t));
    static_assert(rhi::submission_token{}.device_id == 0 && rhi::submission_token{}.sequence == 0);
    static_assert(rhi::has_ability(rhi::all_abilities(), rhi::extension_kind::command_submission));
    template <class T>
    concept individually_submittable = requires(rhi::api_core& api, T& commands) { api.commands->submit(commands); };
    template <class T>
    concept root_factory = requires { &T::create_buffer; };
    static_assert(!root_factory<rhi::api_core>);
    static_assert(std::same_as<decltype(std::declval<rhi::api_core&>().resources.get()), rhi::resource_factory*>);
    static_assert(std::same_as<decltype(std::declval<rhi::api_core&>().commands.get()), rhi::command_context*>);
    static_assert(std::same_as<decltype(std::declval<rhi::api_core&>().frames.get()), rhi::frame_context*>);
    static_assert(sizeof(rhi::context_view<rhi::resource_factory>) == sizeof(void*));
    static_assert(!std::is_copy_constructible_v<rhi::context_view<rhi::resource_factory>>);
    static_assert(!std::is_copy_assignable_v<rhi::context_view<rhi::resource_factory>>);
    template <class T>
    concept frame_argument = requires(rhi::frame_context& frame, T& command) { frame.submit(command); };
    static_assert(!frame_argument<rhi::command_buffer>);
    static_assert(requires(rhi::frame_context& frame) { frame.submit(); });
    template <class T>
    concept merge_receiver = requires(T& commands, std::span<rhi::mergeable_command_buffer_ref const> batch) { commands.execute(batch); };
    static_assert(std::derived_from<rhi::command_buffer, rhi::command_recorder>);
    static_assert(std::derived_from<rhi::mergeable_command_buffer, rhi::command_recorder>);
    static_assert(!std::derived_from<rhi::mergeable_command_buffer, rhi::command_buffer>);
    static_assert(individually_submittable<rhi::command_buffer>);
    static_assert(!individually_submittable<rhi::command_recorder>);
    static_assert(!individually_submittable<rhi::mergeable_command_buffer>);
    static_assert(merge_receiver<rhi::command_buffer>);
    static_assert(!merge_receiver<rhi::command_recorder>);
    static_assert(!merge_receiver<rhi::mergeable_command_buffer>);
    static_assert(std::same_as<decltype(rhi::heap_bind_info::commands), rhi::command_recorder*>);
    struct counted final : rhi::object {
        std::atomic<unsigned>& destroyed;
        rhi::shared_object<counted>::weak self;
        explicit counted(std::atomic<unsigned>& count)
            : object("counted")
            , destroyed(count) {
        }
        void release() noexcept {
            ++destroyed;
            delete this;
        }
    };

    // Contexts may be ordinary derived-class members, constructed before binding in the body.
    struct resource_probe final : rhi::resource_factory {
        unsigned calls = 0;
        rhi::unique_object<rhi::buffer> create_buffer(rhi::buffer_desc const&) override {
            ++calls;
            return {};
        }
        rhi::unique_object<rhi::image> create_image(rhi::image_desc const&) override {
            return {};
        }
        rhi::unique_object<rhi::sampler> create_sampler(rhi::sampler_desc const&) override {
            return {};
        }
        rhi::unique_object<rhi::shader> create_shader(rhi::shader_desc const&) override {
            return {};
        }
        rhi::unique_object<rhi::pipeline> create_pipeline(rhi::pipeline_desc const&) override {
            return {};
        }
        rhi::unique_object<rhi::acceleration_structure> create_acceleration_structure(rhi::acceleration_structure_desc const&) override {
            return {};
        }
        rhi::unique_object<rhi::micromap> create_micromap(rhi::micromap_desc const&) override {
            return {};
        }
    };
    struct command_probe final : rhi::command_context {
        rhi::unique_object<rhi::command_buffer> create_command_buffer(rhi::command_buffer_desc const&) override {
            return {};
        }
        rhi::shared_object<rhi::command_buffer> make_command_buffer(rhi::command_buffer_desc const&) override {
            return {};
        }
        rhi::mergeable_command_buffer_ref make_mergeable_command_buffer(rhi::command_buffer_desc const&) override {
            return {};
        }
        rhi::error submit(rhi::command_buffer&) override {
            return rhi::error::unsupported;
        }
        rhi::error submit(std::span<rhi::mergeable_command_buffer_ref const>) override {
            return rhi::error::invalid_argument;
        }
    };
    struct frame_probe final : rhi::frame_context {
        unsigned calls = 0;
        rhi::command_buffer* begin_commands() override {
            return nullptr;
        }
        rhi::image* frame_image() noexcept override {
            return nullptr;
        }
        rhi::buffer* frame_readback_buffer() noexcept override {
            return nullptr;
        }
        rhi::submit_info frame_begin() override {
            return {};
        }
        rhi::error present() override {
            return rhi::error::unsupported;
        }
        rhi::frame_walker* walk_frames() noexcept override {
            return nullptr;
        }
        rhi::gpu_profiler* profiler() noexcept override {
            return nullptr;
        }
        rhi::swapchain* frame_swapchain() noexcept override {
            return nullptr;
        }
        rhi::error submit() override {
            ++calls;
            return rhi::error::not_ready;
        }
    };
    struct member_probe final : rhi::api_core {
        resource_probe factory;
        command_probe command;
        frame_probe frame;
        member_probe()
            : api_core("member_context_probe") {
            bind_views(factory, command, frame);
        }
        void release() noexcept override {
        }
        [[nodiscard]] rhi::stage_time_map get_time_profiling() const override {
            return {};
        }
        void record_stage_time(std::string_view, double) override {
        }
        void reset_time_profiling_window() override {
        }
        rhi::ability_bits abilities() const noexcept override {
            return {};
        }
        rhi::extension* query_extension(rhi::extension_kind) noexcept override {
            return nullptr;
        }
        void wait_idle() override {
        }
        std::uint32_t api_version() const noexcept override {
            return rhi::abi_version;
        }
    };
} // namespace
int main() {
    check_lazy_loading_contract();
    {
        member_probe first, second;
        CHECK(first.resources && first.commands && first.frames);
        CHECK(first.resources.get() == &first.factory && first.commands.get() == &first.command && first.frames.get() == &first.frame);
        CHECK(first.resources.get() != second.resources.get());
        CHECK(!first.resources->create_buffer({}));
        CHECK(first.factory.calls == 1 && second.factory.calls == 0);
        CHECK(first.frames->submit() == rhi::error::not_ready);
        CHECK(first.frame.calls == 1 && second.frame.calls == 0);
    }
    std::atomic<unsigned> destroyed{0};
    {
        rhi::shared_object<counted> owner{new counted(destroyed)};
        owner->self = owner;
        auto observer = owner->self;
        auto locked = observer.lock();
        CHECK(locked && owner.use_count() == 2);
        owner.reset();
        CHECK(!observer.expired() && destroyed == 0);
        locked.reset();
        CHECK(observer.expired() && !observer.lock() && destroyed == 1);
    }
    // Distinct handles on distinct threads: neither accesses the other's handle.
    // The last external weak may disappear concurrently with the last strong.
    constexpr unsigned iterations = 30000;
    std::barrier gate(2);
    rhi::shared_object<counted> owner;
    rhi::shared_object<counted>::weak observer;
    std::thread strong_thread([&] {
        for (unsigned i = 0; i < iterations; ++i) {
            owner = rhi::shared_object<counted>{new counted(destroyed)};
            observer = owner;
            gate.arrive_and_wait();
            owner.reset();
            gate.arrive_and_wait();
        }
    });
    std::thread weak_thread([&] {
        for (unsigned i = 0; i < iterations; ++i) {
            gate.arrive_and_wait();
            observer.reset();
            gate.arrive_and_wait();
        }
    });
    strong_thread.join();
    weak_thread.join();
    CHECK(destroyed == iterations + 1);
    return deren::vk_test::finish("test_rhi_ownership");
}
