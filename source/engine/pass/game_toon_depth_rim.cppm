// module version: 0.1.0  (independent of the app version in CMakeLists project(VERSION))

/**
 * @file source/engine/pass/game_toon_depth_rim.cppm
 * @brief THE GAME-SHADER CHAIN'S SCREEN-SPACE DEPTH RIM: a contour drawn where the character meets something nearer.
 * @defgroup engine_pass_game_toon_depth_rim Game Toon Depth-Rim Pass
 *
 * WHY IT EXISTS, SEPARATELY FROM `toon_screen_rim`: the article's contour is silenced whenever a REWRITTEN chain
 * is drawing (see `toon_screen_rim_pass::feature`), and the Goo chain has its own `goo_rim` stage. The third
 * chain - the game-shader model ported from the pinned MyZmdShaders sources - has no such stage: its own rim is
 * computed INLINE in the character fragment while the captured viewer's hair/face contour is a DEPTH-CONTOUR
 * term, which a fragment stage cannot sample (the depth is its own attachment). The renderer already owns the
 * only legal way to read that depth: a fullscreen additive pass after the character stage and before the resolve,
 * with the depth SAMPLED and no depth attachment. This is that pass for the game chain, added as a SEPARATE
 * declaration rather than a mode of `toon_screen_rim`, because `feature_active` is asked once per NAME: "the
 * article's contour is off under the game chain" and "the game chain's contour is on" have to be two answers that
 * cannot drift apart, exactly the argument that made `goo_rim` a name of its own.
 *
 * WHY IT IS NOT `goo_rim` EITHER: the Goo chain's rim is a PRODUCT of three attenuations and a screen-space depth
 * factor recomposed from the G-buffer, with the Goo material table's colours. The game chain's contour is the
 * captured viewer's simple depth-contour term, and it takes its colour and shape from THIS pass's parameters.
 * Sharing the stage would make the two chains' rim constants one lane.
 *
 * WHAT IT OWNS: its pipeline (ONE target, the scene colour, ADDITIVE blending, so the pass adds to the frame
 * instead of reading and rewriting it), the 48-byte push block, and the draw.
 *
 * WHAT IT DOES NOT OWN: the geometry (a synthetic fullscreen triangle), the G-buffer and the depth (per-image
 * heap slots), and their transition to a sampled layout, which is the graph's/rendering bookkeeping.
 *
 * IT IS GATED BY `runtime::game_toon_active()` AND THE CHARACTER STAGE'S EXISTENCE at the post-graph lane (see
 * runtime.post_graph.cpp): the silhouette belongs to the game-shaded character and means nothing without it.
 * Its `feature()` still names its own feature - `"game_toon_depth_rim"` - so the declaration carries the question
 * it answers even though the owner's table does not need a second branch to ask the composed predicate directly.
 */

module;

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

export module deren.engine.pass.game_toon_depth_rim;
export import deren.utility.shared;
import deren.promise.rhi;

import deren.engine.pass;
import deren.engine.render_resource;
import deren.engine.pipelines; // vk_pipeline: the RAII owner of the pipeline this pass builds

export namespace deren::engine::pass {

    /**
     * @brief the game chain's depth rim: a fullscreen additive contour over the game-shaded frame
     *
     * It has no frame of its own - every value it needs is either the frame's (the two projection terms) or its
     * own parameters (the rim's width, gain, strength and colour) - so it follows `deferred`, `gbuffer-debug` and
     * `toon_screen_rim` in taking its parameters from the pass and nothing from the renderer.
     */
    class game_toon_depth_rim_pass final : public frame_pass {
    public:
        /// @brief the push block, which is also `game_toon_depth_rim.slang`'s
        struct push_constants {
            float proj_22 = 0.0f; // the depth-linearization terms, from the frame's projection
            float proj_32 = 0.0f;
            float rim_width = 0.0f;    // how far along the screen the depth comparison reaches
            float rim_scale = 0.0f;    // the gain on the depth difference
            float rim_strength = 0.0f; // 0 draws nothing at all
            float pad0 = 0.0f;
            float pad1 = 0.0f;
            float pad2 = 0.0f;
            float rim_colour[4] = {1.0f, 0.94f, 0.90f, 1.0f};
            // THE HEAP INDICES ARE APPENDED BY THE FRAMEWORK, not composed here (see the shader's note and
            // gbuffer_debug's identical one) - so this struct is the PASS's block and nothing more.
        };

        /**
         * @brief this pass's own declaration, kept beside the pass (the pipeline it builds is its own)
         *
         * THE DECLARATION IS THE PASS'S, NOT A SHARED SCHEMA ENTRY, and that is deliberate rather than a shortcut:
         * the framework resolves a pass through `io()` and nothing enumerates a global table of declarations, so a
         * fullscreen stage whose whole vocabulary is "write the scene colour, sample the depth/albedo heap slots"
         * can state it here. `toon_screen_rim_io` lives in `render_resource` because its declaration is shared with
         * the article's rim documentation; this one is not shared with anything.
         */
        inline static constexpr deren::utility::vessel::array<render_resource::render_target, 1> pass_targets = {{
            {.resource = resource_id::scene_color, .element = 0},
        }};
        inline static constexpr render_resource::pass_io pass_declaration = {
            .name = "game_toon_depth_rim",
            .bindings = {},
            .targets = pass_targets,
            .barrier_images = {},
            .barrier_buffers = {},
            .push = render_resource::push_block{.offset = 0, .size = 48, .stages = render_resource::stage_flag::fragment},
        };

        game_toon_depth_rim_pass() = default;
        ~game_toon_depth_rim_pass() override;

        [[nodiscard]] render_resource::pass_io const& io() const noexcept override;
        [[nodiscard]] deren::engine::pass::behaviour const& behaviour() const noexcept override;
        [[nodiscard]] std::string_view feature() const noexcept override;
        void create(pass_context const& context) override;
        void on_swapchain_recreated(pass_host const& host) override;

        /// \brief the record body alone - none of this pass's own ordering
        ///
        /// The graph recorder calls THIS entry and the graph emits the input transitions and the target write;
        /// the pass owns no barriers (its sampled inputs are heap slots the graph declares on its behalf).
        void record_impl(resolved_io const& io);
        deren::promise::rhi::error record_content(resolved_io const& io) noexcept override;

        /// @brief whether the pass built the pipeline it records with (the renderer's gate asks this)
        [[nodiscard]] bool pipeline_ready() const noexcept;
        /// @brief the framework's generic form of the same question
        [[nodiscard]] bool ready() const noexcept override {
            return this->pipeline_ready();
        }
        [[nodiscard]] deren::promise::rhi::pipeline* pipeline_handle() const noexcept override;

        /**
         * @brief the rim's shape, which is THIS pass's parameter
         * @param width the screen-offset scale (the reference's `rimWidth`, in its own units)
         * @param scale the gain on the depth difference (the reference's `* 4`)
         * @param strength 0 draws nothing, which is how the feature's off state costs nothing
         */
        void set_shape(float width, float scale, float strength) noexcept;
        /// @brief the contour's colour (the captured profile's rim colour)
        void set_colour(float r, float g, float b) noexcept;

    private:
        static constexpr std::string_view vertex_shader_name = "post.vert.spv"; // the synthetic fullscreen triangle
        static constexpr std::string_view fragment_shader_name = "game_toon_depth_rim.frag.spv";

        static constexpr deren::utility::vessel::array<std::string_view, 1> pipeline_names = {"game-toon-depth-rim"};
        // called pass_behaviour, not behaviour: the class declares behaviour() and a member of that name
        // would duplicate it and hide the override.
        inline static constexpr deren::engine::pass::behaviour pass_behaviour = {
            .kind = behaviour_kind::fullscreen,
            .extent = extent_rule::full, // the contour is drawn at the frame's resolution
            .extent_of = resource_id::none,
            .pipelines = pipeline_names,
            .resync_viewport = true, // the record step sets the viewport and scissor from io.extent
        };
        void release_owned() noexcept;
        deren::promise::rhi::api_core* built_against = nullptr;
        // called pass_pipeline, not pipeline: the class declares pipeline_handle() and a member of that name
        // would duplicate it and hide the override.
        std::optional<pipelines::pipeline_handle> pass_pipeline = std::nullopt;
        float rim_width = 1.0f;
        float rim_scale = 4.0f;
        float rim_strength = 0.55f;
        float rim_colour[3] = {1.0f, 0.94f, 0.90f};
    }; // namespace deren::engine::pass

    /// the pass's own block: the eight composed values plus the colour, and the framework appends the two heap
    /// lanes on top (see the shader).
    static_assert(sizeof(game_toon_depth_rim_pass::push_constants) == game_toon_depth_rim_pass::pass_declaration.push->size,
                  "the game chain rim's declared push block must be the size of the struct the pass composes");

} // namespace deren::engine::pass
