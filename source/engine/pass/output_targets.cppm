/** @file
 * @brief Output-sized G-buffer and HDR resources shared by the scene and post passes.
 * This producer owns no recording stage. The chain owns the generations; host routing and barriers
 * retain their existing behavior. scene_color is an alias, not an allocation request.
 *
 * THE STOCHASTIC PUNCTUAL LIGHTING CHAIN (ml_trace / ml_resolve / ml_history) IS NOT HERE ANY MORE: the feature
 * was removed temporarily, so its three per-swapchain-image targets, their heap slots and their half-resolution
 * extents went with it. Family indices below are therefore the ones the runtime's arrays use, and they shifted
 * (rt_shadow_visibility is 13 now).
 */
module;
#include <algorithm>
#include <array>
#include <string_view>
#include <vector>
export module deren.engine.pass.output_targets;
export import deren.utility.shared;
import deren.engine.pass;
import deren.engine.render_resource;
import deren.engine.render_layout;
import deren.promise.rhi;

export namespace deren::engine::pass {
    class output_targets_pass final : public frame_pass {
    public:
        static constexpr size_t resource_count = 15;

    private:
        deren::utility::vessel::array<bool, resource_count> required = [] { deren::utility::vessel::array<bool, resource_count> values; values.fill(true); return values; }();
        render_resource::instances::resource_snapshot resource_view;

    public:
        static constexpr deren::utility::vessel::array ids{
            render_resource::resource_id::hdr, render_resource::resource_id::gbuffer_albedo,
            render_resource::resource_id::gbuffer_normal, render_resource::resource_id::gbuffer_material,
            render_resource::resource_id::velocity, render_resource::resource_id::scene_input,
            render_resource::resource_id::gbuffer_depth, render_resource::resource_id::taa_history, render_resource::resource_id::ldr,
            render_resource::resource_id::bloom_level0, render_resource::resource_id::bloom_level1,
            render_resource::resource_id::bloom_level2, render_resource::resource_id::bloom_level3,
            render_resource::resource_id::rt_shadow_visibility, render_resource::resource_id::visibility_buffer};
        static constexpr deren::utility::vessel::array heap_slots{
            render_layout::heap_slots::post_color, render_layout::heap_slots::gbuffer_albedo,
            render_layout::heap_slots::gbuffer_normal, render_layout::heap_slots::gbuffer_material,
            render_layout::heap_slots::gbuffer_velocity, render_layout::heap_slots::taa_current,
            render_layout::heap_slots::gbuffer_depth, render_layout::heap_slots::taa_history, render_layout::heap_slots::display_color,
            render_layout::heap_slots::bloom_l0,
            render_layout::heap_slots::bloom_l0 + render_layout::heap_image_capacity,
            render_layout::heap_slots::bloom_l0 + 2u * render_layout::heap_image_capacity,
            render_layout::heap_slots::bloom_l0 + 3u * render_layout::heap_image_capacity,
            render_layout::heap_slots::rt_visibility, render_layout::heap_slots::visibility};
        static constexpr deren::utility::vessel::array<uint32_t, resource_count> storage_slots = [] {
            deren::utility::vessel::array<uint32_t, resource_count> slots{};
            slots[13] = render_layout::heap_slots::rt_visibility_storage;
            return slots;
        }();
        static deren::utility::vessel::array<render_resource::instances::request, resource_count> descriptions(render_resource::instances::resource_context const& context) {
            namespace managed = render_resource::instances;
            namespace rhi = deren::promise::rhi;
            deren::utility::vessel::array<managed::request, resource_count> values;
            constexpr deren::utility::vessel::array formats{render_layout::hdr_format, render_layout::gbuffer_formats[0],
                                                            render_layout::gbuffer_formats[1], render_layout::gbuffer_formats[2],
                                                            render_layout::gbuffer_velocity_format, render_layout::hdr_format, rhi::image_format::depth,
                                                            render_layout::hdr_format, render_layout::hdr_format, render_layout::hdr_format,
                                                            render_layout::hdr_format, render_layout::hdr_format, render_layout::hdr_format,
                                                            rhi::image_format::r16_sfloat, render_layout::visibility_format};
            for (size_t i = 0; i < values.size(); ++i) {
                auto& value = values[i];
                value.id = ids[i];
                value.life = i == 7 ? managed::lifetime::history : managed::lifetime::output_size;
                value.scope = i == 13 ? render_resource::resource_scope::per_frame_slot : render_resource::resource_scope::per_swapchain_image;
                value.image.extent = context.render_extent;
                if (i >= 9 && i <= 12) {
                    value.image.extent.width = std::max(1u, context.render_extent.width >> (i - 8));
                    value.image.extent.height = std::max(1u, context.render_extent.height >> (i - 8));
                }
                value.image.format = formats[i];
                value.image.flags = rhi::to_bits(rhi::image_flag::sampled) |
                                    rhi::to_bits(i == 6 ? rhi::image_flag::depth_attachment : rhi::image_flag::color_attachment);
                // THE DEPTH-PROBE CAPTURE reads it back (visibility-buffer root-cause measurement: the shared G-buffer
                // depth is copied to host memory on the screenshot frame, see runtime.post_graph.cpp / runtime.readback.cppm).
                // THE TARGETS THE VISIBILITY RESOLVE WRITES AS STORAGE IMAGES (v0.4b): a compute stage cannot use a
                // colour attachment, so these families are created with STORAGE usage as well. Family 0 is included
                // because the scene colour target ALTERNATES between family 5 (the TAA working image) and family 0
                // (the HDR image) - see `runtime::resolve_pass`, which publishes the storage descriptors per frame
                // and knows which one this frame's scene colour is.
                if (i <= 5)
                    value.image.flags |= rhi::to_bits(rhi::image_flag::storage);
                if (i == 7)
                    value.image.flags = rhi::to_bits(rhi::image_flag::sampled) | rhi::to_bits(rhi::image_flag::transfer_destination);
                if (i == 13)
                    value.image.flags = rhi::to_bits(rhi::image_flag::sampled) | rhi::to_bits(rhi::image_flag::storage);
                if (i == 0)
                    value.image.flags |= rhi::to_bits(rhi::image_flag::transfer_source);
                if (i == 6)
                    value.image.flags |= rhi::to_bits(rhi::image_flag::transfer_source);
                value.image.debug_name = "managed output target";
            }
            return values;
        }
        void select_resources(deren::utility::vessel::array<bool, resource_count> selection) noexcept {
            required = selection;
        }
        void describe_resources(render_resource::instances::resource_context const& context,
                                deren::utility::vessel::vector<render_resource::instances::request>& requests) const override {
            auto const values = descriptions(context);
            for (size_t i = 0; i < values.size(); ++i)
                if (required[i])
                    requests.push_back(values[i]);
        }
        void bind_resources(render_resource::instances::resource_snapshot const& snapshot) override {
            resource_view = snapshot;
        }
        render_resource::instances::resource_snapshot const& resources() const noexcept {
            return resource_view;
        }
        render_resource::pass_io const& io() const noexcept override {
            static render_resource::pass_io const value{.name = "output_targets"};
            return value;
        }
        deren::engine::pass::behaviour const& behaviour() const noexcept override {
            static deren::engine::pass::behaviour const value{.extent = extent_rule::none, .resync_viewport = false};
            return value;
        }
        std::string_view feature() const noexcept override {
            return {};
        }
        void create(pass_context const&) override {
        }
        void on_swapchain_recreated(pass_host const&) override {
        }
    };
} // namespace deren::engine::pass
