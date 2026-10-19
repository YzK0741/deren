#include "vk_test.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.h>
import deren.utility.shared;
import deren.promise.rhi;
import deren.engine.backend_loader;
// abi 40: the native handles now come from the contract's opaque main context, cast through the shared
// Vulkan layout (the same path the GUI plugin uses).
import deren.vulkan.main_context;
namespace rhi = deren::promise::rhi;

namespace {
    /// abi 40: a TEST gets a native device/queue the same way the GUI plugin does - the contract's opaque
    /// `main_context` cast to the Vulkan layout the backend and GUI plugin share. nullptr when the context is
    /// absent or is not this backend's (the tests below use a real backend, so it never is).
    [[nodiscard]] deren::vulkan::main_context const* vulkan_main(rhi::api_core& core) noexcept {
        rhi::main_context* const context = core.main_context();
        if (context == nullptr || !deren::vulkan::is_main_context(*context)) {
            return nullptr;
        }
        return rhi::do_concrete<deren::vulkan::main_context>(*context, deren::vulkan::main_context_name);
    }
} // namespace

struct queue_gate {
    rhi::api_core& core;
    VkDevice device;
    VkQueue queue = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    bool armed = false;
    queue_gate(rhi::api_core& context, VkDevice const device_, VkQueue const queue)
        : core(context)
        , device(device_)
        , queue(queue) {
        VkSemaphoreTypeCreateInfo type{};
        type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo create{};
        create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        create.pNext = &type;
        CHECK(vkCreateSemaphore(device, &create, nullptr, &semaphore) == VK_SUCCESS);
        if (!semaphore)
            return;
        std::uint64_t value = 1;
        VkTimelineSemaphoreSubmitInfo timeline{};
        timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        timeline.waitSemaphoreValueCount = 1;
        timeline.pWaitSemaphoreValues = &value;
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        wait.pNext = &timeline;
        wait.waitSemaphoreCount = 1;
        wait.pWaitSemaphores = &semaphore;
        wait.pWaitDstStageMask = &stage;
        armed = vkQueueSubmit(queue, 1, &wait, VK_NULL_HANDLE) == VK_SUCCESS;
        CHECK(armed);
    }
    void release() {
        if (!armed)
            return;
        VkSemaphoreSignalInfo signal{};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
        signal.semaphore = semaphore;
        signal.value = 1;
        CHECK(vkSignalSemaphore(device, &signal) == VK_SUCCESS);
        core.wait_idle();
        armed = false;
    }
    ~queue_gate() {
        release();
        if (semaphore)
            vkDestroySemaphore(device, semaphore, nullptr);
    }
};

void test_heap_sampling(rhi::api_core& core, deren::vulkan::main_context const& main_context) {
    std::ifstream input(VR_TEXTURE_GROUP_SHADER_PATH, std::ios::binary | std::ios::ate);
    CHECK(input.good());
    if (!input.good())
        return;
    deren::utility::vessel::vector<std::byte> code(static_cast<std::size_t>(input.tellg()));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(code.size()));
    rhi::pipeline_desc desc{};
    desc.first_stage = rhi::shader_stage::compute;
    desc.compute_code = code;
    desc.texture_group = {.enabled = true, .push_byte_offset = 8, .shader_stage_bits = 4};
    rhi::object_ptr<rhi::pipeline> pipeline{core.create_pipeline(desc)};
    CHECK(pipeline);
    auto wrong = desc;
    wrong.texture_group.push_byte_offset = 0; // destination is uint64, not the group token
    CHECK(!core.create_pipeline(wrong));
    if (!pipeline)
        return;
    auto make_image = [&core](std::uint32_t pixel) {
        rhi::image_desc info{};
        info.extent = {.width = 1, .height = 1, .depth = 1};
        info.format = rhi::image_format::rgba8_unorm;
        info.flags = rhi::to_bits(rhi::image_flag::sampled);
        info.initial_bytes = std::as_bytes(std::span(&pixel, 1));
        return rhi::object_ptr<rhi::image>(core.create_image(info));
    };
    auto red = make_image(0xff0000ffu), blue = make_image(0xffff0000u), green = make_image(0xff00ff00u);
    CHECK(red && blue && green);
    if (!red || !blue || !green)
        return;
    rhi::texture_group_info a{};
    a.textures[0] = red.get();
    a.textures[15] = blue.get();
    rhi::texture_group_info b{};
    b.textures[0] = green.get();
    b.textures[15] = green.get();
    auto group_a = core.assign_texture_group(a), group_b = core.assign_texture_group(b);
    CHECK(group_a && group_b);
    if (!group_a || !group_b)
        return;
    rhi::image* const red_image = red.get();
    red.reset();
    blue.reset();
    green.reset();
    rhi::object_ptr<rhi::buffer> output{core.create_buffer({.size = 60, .usage = rhi::buffer_usage::storage_coherent, .flags = rhi::to_bits(rhi::buffer_flag::device_address)})};
    auto commands = core.make_command_buffer({});
    auto* heap = rhi::query_extension<rhi::descriptor_heap>(core);
    auto* addresses = rhi::query_extension<rhi::device_address>(core);
    CHECK(output && commands && heap && addresses);
    // ---- THE DESCRIPTOR SPACE (new ability, no abi bump): positions + the portable write -----------
    // On the REAL backend: a registered group answers a stable position, the portable write publishes a
    // sampled descriptor through it, an unsupported view shape is refused by name, and the position of a
    // released group is stale (invalid_argument) - the generation is what tells the two states apart.
    auto* const space = rhi::query_extension<rhi::descriptor_space>(core);
    CHECK(space != nullptr);
    // abi 34 retired the native-vocabulary transition: a heap image write that still chains a structure is
    // refused (`unsupported`) instead of having the chain walked.
    if (heap != nullptr) {
        rhi::heap_image_write_info chained{};
        rhi::structure_header extra{};
        chained.header.next = &extra;
        CHECK(heap->write_image(chained) == rhi::error::unsupported);
    }
    rhi::descriptor_position position_a{};
    if (space != nullptr) {
        position_a = space->position(*group_a);
        CHECK(position_a.space == 0u);
        CHECK(position_a.base != 0u);
        CHECK(position_a.count == rhi::texture_group_capacity);
        CHECK(position_a.generation != 0u);
        CHECK(space->position(*group_a).base == position_a.base);
        rhi::descriptor_write write{};
        write.where = position_a;
        write.index = 0u;
        write.resource = red_image;
        CHECK(space->write(write) == rhi::error::ok);
        rhi::image_view_desc narrower{};
        narrower.mip_count = 1; // the group profile is all-mips; a narrower view is not this write
        rhi::descriptor_write wrong_view = write;
        wrong_view.view = &narrower;
        CHECK(space->write(wrong_view) == rhi::error::unsupported);
        rhi::descriptor_write out_of_range = write;
        out_of_range.index = rhi::texture_group_capacity;
        CHECK(space->write(out_of_range) == rhi::error::invalid_argument);
    }
    if (!output || !commands || !heap || !addresses)
        return;
    std::memset(output->mapped().data(), 0, 60);
    CHECK(commands->load_texture_group(group_a) == rhi::error::not_ready);
    CHECK(commands->begin_recording({}) == rhi::error::ok);
    CHECK(commands->load_texture_group(group_a) == rhi::error::unsupported);
    CHECK(commands->bind_pipeline(*pipeline) == rhi::error::ok);
    struct parameters {
        std::uint64_t destination;
        std::uint32_t token;
        std::uint32_t index;
    };
    parameters data{addresses->buffer_address(*output, 0), 0, 0};
    CHECK(heap->push_data({.commands = commands.get(), .data = std::as_bytes(std::span(&data.destination, 1))}) == rhi::error::ok);
    CHECK(heap->push_data({.commands = commands.get(), .offset = 8, .data = std::as_bytes(std::span(&data.token, 1))}) == rhi::error::invalid_argument);
    for (std::uint32_t i = 0; i < 5; ++i) {
        data.index = i;
        CHECK(heap->push_data({.commands = commands.get(), .offset = 12, .data = std::as_bytes(std::span(&data.index, 1))}) == rhi::error::ok);
        if (i == 0 || i == 4)
            CHECK(commands->load_texture_group(group_a) == rhi::error::ok);
        if (i == 0) {
            struct foreign_group : rhi::texture_group {
                foreign_group()
                    : texture_group("deren_texture_group_vulkan") {
                }
                void release() noexcept override {
                }
            } foreign;
            rhi::texture_group_ref wrong(&foreign, [](rhi::texture_group*) {});
            CHECK(commands->load_texture_group(wrong) == rhi::error::invalid_argument);
        }
        if (i == 1)
            CHECK(commands->load_texture_group(group_b) == rhi::error::ok);
        if (i == 2)
            CHECK(commands->load_texture_group({}) == rhi::error::ok);
        if (i == 3)
            CHECK(commands->bind_pipeline(*pipeline) == rhi::error::ok);
        commands->dispatch(1, 1, 1);
    }
    auto native = static_cast<VkCommandBuffer>(main_context.native_command_buffer(*main_context.core, *commands));
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(native, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    CHECK(commands->end_recording() == rhi::error::ok);
    auto submitted = commands;
    auto extra_image = make_image(0xff00ffffu);
    rhi::texture_group_info extra_info{};
    extra_info.textures[0] = extra_image.get();
    std::weak_ptr<rhi::texture_group> lifetime_a = group_a, lifetime_b = group_b;
    queue_gate gate(core, main_context.device, main_context.graphics_queue);
    CHECK(core.submit(*submitted) == rhi::error::ok);
    group_a.reset();
    group_b.reset();
    commands.reset();
    submitted.reset();
    CHECK(!lifetime_a.expired() && !lifetime_b.expired());
    auto new_group = core.assign_texture_group(extra_info);
    CHECK(new_group); // new records/descriptors must not overwrite the delayed groups
    gate.release();
    CHECK(lifetime_a.expired() && lifetime_b.expired());
    if (space != nullptr) {
        // THE STALE POSITION: the token is gone (or reused), so the write is refused by name. The image
        // pointer is only compared, never dereferenced: the group that owned it was released just above.
        rhi::descriptor_write stale{};
        stale.where = position_a;
        stale.index = 0u;
        stale.resource = red_image;
        CHECK(space->write(stale) == rhi::error::invalid_argument);
    }
    deren::utility::vessel::array<std::uint32_t, 15> actual{};
    std::memcpy(actual.data(), output->mapped().data(), 60);
    deren::utility::vessel::array<std::uint32_t, 15> const expected{0x8001, 0xff, 0xff0000, 0x8001, 0xff00, 0xff00,
                                                                    0, 0, 0, 0, 0, 0, 0x8001, 0xff, 0xff0000};
    CHECK(actual == expected);
    if (actual != expected)
        for (std::size_t i = 0; i < actual.size(); ++i)
            deren::vk_test::write_line("heap output[{}] = {:#x}, expected {:#x}", i, actual[i], expected[i]);

    // ---- ABI 35: BATCH SUBMISSION of two mergeable command buffers (real backend) --------------------
    // Two independent segments copy one value each into two halves of a host-visible buffer; ONE batch
    // submission executes both, and the mapped read-back shows both writes - the minimal observable
    // difference between "the batch ran" and "the handles were accepted".
    rhi::object_ptr<rhi::buffer> batch_output{core.create_buffer({.size = 8u,
                                                                  .usage = rhi::buffer_usage::storage_coherent,
                                                                  .flags = rhi::to_bits(rhi::buffer_flag::device_address)})};
    std::uint32_t const first_value = 0x11111111u, second_value = 0x22222222u;
    rhi::object_ptr<rhi::buffer> first_source{core.create_buffer({.size = 4u,
                                                                  .usage = rhi::buffer_usage::storage_coherent,
                                                                  .flags = rhi::to_bits(rhi::buffer_flag::device_address),
                                                                  .initial_bytes = std::as_bytes(std::span(&first_value, 1))})};
    rhi::object_ptr<rhi::buffer> second_source{core.create_buffer({.size = 4u,
                                                                   .usage = rhi::buffer_usage::storage_coherent,
                                                                   .flags = rhi::to_bits(rhi::buffer_flag::device_address),
                                                                   .initial_bytes = std::as_bytes(std::span(&second_value, 1))})};
    CHECK(batch_output && first_source && second_source);
    rhi::mergeable_command_buffer_ref mergeable_a = core.make_mergeable_command_buffer({});
    rhi::mergeable_command_buffer_ref mergeable_b = core.make_mergeable_command_buffer({});
    CHECK(mergeable_a && mergeable_b);
    if (batch_output && first_source && second_source && mergeable_a && mergeable_b) {
        CHECK(mergeable_a->begin_recording({}) == rhi::error::ok);
        CHECK(mergeable_a->copy_buffer(*batch_output, *first_source, 4u, 0u, 0u) == rhi::error::ok);
        CHECK(mergeable_a->end_recording() == rhi::error::ok);
        CHECK(mergeable_b->begin_recording({}) == rhi::error::ok);
        CHECK(mergeable_b->copy_buffer(*batch_output, *second_source, 4u, 0u, 4u) == rhi::error::ok);
        CHECK(mergeable_b->end_recording() == rhi::error::ok);
        rhi::mergeable_command_buffer_ref const batch[] = {mergeable_a, mergeable_b};
        CHECK(core.submit(std::span<rhi::mergeable_command_buffer_ref const>(batch)) == rhi::error::ok);
        core.wait_idle();
        deren::utility::vessel::array<std::uint32_t, 2> observed{};
        std::memcpy(observed.data(), batch_output->mapped().data(), sizeof(observed));
        CHECK(observed[0] == first_value);
        CHECK(observed[1] == second_value);

        // ---- ABI 36: THE RECORDING-TIME BATCH MERGE on the real backend --------------------------------
        // The same two segments, merged into an ORDINARY primary while recording (the shape the engine's
        // multithreaded passes use): one execute(span) call, then the normal single-buffer submit.
        rhi::object_ptr<rhi::buffer> merged_output{core.create_buffer({.size = 8u,
                                                                       .usage = rhi::buffer_usage::storage_coherent,
                                                                       .flags = rhi::to_bits(rhi::buffer_flag::device_address)})};
        CHECK(merged_output);
        if (merged_output) {
            CHECK(mergeable_a->begin_recording({}) == rhi::error::ok);
            CHECK(mergeable_a->copy_buffer(*merged_output, *first_source, 4u, 0u, 0u) == rhi::error::ok);
            CHECK(mergeable_a->end_recording() == rhi::error::ok);
            CHECK(mergeable_b->begin_recording({}) == rhi::error::ok);
            CHECK(mergeable_b->copy_buffer(*merged_output, *second_source, 4u, 0u, 4u) == rhi::error::ok);
            CHECK(mergeable_b->end_recording() == rhi::error::ok);
            auto recording_target = core.make_command_buffer({.kind = rhi::command_buffer_kind::primary});
            CHECK(recording_target != nullptr);
            if (recording_target) {
                CHECK(recording_target->begin_recording({}) == rhi::error::ok);
                rhi::mergeable_command_buffer_ref const merged[] = {mergeable_a, mergeable_b};
                CHECK(recording_target->execute(std::span<rhi::mergeable_command_buffer_ref const>(merged)) == rhi::error::ok);
                CHECK(recording_target->end_recording() == rhi::error::ok);
                CHECK(core.submit(*recording_target) == rhi::error::ok);
                core.wait_idle();
                deren::utility::vessel::array<std::uint32_t, 2> merged_values{};
                std::memcpy(merged_values.data(), merged_output->mapped().data(), sizeof(merged_values));
                CHECK(merged_values[0] == first_value);
                CHECK(merged_values[1] == second_value);
            }
        }
    }
}

int main(int argc, char** argv) {
    bool with_device = false;
    for (int i = 1; i < argc; ++i)
        with_device |= std::string_view(argv[i]) == "--with-device";
    if (!with_device)
        return deren::vk_test::finish("test_texture_group (device skipped)");
    rhi::create_info creation{};
    creation.window_width = 64;
    creation.window_height = 64;
    creation.window_visible = false;
    creation.validation_layers = true;
    auto core = deren::engine::load_api_core(creation);
    CHECK(core);
    if (!core)
        return deren::vk_test::finish("test_texture_group");
    deren::vulkan::main_context const* const main_context = vulkan_main(*core);
    CHECK(main_context != nullptr);
    if (main_context == nullptr || main_context->native_command_buffer == nullptr)
        return deren::vk_test::finish("test_texture_group");
    VkDevice const device = main_context->device;
    VkQueue const queue = main_context->graphics_queue;
    test_heap_sampling(*core, *main_context);
    rhi::object_ptr<rhi::buffer> output{core->create_buffer({.size = 4, .usage = rhi::buffer_usage::readback_coherent})};
    auto primary = core->make_command_buffer({});
    CHECK(output && primary);
    if (!output || !primary)
        return deren::vk_test::finish("test_texture_group");
    // abi 42: the fill travels through the CONTRACT (a copy from a host-initialised buffer), because the
    // escape no longer lends a native buffer handle - only the recording target a third-party renderer needs.
    std::array<std::byte, 4> const source_value{std::byte{0x44}, std::byte{0x33}, std::byte{0x22}, std::byte{0x11}};
    rhi::object_ptr<rhi::buffer> source{core->create_buffer({.size = 4,
                                                             .usage = rhi::buffer_usage::storage_coherent,
                                                             .initial_bytes = std::span<std::byte const>(source_value.data(), source_value.size())})};
    CHECK(source);
    if (!source)
        return deren::vk_test::finish("test_texture_group");
    std::memset(output->mapped().data(), 0, 4);
    CHECK(primary->begin_recording({}) == rhi::error::ok);
    CHECK(primary->copy_buffer(*output, *source, 4u, 0u, 0u) == rhi::error::ok);
    CHECK(primary->end_recording() == rhi::error::ok);

    VkSemaphoreTypeCreateInfo type{};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo create{};
    create.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    create.pNext = &type;
    VkSemaphore gate = VK_NULL_HANDLE;
    CHECK(vkCreateSemaphore(device, &create, nullptr, &gate) == VK_SUCCESS);
    if (gate == VK_NULL_HANDLE)
        return deren::vk_test::finish("test_texture_group");
    std::uint64_t const value = 1;
    VkTimelineSemaphoreSubmitInfo timeline{};
    timeline.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
    timeline.waitSemaphoreValueCount = 1;
    timeline.pWaitSemaphoreValues = &value;
    VkPipelineStageFlags const stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    wait.pNext = &timeline;
    wait.waitSemaphoreCount = 1;
    wait.pWaitSemaphores = &gate;
    wait.pWaitDstStageMask = &stage;
    CHECK(vkQueueSubmit(queue, 1, &wait, VK_NULL_HANDLE) == VK_SUCCESS);
    CHECK(core->submit(*primary) == rhi::error::ok);
    CHECK(primary->begin_recording({}) == rhi::error::not_ready);
    CHECK(core->submit(*primary) == rhi::error::not_ready);
    primary.reset(); // its command pool must survive the caller until completion
    VkSemaphoreSignalInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    signal.semaphore = gate;
    signal.value = value;
    CHECK(vkSignalSemaphore(device, &signal) == VK_SUCCESS);
    core->wait_idle();
    std::uint32_t actual = 0;
    std::memcpy(&actual, output->mapped().data(), sizeof(actual));
    CHECK(actual == 0x11223344u);
    vkDestroySemaphore(device, gate, nullptr);
    auto recycled = core->make_command_buffer({});
    CHECK(recycled);
    if (recycled) {
        CHECK(recycled->begin_recording({}) == rhi::error::ok);
        CHECK(recycled->end_recording() == rhi::error::ok);
        CHECK(core->submit(*recycled) == rhi::error::ok);
        core->wait_idle();
        CHECK(recycled->begin_recording({}) == rhi::error::ok);
        CHECK(recycled->end_recording() == rhi::error::ok);
    }
    return deren::vk_test::finish("test_texture_group");
}
