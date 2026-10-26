// Opt-in device measurement. Timed paths mirror current VMA host upload and a staging alternative.
#include "texture_upload_layout.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string_view>
#include <vma/vk_mem_alloc.h>
import deren.utility.shared;
import deren.promise.rhi;
import deren.engine.backend_loader;
import deren.vulkan.main_context;
namespace rhi = deren::promise::rhi;
namespace vessel = deren::utility::vessel;
using timer = std::chrono::steady_clock;

namespace {
    void require(bool value, char const* message) {
        if (!value)
            deren::utility::fast_panic(message);
    }
    void check(VkResult result, char const* operation) {
        require(result == VK_SUCCESS, operation);
    }
    double elapsed(timer::time_point begin) {
        return std::chrono::duration<double, std::micro>(timer::now() - begin).count();
    }
    struct shape {
        char const* name;
        VkFormat format;
        uint32_t side, layers, mips;
    };
    struct buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VmaAllocationInfo info{};
        VkDeviceSize capacity = 0;
    };
    struct times {
        double allocation = 0, prepare = 0, host_copy = 0, submit = 0, wait = 0, upload = 0, total = 0;
        unsigned submits = 0;
        VkDeviceSize image_bytes = 0, staging_bytes = 0;
        uint32_t memory_type = 0;
    };
    enum class route { host_current,
                       host_native,
                       staging_shared_usage,
                       staging };
    char const* name(route method) {
        switch (method) {
        case route::host_current:
            return "host_current";
        case route::host_native:
            return "host_native";
        case route::staging_shared_usage:
            return "staging_shared_usage";
        case route::staging:
            return "staging";
        }
        return "unknown";
    }
    struct context {
        deren::vulkan::main_context const& main;
        VmaAllocator allocator = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        PFN_vkCopyMemoryToImageEXT host_copy = nullptr;
        PFN_vkTransitionImageLayoutEXT host_transition = nullptr;
        buffer warm_staging, readback;
        explicit context(deren::vulkan::main_context const& value)
            : main(value) {
            VmaAllocatorCreateInfo allocation{};
            allocation.instance = main.instance;
            allocation.physicalDevice = main.physical_device;
            allocation.device = main.device;
            allocation.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
            check(vmaCreateAllocator(&allocation, &allocator), "benchmark VMA allocator");
            VkCommandPoolCreateInfo create{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            create.queueFamilyIndex = main.graphics_queue_family;
            check(vkCreateCommandPool(main.device, &create, nullptr, &pool), "benchmark command pool");
            VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            commands.commandPool = pool;
            commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            commands.commandBufferCount = 1;
            check(vkAllocateCommandBuffers(main.device, &commands, &command), "benchmark command buffer");
            VkFenceCreateInfo fences{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            check(vkCreateFence(main.device, &fences, nullptr, &fence), "benchmark fence");
            host_copy = reinterpret_cast<PFN_vkCopyMemoryToImageEXT>(vkGetDeviceProcAddr(main.device, "vkCopyMemoryToImageEXT"));
            require(host_copy != nullptr, "benchmark host copy entry point");
            host_transition = reinterpret_cast<PFN_vkTransitionImageLayoutEXT>(vkGetDeviceProcAddr(main.device, "vkTransitionImageLayoutEXT"));
            require(host_transition != nullptr, "benchmark host transition entry point");
        }
        void release(buffer& value) {
            if (value.handle)
                vmaDestroyBuffer(allocator, value.handle, value.allocation);
            value = {};
        }
        void ensure(buffer& value, VkDeviceSize size, bool download = false) {
            if (value.capacity >= size)
                return;
            release(value);
            VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            create.size = size;
            create.usage = download ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            VmaAllocationCreateInfo allocation{};
            allocation.usage = download ? VMA_MEMORY_USAGE_GPU_TO_CPU : VMA_MEMORY_USAGE_CPU_TO_GPU;
            allocation.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | (download ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);
            check(vmaCreateBuffer(allocator, &create, &allocation, &value.handle, &value.allocation, &value.info), "benchmark staging/readback buffer");
            require(value.info.pMappedData != nullptr, "benchmark buffer mapping");
            value.capacity = size;
        }
        void begin() {
            check(vkResetCommandBuffer(command, 0), "benchmark reset command buffer");
            check(vkResetFences(main.device, 1, &fence), "benchmark reset fence");
            VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkBeginCommandBuffer(command, &info), "benchmark begin command buffer");
        }
        void submit(times& measured) {
            check(vkEndCommandBuffer(command), "benchmark end command buffer");
            VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            info.commandBufferCount = 1;
            info.pCommandBuffers = &command;
            auto start = timer::now();
            // This standalone process never starts rendering; it is the sole queue user after setup.
            check(vkQueueSubmit(main.graphics_queue, 1, &info, fence), "benchmark submit");
            measured.submit += elapsed(start);
            start = timer::now();
            check(vkWaitForFences(main.device, 1, &fence, VK_TRUE, 5'000'000'000ull), "benchmark wait");
            measured.wait += elapsed(start);
            ++measured.submits;
        }
        void barrier(VkImage image, shape const& test, VkImageLayout old_layout, VkImageLayout new_layout,
                     VkAccessFlags src_access, VkAccessFlags dst_access, VkPipelineStageFlags src, VkPipelineStageFlags dst) {
            VkImageMemoryBarrier transition{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            transition.oldLayout = old_layout;
            transition.newLayout = new_layout;
            transition.srcAccessMask = src_access;
            transition.dstAccessMask = dst_access;
            transition.srcQueueFamilyIndex = transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            transition.image = image;
            transition.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, test.mips, 0, test.layers};
            vkCmdPipelineBarrier(command, src, dst, 0, 0, nullptr, 0, nullptr, 1, &transition);
        }
        bool supported(shape const& test, route method, VkBool32& optimal, VkBool32& identical) {
            VkPhysicalDeviceImageFormatInfo2 request{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
            request.format = test.format;
            request.type = VK_IMAGE_TYPE_2D;
            request.tiling = VK_IMAGE_TILING_OPTIMAL;
            request.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (method != route::staging)
                request.usage |= VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT;
            if (test.layers == 6)
                request.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            VkHostImageCopyDevicePerformanceQueryEXT performance{VK_STRUCTURE_TYPE_HOST_IMAGE_COPY_DEVICE_PERFORMANCE_QUERY_EXT};
            VkImageFormatProperties2 result{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
            if (method != route::staging)
                result.pNext = &performance;
            auto status = vkGetPhysicalDeviceImageFormatProperties2(main.physical_device, &request, &result);
            optimal = performance.optimalDeviceAccess;
            identical = performance.identicalMemoryLayout;
            return status == VK_SUCCESS && result.imageFormatProperties.maxExtent.width >= test.side &&
                   result.imageFormatProperties.maxMipLevels >= test.mips && result.imageFormatProperties.maxArrayLayers >= test.layers;
        }
        void verify(VkImage image, shape const& test, vessel::vector<VkBufferImageCopy> const& regions, vessel::vector<uint8_t> const& bytes) {
            ensure(readback, bytes.size(), true);
            begin();
            barrier(image, test, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle,
                                   static_cast<uint32_t>(regions.size()), regions.data());
            VkBufferMemoryBarrier visibility{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            visibility.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            visibility.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            visibility.srcQueueFamilyIndex = visibility.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            visibility.buffer = readback.handle;
            visibility.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &visibility, 0, nullptr);
            times ignored;
            submit(ignored);
            check(vmaInvalidateAllocation(allocator, readback.allocation, 0, bytes.size()), "benchmark readback invalidate");
            require(std::memcmp(readback.info.pMappedData, bytes.data(), bytes.size()) == 0, "benchmark uploaded bytes mismatch");
        }
        times upload(shape const& test, route method, bool warm, vessel::vector<uint8_t> const& bytes,
                     vessel::vector<VkBufferImageCopy> const& regions, vessel::vector<VkMemoryToImageCopyEXT> const& host_regions, bool validate) {
            times measured;
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation memory = VK_NULL_HANDLE;
            VmaAllocationInfo allocation_info{};
            buffer cold;
            auto total = timer::now();
            VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            create.imageType = VK_IMAGE_TYPE_2D;
            create.format = test.format;
            create.extent = {test.side, test.side, 1};
            create.mipLevels = test.mips;
            create.arrayLayers = test.layers;
            create.samples = VK_SAMPLE_COUNT_1_BIT;
            create.tiling = VK_IMAGE_TILING_OPTIMAL;
            create.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (method != route::staging)
                create.usage |= VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT;
            if (test.layers == 6)
                create.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
            VmaAllocationCreateInfo allocation{};
            allocation.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            auto start = timer::now();
            check(vmaCreateImage(allocator, &create, &allocation, &image, &memory, &allocation_info), "benchmark image allocation");
            measured.allocation = elapsed(start);
            measured.image_bytes = allocation_info.size;
            measured.memory_type = allocation_info.memoryType;
            start = timer::now();
            if (method == route::host_current || method == route::host_native) {
                if (method == route::host_current) {
                    begin();
                    barrier(image, test, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_HOST_WRITE_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_HOST_BIT);
                    submit(measured);
                } else {
                    VkHostImageLayoutTransitionInfoEXT transition{VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO_EXT};
                    transition.image = image;
                    transition.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    transition.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                    transition.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, test.mips, 0, test.layers};
                    check(host_transition(main.device, 1, &transition), "benchmark native host transition");
                }
                VkCopyMemoryToImageInfoEXT copy{VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO_EXT};
                copy.dstImage = image;
                copy.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
                copy.regionCount = static_cast<uint32_t>(host_regions.size());
                copy.pRegions = host_regions.data();
                auto call = timer::now();
                check(host_copy(main.device, &copy), "benchmark host copy");
                measured.host_copy = elapsed(call);
                if (method == route::host_current) {
                    begin();
                    barrier(image, test, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                    submit(measured);
                }
                // Native host writes are made visible by the next queue submission (Vulkan spec).
            } else {
                auto prepare = timer::now();
                buffer& staging = warm ? warm_staging : cold;
                ensure(staging, bytes.size());
                std::memcpy(staging.info.pMappedData, bytes.data(), bytes.size());
                check(vmaFlushAllocation(allocator, staging.allocation, 0, bytes.size()), "benchmark staging flush");
                measured.prepare = elapsed(prepare);
                measured.staging_bytes = staging.capacity;
                begin();
                barrier(image, test, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                vkCmdCopyBufferToImage(command, staging.handle, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       static_cast<uint32_t>(regions.size()), regions.data());
                barrier(image, test, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                submit(measured);
            }
            measured.upload = elapsed(start);
            measured.total = elapsed(total);
            // Data generation, readback and object destruction are deliberately outside timings.
            if (validate)
                verify(image, test, regions, bytes);
            release(cold);
            vmaDestroyImage(allocator, image, memory);
            return measured;
        }
        ~context() {
            release(warm_staging);
            release(readback);
            if (fence)
                vkDestroyFence(main.device, fence, nullptr);
            if (pool)
                vkDestroyCommandPool(main.device, pool, nullptr);
            if (allocator)
                vmaDestroyAllocator(allocator);
        }
    };
} // namespace

int main(int argc, char** argv) {
    bool validation = false, quick = false;
    int samples = 15, warmups = 3;
    std::filesystem::path output = "image-upload.csv";
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--validation")
            validation = true;
        else if (arg == "--quick")
            quick = true;
        else if (arg == "--out" && i + 1 < argc)
            output = argv[++i];
        else if ((arg == "--samples" || arg == "--warmups") && i + 1 < argc) {
            int value = 0;
            std::string_view text(argv[++i]);
            auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value > 0 && value <= 1000, "benchmark sample count");
            if (arg == "--samples")
                samples = value;
            else
                warmups = value;
        } else
            require(false, "benchmark invalid argument");
    }
    std::error_code error;
    if (!output.parent_path().empty())
        std::filesystem::create_directories(output.parent_path(), error);
    require(!error, "benchmark output directory");
    std::ofstream csv(output);
    require(static_cast<bool>(csv), "benchmark CSV output");
    csv << std::setprecision(12);
    csv << "case,format,side,layers,mips,payload_bytes,mode,method,sample,allocation_us,prepare_us,host_copy_us,submit_us,wait_us,upload_us,total_us,submits,image_allocation_bytes,staging_capacity_bytes,memory_type,optimal_device_access,identical_layout,validated\n";
    rhi::create_info creation{};
    creation.window_width = creation.window_height = 64;
    creation.window_visible = false;
    creation.validation_layers = validation;
    auto core = deren::engine::load_api_core(creation);
    require(static_cast<bool>(core), "benchmark backend creation");
    auto* opaque = core->main_context();
    require(opaque && deren::vulkan::is_main_context(*opaque), "benchmark Vulkan context");
    auto* native = rhi::do_concrete<deren::vulkan::main_context>(*opaque, deren::vulkan::main_context_name);
    core->wait_idle();
    context gpu(*native);
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(native->physical_device, &properties);
    std::ofstream metadata(output.string() + ".device.txt");
    metadata << "device=" << properties.deviceName << "\ndriver_raw=" << properties.driverVersion << "\napi=" << properties.apiVersion
             << "\nvalidation=" << validation << "\nsamples=" << samples << "\nwarmups=" << warmups << '\n';
    VkPhysicalDeviceMemoryProperties memory_properties{};
    vkGetPhysicalDeviceMemoryProperties(native->physical_device, &memory_properties);
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i)
        metadata << "memory_type_" << i << "_flags=" << memory_properties.memoryTypes[i].propertyFlags
                 << " heap=" << memory_properties.memoryTypes[i].heapIndex << '\n';
    for (uint32_t i = 0; i < memory_properties.memoryHeapCount; ++i)
        metadata << "heap_" << i << "_bytes=" << memory_properties.memoryHeaps[i].size << '\n';
    vessel::vector<shape> cases;
    for (uint32_t side : {16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u})
        if (!quick || side == 16 || side == 256 || side == 1024)
            cases.push_back({"rgba8", VK_FORMAT_R8G8B8A8_UNORM, side, 1, 1});
    for (uint32_t side : {64u, 257u, 1024u, 2048u}) {
        if (quick && side != 257)
            continue;
        cases.push_back({"rgba8_mips", VK_FORMAT_R8G8B8A8_UNORM, side, 1, static_cast<uint32_t>(std::bit_width(side))});
    }
    for (uint32_t side : {64u, 256u, 1024u, 4096u}) {
        if (quick && side != 256)
            continue;
        cases.push_back({"bc7_mips", VK_FORMAT_BC7_UNORM_BLOCK, side, 1, static_cast<uint32_t>(std::bit_width(side))});
    }
    for (uint32_t side : {64u, 256u, 1024u, 2048u}) {
        if (quick && side != 256)
            continue;
        cases.push_back({"rgba16f", VK_FORMAT_R16G16B16A16_SFLOAT, side, 1, 1});
    }
    for (uint32_t side : {64u, 256u, 1024u}) {
        if (quick && side != 64)
            continue;
        cases.push_back({"rgba8_cube_mips", VK_FORMAT_R8G8B8A8_UNORM, side, 6, static_cast<uint32_t>(std::bit_width(side))});
    }
    unsigned verified = 0, skipped = 0;
    for (auto const& test : cases) {
        auto packed = deren::vulkan::detail::packed_image_size(test.format, test.side, test.side, test.layers, test.mips);
        require(packed.has_value(), "benchmark packed size");
        vessel::vector<uint8_t> bytes(static_cast<std::size_t>(*packed));
        for (std::size_t i = 0; i < bytes.size(); ++i)
            bytes[i] = static_cast<uint8_t>((i * 31 + test.side) % 251);
        if (test.format == VK_FORMAT_BC7_UNORM_BLOCK)
            for (std::size_t i = 0; i < bytes.size(); i += 16)
                bytes[i] = 0x40; // valid BC7 mode6
        vessel::vector<VkBufferImageCopy> regions;
        vessel::vector<VkMemoryToImageCopyEXT> host_regions;
        VkDeviceSize offset = 0;
        for (uint32_t mip = 0; mip < test.mips; ++mip) {
            auto side = std::max(1u, test.side >> mip);
            VkBufferImageCopy region{};
            region.bufferOffset = offset;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, test.layers};
            region.imageExtent = {side, side, 1};
            regions.push_back(region);
            VkMemoryToImageCopyEXT host{VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY_EXT};
            host.pHostPointer = bytes.data() + offset;
            host.imageSubresource = region.imageSubresource;
            host.imageExtent = region.imageExtent;
            host_regions.push_back(host);
            offset += *deren::vulkan::detail::packed_mip_size(test.format, side, side, test.layers);
        }
        require(offset == bytes.size(), "benchmark packed region size");
        for (bool warm : {false, true}) {
            if (warm)
                gpu.ensure(gpu.warm_staging, bytes.size());
            for (int sample = -warmups; sample < samples; ++sample) {
                for (unsigned n = 0; n < 4; ++n) {
                    auto method = static_cast<route>((n + static_cast<unsigned>(sample + warmups)) % 4);
                    VkBool32 optimal = 0, identical = 0;
                    if (!gpu.supported(test, method, optimal, identical)) {
                        ++skipped;
                        continue;
                    }
                    bool const validate_bytes = sample == -warmups || sample == samples - 1;
                    auto measured = gpu.upload(test, method, warm, bytes, regions, host_regions, validate_bytes);
                    verified += validate_bytes;
                    if (sample < 0)
                        continue;
                    csv << test.name << ',' << static_cast<int>(test.format) << ',' << test.side << ',' << test.layers << ',' << test.mips << ',' << bytes.size()
                        << ',' << (warm ? "warm" : "cold") << ',' << name(method) << ',' << sample << ',' << measured.allocation << ',' << measured.prepare
                        << ',' << measured.host_copy << ',' << measured.submit << ',' << measured.wait << ',' << measured.upload << ',' << measured.total
                        << ',' << measured.submits << ',' << measured.image_bytes << ',' << measured.staging_bytes << ',' << measured.memory_type
                        << ',' << optimal << ',' << identical << ',' << validate_bytes << '\n';
                }
            }
        }
        csv.flush();
        std::printf("%s %u layers=%u mips=%u bytes=%llu done\n", test.name, test.side, test.layers, test.mips, static_cast<unsigned long long>(bytes.size()));
        std::fflush(stdout);
    }
    csv.close();
    require(static_cast<bool>(csv), "benchmark CSV write");
    std::printf("verified=%u skipped_iterations=%u cases=%zu\n", verified, skipped, cases.size());
    return skipped ? 2 : 0;
}
