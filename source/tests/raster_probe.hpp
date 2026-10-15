#pragma once
#include <utility>
#include <vector>
import deren.promise.rhi;
namespace rhi = deren::promise::rhi;
namespace raster_test {
    using namespace deren::promise::rhi;
    struct raster_probe_buffer : rhi::buffer {
        unsigned index;
        int& releases;
        std::uint64_t capacity;
        bool host_enabled = false;
        deren::utility::vessel::vector<std::byte> storage;
        raster_probe_buffer(unsigned i, int& r, std::uint64_t size)
            : buffer("gaussian_raster_probe_buffer")
            , index(i)
            , releases(r)
            , capacity(size) {
        }
        void release() noexcept override {
            ++releases;
            delete this;
        }
        std::uint64_t size() const noexcept override {
            return capacity;
        }
        std::span<std::byte> mapped() noexcept override {
            return host_enabled ? std::span{storage} : std::span<std::byte>{};
        }
    };
    struct raster_probe_address : rhi::device_address {
        unsigned zero_at = 0, unaligned_at = 0;
        raster_probe_address()
            : device_address("gaussian_raster_probe_address") {
        }
        std::uint64_t buffer_address(rhi::buffer const& b, std::uint64_t) const noexcept override {
            auto const& buffer = static_cast<raster_probe_buffer const&>(b);
            return buffer.index == zero_at ? 0 : buffer.index * 4096ull + (buffer.index == unaligned_at ? 1 : 0);
        }
    };
    struct raster_probe_pipeline : rhi::pipeline {
        int* releases;
        explicit raster_probe_pipeline(int& count)
            : pipeline("raster_probe_pipeline")
            , releases(&count) {
        }
        void release() noexcept override {
            ++*releases;
            delete this;
        }
    };

    struct raster_probe_core : rhi::api_core {
        bool supported = true;
        bool pipeline_fail = false;
        int pipeline_releases = 0;
        unsigned pipeline_calls = 0;
        deren::utility::vessel::vector<rhi::image_format> colors;
        deren::utility::vessel::vector<rhi::blend_mode> blends;
        rhi::shader_stage first_stage = rhi::shader_stage::compute;
        rhi::image_format depth_format = rhi::image_format::unknown;
        bool depth_test = false;

        unsigned fail_at = 0, calls = 0, undersized_at = 0;
        int releases = 0;
        int idle_calls = 0;
        raster_probe_address address;
        deren::utility::vessel::vector<rhi::buffer_desc> descriptors;
        deren::utility::vessel::vector<deren::utility::vessel::vector<std::byte>> bytes;
        raster_probe_core()
            : api_core("gaussian_raster_probe_core") {
        }
        rhi::ability_bits abilities() const noexcept override {
            return supported ? rhi::to_bits(rhi::extension_kind::device_address) : 0;
        }
        rhi::extension* query_extension(rhi::extension_kind kind) noexcept override {
            return supported && kind == rhi::extension_kind::device_address ? &address : nullptr;
        }
        rhi::buffer* create_buffer(rhi::buffer_desc const& desc) override {
            ++calls;
            if (calls == fail_at)
                return nullptr;
            auto copy = desc;
            copy.initial_bytes = {};
            descriptors.push_back(copy);
            bytes.emplace_back(desc.initial_bytes.begin(), desc.initial_bytes.end());
            auto* result = new raster_probe_buffer(calls, releases, calls == undersized_at ? desc.size - 1 : desc.size);
            result->host_enabled = desc.usage == rhi::buffer_usage::storage_coherent;
            if (result->host_enabled) {
                result->storage.resize(static_cast<std::size_t>(desc.size));
                std::copy(desc.initial_bytes.begin(), desc.initial_bytes.end(), result->storage.begin());
            }
            return result;
        }
        rhi::swapchain* create_swapchain(rhi::swapchain_desc const&) override {
            return nullptr;
        }
        rhi::image* create_image(rhi::image_desc const&) override {
            return nullptr;
        }
        rhi::acceleration_structure* create_acceleration_structure(rhi::acceleration_structure_desc const&) override {
            return nullptr;
        }
        rhi::micromap* create_micromap(rhi::micromap_desc const&) override {
            return nullptr;
        }
        rhi::sampler* create_sampler(rhi::sampler_desc const&) override {
            return nullptr;
        }
        rhi::shader* create_shader(rhi::shader_desc const&) override {
            return nullptr;
        }
        rhi::pipeline* create_pipeline(rhi::pipeline_desc const& desc) override {
            ++pipeline_calls;
            colors.assign(desc.color_formats.begin(), desc.color_formats.end());
            blends.assign(desc.blend_modes.begin(), desc.blend_modes.end());
            first_stage = desc.first_stage;
            depth_format = desc.depth_format;
            depth_test = desc.depth_test;
            return pipeline_fail ? nullptr : new raster_probe_pipeline(pipeline_releases);
        }
        rhi::query* create_query(rhi::query_desc const&) override {
            return nullptr;
        }
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
        void wait_idle() override {
            ++idle_calls;
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
        rhi::error submit(rhi::command_buffer&) override {
            return rhi::error::unsupported;
        }
        rhi::command_buffer* create_command_buffer(rhi::command_buffer_desc const&) override {
            return nullptr;
        }
        std::shared_ptr<rhi::command_buffer> make_command_buffer(rhi::command_buffer_desc const&) override {
            return {};
        }
        std::uint32_t api_version() const noexcept override {
            return rhi::abi_version;
        }
    };
    struct raster_probe_commands : rhi::command_buffer {
        void reset() {
            draws = vertices = instances = barriers = begins = ends = binds = fail_barrier = 0;
            fail_bind = fail_begin = false;
            depth_write = true;
            cull = rhi::cull_mode::back;
            color_load = depth_load = rhi::load_op::clear;
            transitions.clear();
        }

        unsigned draws = 0, vertices = 0, instances = 0, barriers = 0, begins = 0, ends = 0, binds = 0;
        unsigned fail_barrier = 0;
        bool fail_bind = false, fail_begin = false, depth_write = true;
        rhi::cull_mode cull = rhi::cull_mode::back;
        rhi::load_op color_load = rhi::load_op::clear, depth_load = rhi::load_op::clear;
        deren::utility::vessel::vector<std::pair<rhi::image_use, rhi::image_use>> transitions;
        raster_probe_commands()
            : command_buffer("raster_probe_commands") {
        }
        void release() noexcept override {
        }
        rhi::error begin_recording(command_buffer_begin_info const& info) override {
            (void)info;
            return rhi::error::ok;
        }
        rhi::error end_recording() noexcept override {
            return rhi::error::ok;
        }
        rhi::error execute(command_buffer& secondary) override {
            (void)secondary;
            return rhi::error::ok;
        }
        rhi::error use(image const& resource, image_use from, image_use to) noexcept override {
            (void)resource;
            (void)from;
            (void)to;
            return rhi::error::ok;
        }
        rhi::error copy_image_to_buffer(buffer& destination, image const& source, image_copy_region const& region) noexcept override {
            (void)destination;
            (void)source;
            (void)region;
            return rhi::error::ok;
        }
        rhi::error begin_gpu_timing() noexcept override {
            return rhi::error::ok;
        }
        rhi::error mark_gpu_timing(std::uint32_t mark_index, std::string_view stage_name) noexcept override {
            (void)mark_index;
            (void)stage_name;
            return rhi::error::ok;
        }
        rhi::error begin_rendering(rendering_info const& info) override {
            (void)info;
            ++begins;
            if (fail_begin) {
                return rhi::error::unsupported;
            }
            color_load = info.colors[0].load;
            depth_load = info.depth.load;
            return rhi::error::ok;
        }
        void end_rendering() noexcept override {
            ++ends;
        }
        rhi::error bind_pipeline(pipeline const& handle) override {
            (void)handle;
            ++binds;
            return fail_bind ? rhi::error::unsupported : rhi::error::ok;
        }
        rhi::error bind_vertex_buffer(buffer const& handle, std::uint64_t offset) override {
            (void)handle;
            (void)offset;
            return rhi::error::ok;
        }
        rhi::error bind_index_buffer(buffer const& handle, std::uint64_t offset, index_type type) override {
            (void)handle;
            (void)offset;
            (void)type;
            return rhi::error::ok;
        }
        void draw(std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex, std::uint32_t first_instance) noexcept override {
            (void)vertex_count;
            (void)instance_count;
            (void)first_vertex;
            (void)first_instance;
            ++draws;
            vertices = vertex_count;
            instances = instance_count;
        }
        void draw_indexed(std::uint32_t index_count, std::uint32_t instance_count, std::uint32_t first_index, std::int32_t vertex_offset, std::uint32_t first_instance) noexcept override {
            (void)index_count;
            (void)instance_count;
            (void)first_index;
            (void)vertex_offset;
            (void)first_instance;
        }
        void dispatch(std::uint32_t groups_x, std::uint32_t groups_y, std::uint32_t groups_z) noexcept override {
            (void)groups_x;
            (void)groups_y;
            (void)groups_z;
        }
        void draw_mesh_tasks(std::uint32_t groups_x, std::uint32_t groups_y, std::uint32_t groups_z) noexcept override {
            (void)groups_x;
            (void)groups_y;
            (void)groups_z;
        }
        rhi::error draw_indirect(buffer const&, std::uint64_t, std::uint32_t, std::uint32_t) override { return rhi::error::unsupported; }
        rhi::error draw_mesh_tasks_indirect(buffer const& argument_buffer, std::uint64_t offset, std::uint32_t count, std::uint32_t stride) override {
            (void)argument_buffer;
            (void)offset;
            (void)count;
            (void)stride;
            return rhi::error::ok;
        }
        void trace_rays(shader_binding_table_region const& raygen, shader_binding_table_region const& miss, shader_binding_table_region const& hit, shader_binding_table_region const& callable, std::uint32_t width, std::uint32_t height, std::uint32_t depth) noexcept override {
            (void)raygen;
            (void)miss;
            (void)hit;
            (void)callable;
            (void)width;
            (void)height;
            (void)depth;
        }
        rhi::error build_acceleration_structure(acceleration_structure& target) override {
            (void)target;
            return rhi::error::ok;
        }
        rhi::error refit_acceleration_structure(acceleration_structure& target) override {
            (void)target;
            return rhi::error::ok;
        }
        rhi::error build_micromap(micromap& target) override {
            (void)target;
            return rhi::error::ok;
        }
        void set_viewport(viewport const& vp) noexcept override {
            (void)vp;
        }
        void set_scissor(rect const& scissor) noexcept override {
            (void)scissor;
        }
        void set_cull_mode(cull_mode mode) noexcept override {
            cull = mode;
        }
        void set_depth_write(bool enable) noexcept override {
            depth_write = enable;
        }
        void set_depth_bias(float constant_factor, float slope_factor, float clamp) noexcept override {
            (void)constant_factor;
            (void)slope_factor;
            (void)clamp;
        }
        rhi::error barrier(barrier_group const& group) override {
            ++barriers;
            if (barriers == fail_barrier) {
                return rhi::error::unsupported;
            }
            for (auto const& b : group.images) {
                transitions.emplace_back(b.from, b.to);
            }
            return rhi::error::ok;
        }
        rhi::error barrier(image_barrier const& one) override {
            return barrier(rhi::barrier_group{.images = std::span{&one, std::size_t{1}}});
        }
        rhi::error copy_image(image_copy const& copy) override {
            (void)copy;
            return rhi::error::ok;
        }
        rhi::error copy_buffer(buffer& destination, buffer const& source, std::uint64_t size, std::uint64_t source_offset, std::uint64_t destination_offset) override {
            (void)destination;
            (void)source;
            (void)size;
            (void)source_offset;
            (void)destination_offset;
            return rhi::error::ok;
        }
        rhi::error clear_color_image(image const& target, deren::utility::vessel::array<float, 4> const& color, subresource_range const& range) override {
            (void)target;

            (void)color;
            (void)range;
            return rhi::error::ok;
        }
    };

} // namespace raster_test
using raster_test::raster_probe_commands;
using raster_test::raster_probe_core;
