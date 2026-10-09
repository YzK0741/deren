// module version: 0.2.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file source/engine/pass/scene.cppm
 * @brief The THIRD real pass: the scene itself - the frame's primitives, drawn into the surface targets.
 * @defgroup vulkan_pass_scene Scene Pass
 *
 * WHY THIS ONE IS DIFFERENT FROM THE OTHER TWO, and it is the reason the framework did not have this shape
 * before: its WORK IS DATA. A compute pass dispatches a declared number of groups; a fullscreen resolve draws
 * one triangle; this pass draws whatever the frame's culling left visible, each piece through its own pipeline
 * and its own push constants. Nothing in a declaration can say that, so the renderer hands the list over - see
 * `scene_frame` - and this pass owns what is left once the list exists: the rendering instance over the
 * declared targets, the segment strategy (one secondary, or several recorded in parallel), the per-segment draw
 * state, the order of the draws, and ending the instance the moment it is done with it.
 *
 * THIS PASS FIXES A COUPLING BY EXISTING. The instance it opens used to be opened by one function
 * (`runtime::record_opaque_scene`) and closed by another (`runtime::record_scene_tail`), with the whole rest of
 * the frame's work in between - so "who owns the open instance" was a fact spread over two scopes, and the
 * close had to be remembered on every path out of the pass. Here the open and the close are two statements in
 * one function.
 *
 * WHAT IT OWNS AND WHAT IT ONLY USES, because this one is a genuine split rather than a move:
 *  * OWNS the instance (its colour and depth attachments come from its declaration), the segment strategy, the
 *    secondary begin/end and its inheritance, the draw loop, and the per-segment `render_environment`'s life;
 *  * USES the pipeline REGISTRY, and that is deliberate: a leaf asks for a pipeline by name, the names are the
 *    renderer's (`"pbr"`, `"unlit"`, `"gbuffer"`, whatever the scene tree requests), and the registry's
 *    concurrency contract belongs to the runtime. So the renderer builds the environment (a fresh one per
 *    segment - the pipeline-dedup state is per recording session) and this pass asks for it;
 *  * USES the parallel scheduler for the same reason: the task pool, its priorities and the per-slot secondary
 *    command buffers are the frame loop's resources, so the renderer hands over a callback and the pass builds
 *    the tasks. When the framework learns to schedule segments itself, that callback is what disappears - the
 *    pass's per-segment recording does not change.
 */

module;

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory> // std::shared_ptr: a frame's session target is the contract handle (see make_environment)
#include <span>
#include <string_view>

export module deren.engine.pass.scene;

import deren.promise.rhi; // the contract command buffer the frame's secondaries are
import deren.engine.pass;
import deren.engine.render_resource;
import deren.engine.render_resource.shared;
import deren.engine.primitive;          // the leaves this pass draws (a concrete pass may know the scene types)
import deren.engine.render_environment; // the per-segment draw state the renderer builds for it

export namespace deren::engine::pass {

    /// @brief one per-slot secondary command buffer for a recording segment
    /// @note the command POOL does not travel with it: the backend's `vk_command_buffer` owns its own
    ///       pool now (one per buffer, which is what keeps concurrent recording threads off a shared
    ///       pool), and no pass ever read the pool this struct used to carry.
    struct segment_buffer {
        /// THE CONTRACT HANDLE (abi 20), BORROWED: the frame loop owns the segment buffers (they are its
        /// per-slot `object_manager<command_buffer>` entries), and the pass only records into them - so this
        /// is a raw pointer, never a shared pointer, and the pass must not release it.
        deren::promise::rhi::command_buffer* buffer = nullptr;
    };

    /**
     * @brief the SCENE, as the renderer hands it to the pass
     *
     * Everything in here is a fact this pass cannot know and must not own: the leaves are the scene's (culled
     * by the frame loop), the secondaries and the task pool are the frame loop's resources, and the pipeline
     * registry and the shared scene layout are the renderer's. What the pass does with them is the part that
     * IS its own: the instance, the segmentation, and the order of the draws.
     */
    struct scene_frame {
        /// the visible opaque leaves, in draw order (the pass draws them in exactly this order)
        std::span<primitive const* const> leaves = {};
        /// the per-slot secondary command buffers the segments are recorded into
        std::span<segment_buffer const> segments = {};
        /**
         * Build the draw state for ONE segment.
         *
         * A callback rather than a ready environment because the environment carries the pipeline-dedup state
         * ("which pipeline is bound right now"), which is per RECORDING SESSION: a segment that started with
         * the previous segment's state would skip a bind it needs. The renderer builds it because the registry
         * is its, and `gbuffer` says which pass's default a leaf with default semantics wants.
         */
        render_environment (*make_environment)(void* owner, std::shared_ptr<deren::promise::rhi::command_buffer> command_buffer, bool gbuffer) = nullptr;
        /// record the segments in parallel through the renderer's task pool (same reason as above)
        void (*run_tasks)(void* owner, std::span<std::function<void()>> tasks) = nullptr;
        void* owner = nullptr;
        // (THE HEAP-BIND FIELD IS GONE - plan X5 B3.5. It existed to chain the two `VkBindHeapInfoEXT` a
        //  secondary must inherit, because a secondary is VALIDATED ON ITS OWN and the heap bound on the primary
        //  does not reach it - but THE PASS STOPPED CALLING IT AT ABI 20: a secondary is begun through the
        //  contract, whose chain carries the ATTACHMENT inheritance alone, and the BACKEND derives the
        //  descriptor-heap inheritance from the heaps it owns. The field, the hook that filled it and the two raw
        //  structs were a second truth with no reader, and the frame's publisher stopped filling them too.)
        /// the attachments a SECONDARY must inherit (dynamic rendering): formats in attachment order + depth
        std::span<::deren::promise::rhi::image_format const> color_formats = {};
        ::deren::promise::rhi::image_format depth_format = ::deren::promise::rhi::image_format::unknown;
        std::uint32_t samples = 1u;
        /// whether this frame writes the G-buffer (the surface pass) or shades into the HDR target
        bool gbuffer = true;
        deren::promise::rhi::image_extent extent = {0, 0};
    };

    /**
     * @brief the frame's scene: the primitives, drawn into the surface targets the declaration names
     *
     * The instance it opens covers EXACTLY this pass - the lighting stage, the transparent pass, the resolve
     * and the lighting chain all run after it ends, each opening its own - which is why it can own the whole thing.
     */
    class scene_pass final : public frame_pass {
    public:
        scene_pass() = default;
        ~scene_pass() override = default;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override;
        [[nodiscard]] deren::engine::pass::behaviour const& behaviour() const noexcept override;
        [[nodiscard]] std::string_view feature() const noexcept override;
        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const& host) override;
        void record(resolved_io const& io) override;

        /// @brief hand over this frame's scene (the renderer calls it right before the stage is recorded)
        void set_frame(scene_frame const& frame) noexcept;

    private:
        /// the renderer's own threshold for when parallel recording is worth it (few leaves: one segment)
        static constexpr std::size_t min_leaves_for_parallel = 4;

        /// NO pipeline names, and that is not an omission: a leaf names the pipeline it wants and the
        /// renderer's registry resolves it (see `scene_frame::make_environment`), so there is nothing for the
        /// runner to bind before this pass records - the pass binds per segment, through the draw path.
        static constexpr std::array<std::string_view, 0> pipeline_names = {};
        // called pass_behaviour, not behaviour: the class declares behaviour() and a member of that name
        // would duplicate it and hide the override.
        inline static constexpr deren::engine::pass::behaviour pass_behaviour = {
            .kind = behaviour_kind::graphics,
            .extent = extent_rule::full,
            .extent_of = resource_id::none,
            .pipelines = pipeline_names,
            // the scene pipelines carry their own stored viewport (the renderer resyncs it once per frame in
            // update_pass_geometry, and each bind applies it), so the runner has nothing to set here
            .resync_viewport = false,
        };

        /// begin one secondary with the instance's attachment inheritance, or report that it could not
        [[nodiscard]] bool begin_segment(deren::promise::rhi::command_buffer& command_buffer) const;
        /// one segment's content: every leaf of that segment through its draw path (the heaps are already bound)
        void record_segment(deren::promise::rhi::command_buffer& command_buffer, std::span<primitive const* const> leaves) const;

        // called pass_frame, not frame: set_frame()'s frame parameter in scene.cpp would hide a member of that name
        // and MSVC /W4 reports C4458 (an error under /WX).
        scene_frame pass_frame = {};
    };

} // namespace deren::engine::pass
