// -*- C++ -*-
// module: deren.promise.rhi:pipeline
module;

#include <cstddef>
#include <cstdint>

export module deren.promise.rhi:pipeline;
export import deren.vstd;
export import deren.utility.shared;

import :contract;
import :resource;

export namespace deren::promise::rhi {

    /// The shader stage a `shader_desc`'s code is compiled for. Values are the renderer's actual
    /// stage vocabulary; appended, never moved.
    enum class shader_stage : std::uint32_t {
        vertex = 0,
        mesh = 1, ///< the stage that REPLACES the vertex stage (docs/mesh_shaders.md)
        fragment = 2,
        compute = 3,
        /// THE RAY-TRACING STAGES, APPENDED (abi 21 - adding VALUES is explicitly not an abi change, see
        /// `abi_version`). They name the five entry points a ray-tracing pipeline is built from, and they exist
        /// so a ray-tracing shader module is NOT mislabeled as a compute one: the module itself is stage-less,
        /// but the pipeline's stage info is what the group table indexes, and a caller that cannot say
        /// "raygen" cannot build a group table at all.
        ray_generation = 4,
        miss = 5,
        closest_hit = 6,
        any_hit = 7,
        intersection = 8,
    };

    /// How a shader is created: its stage and its SPIR-V. Same append-only `struct_size` guard.
    struct shader_desc {
        std::uint32_t struct_size = sizeof(shader_desc); ///< size of this structure as the CALLER compiled it
        shader_stage stage = shader_stage::vertex;
        std::span<std::byte const> code;  ///< the SPIR-V
        char const* debug_name = nullptr; ///< what the backend logs on refusal; not retained
    };

    /// THE FOUR BLEND RECIPES this renderer's pipelines actually use (the survey found exactly these
    /// four behind the raw `VkPipelineColorBlendAttachmentState` constants): a caller names the MODE,
    /// the backend spells the factors and ops. Appended, never moved.
    enum class blend_mode : std::uint32_t {
        opaque = 0,   ///< overwrite: the G-buffer's surface targets (alpha carries data there)
        alpha = 1,    ///< classic src-alpha over
        additive = 2, ///< accumulate lighting over the stored scene color
        multiply = 3, ///< multiply: the toon chain's light modulation
    };

    /// The depth compare operator a pipeline's depth test uses. The renderer's vocabulary today:
    /// the LESS_OR_EQUAL default and the character-forward pass's EQUAL overwrite.
    enum class depth_compare : std::uint32_t {
        less_or_equal = 0,
        equal = 1,
        greater = 2, // appended for the character-forward eye depth prepass; existing values keep their numbers
    };

    /// ONE STAGE OF A RAY-TRACING PIPELINE (appended with the ray-tracing spelling, abi 21).
    ///
    /// A ray-tracing pipeline is built from several entry points at once, and its GROUPS index this list - so
    /// the stage kind and the code travel together, and the ORDER here is what `ray_tracing_group`'s indices
    /// mean. `debug_name` is the backend's log text for a stage it refuses, exactly as it is for a shader.
    struct ray_tracing_stage {
        shader_stage stage = shader_stage::ray_generation;
        std::span<std::byte const> code;
        char const* debug_name = nullptr;
    };

    /// "THIS GROUP SLOT NAMES NO STAGE", the spelling a `ray_tracing_group` uses for the slots it leaves empty
    /// (`VK_SHADER_UNUSED_KHR` in the API's vocabulary, as a value so the descriptor stays portable).
    inline constexpr std::uint32_t shader_group_none = 0xFFFFFFFFu;

    /// ONE SHADER GROUP of a ray-tracing pipeline: the STAGE INDICES it binds, in the order
    /// `ray_tracing_stages` lists them.
    ///
    /// A GENERAL group names ONE shader (`general`: a raygen, a miss or an intersection entry); a HIT group
    /// names up to three (`closest_hit`, `any_hit`, `intersection`) and its geometry kind. WHICH KIND a group is
    /// follows from which slots are filled, which is the rule the API's group type states - so the
    /// descriptor does not carry a group-type enumerator that could disagree with the slots.
    ///
    /// THE GROUP ORDER IS THE CALLER'S SHADER BINDING TABLE ORDER: the caller fills its SBT regions in exactly
    /// this order (see `pass::rt_shadow_pass`), which is why the count travels back with the pipeline.
    struct ray_tracing_group {
        std::uint32_t general = shader_group_none;
        std::uint32_t closest_hit = shader_group_none;
        std::uint32_t any_hit = shader_group_none;
        std::uint32_t intersection = shader_group_none;
        /// whether a HIT group's geometry is TRIANGLES (the only kind this renderer traces); ignored by a
        /// general group. It exists because the API makes the caller state it, and this renderer's answer is
        /// the same at every site.
        bool triangles = true;
    };

    /// A shader AS binding backed by an existing descriptor heap range. No descriptor set is allocated.
    /// The backend translates the binding to its heap mapping; array elements are stride bytes apart.
    struct acceleration_structure_heap_binding {
        shader_stage stage = shader_stage::ray_generation;
        std::uint32_t descriptor_set = 0;
        std::uint32_t binding = 0;
        std::uint32_t byte_offset = 0;
        std::uint32_t array_stride = 0;
        std::uint32_t array_count = 1;
    };
    struct pipeline_desc {
        std::uint32_t struct_size = sizeof(pipeline_desc); ///< size of this structure as the CALLER compiled it
        std::span<image_format const> color_formats;       ///< attachment order; empty = depth-only
        image_format depth_format = image_format::unknown; ///< `unknown` = no depth attachment
        std::span<std::byte const> vertex_code;            ///< the FIRST stage's SPIR-V (vertex, or mesh)
        std::span<std::byte const> fragment_code;          ///< the fragment stage's SPIR-V
        shader_stage first_stage = shader_stage::vertex;   ///< `mesh` REPLACES the vertex stage
        std::uint32_t sample_count = 1u;                   ///< the render instance's MSAA level (1, 2, 4...)
        bool depth_test = true;                            ///< test + DYNAMIC write state (per-draw)
        float depth_bias_constant_factor = 0.0f;
        float depth_bias_slope_factor = 0.0f;
        float depth_bias_clamp = 0.0f;
        std::span<blend_mode const> blend_modes; ///< per color attachment; empty = all opaque
        depth_compare compare = depth_compare::less_or_equal;
        char const* debug_name = nullptr; ///< what the backend logs on refusal; not retained
        /**
         * THE COMPUTE SPELLING (APPENDED, guarded by `struct_size`, so an older caller keeps the graphics
         * meaning it compiled against).
         *
         * WHY A FIELD AND NOT A SECOND DESCRIPTOR: a compute pipeline is the SAME request with ONE stage and
         * no attachment state, and `first_stage == shader_stage::compute` is what selects it - the colour
         * formats, the depth format, the blend modes, the sample count and the depth test are then IGNORED,
         * exactly as `fragment_code` is ignored for a stage that has no fragment shader. The name says what
         * the bytes ARE rather than reusing `vertex_code` for a shader that is not a vertex stage: the two
         * graphics fields are named for their stages, and this one is named for its stage.
         *
         * THE ENTRY POINT IS NOT HERE, on purpose: every shader this renderer builds is entered at "main"
         * (the graphics path hardcodes the same name in `make_pipeline`), so a field for it would carry one
         * value forever. A backend that sees no entry-point field uses "main"; a caller with a differently
         * named entry point is not a case this engine has.
         *
         * THE HEAP FLAG IS THE BACKEND'S: a heap-native pipeline is created with a NULL layout (validation
         * refuses the alternative), and a create call in that shape must carry
         * `VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT` - a descriptor-heap capability of the backend, not a
         * caller decision, so the contract does not spell it.
         */
        std::span<std::byte const> compute_code;
        /**
         * THE RAY-TRACING SPELLING (APPENDED in the same batch, `struct_size`-guarded).
         *
         * NON-EMPTY `ray_tracing_stages` IS THE WHOLE SWITCH: a ray-tracing pipeline is the only kind built
         * from SEVERAL named entry points and a GROUP TABLE, so the presence of the stages is what says which
         * of the three paths `create_pipeline` takes (graphics, compute, ray tracing) - the colour, depth,
         * compute and blend fields are ignored, exactly as the compute spelling ignores the attachment state.
         *
         * WHAT IS NOT HERE: the SHADER BINDING TABLE. Its handles are per-pipeline data read back after
         * creation, and its regions are the CALLER's memory with the device's stride rules - both are the
         * caller's side of the boundary (see the pass that fills one), and a descriptor carrying them would be
         * promising a lifetime this contract cannot state.
         */
        std::span<ray_tracing_stage const> ray_tracing_stages;
        /// ONE GROUP PER SHADER BINDING TABLE REGION, in the caller's order; empty = not a ray-tracing pipeline.
        std::span<ray_tracing_group const> ray_tracing_groups;
        /// the pipeline's ray recursion depth (`maxPipelineRayRecursionDepth`); 1 = "a ray may hit once".
        std::uint32_t max_ray_recursion = 1u;
        /// Optional AS shader bindings into the heap. Appended and guarded by struct_size;
        /// older callers keep an empty list. Currently supported by ray-tracing pipelines.
        std::span<acceleration_structure_heap_binding const> acceleration_structure_bindings;
        texture_group_binding texture_group{};
        std::span<resource_group_binding const> resource_group;
    };

    /// The descriptors of the remaining factories. Opaque until S1 (see the banner).

    /// The remaining tier-1 objects: samplers, shaders, pipelines, swapchains, queries
    /// and command lists.
    ///
    /// Each one is a polymorphic handle and not much more yet: the operations the engine
    /// performs on them (bind, draw, dispatch, barrier, present) are exactly the S1
    /// design surface, held back by §6.4's "explicit barrier first, then shadow mode"
    /// decision. What they carry today is retention: destroying one through this base
    /// has to reach the backend's destructor, which is the property the boundary rests
    /// on.
    struct shader : object {

        shader() noexcept = default;
        virtual ~shader() noexcept = default;
        /// see `buffer::release()`
        virtual void release() noexcept = 0;
    };

    struct pipeline : object {

        pipeline() noexcept = default;
        virtual ~pipeline() noexcept = default;
        /// see `buffer::release()`
        virtual void release() noexcept = 0;
    };

} // namespace deren::promise::rhi
