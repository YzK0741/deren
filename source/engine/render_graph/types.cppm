module;
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
export module deren.engine.render_graph:types;
export import deren.utility.shared;
export namespace deren::engine::render_graph {
    inline constexpr uint32_t invalid_index = std::numeric_limits<uint32_t>::max();
    class builder;
    struct resource_access;
    template <typename Tag>
    class basic_ref {
        friend class builder;
        friend struct resource_access;
        uint64_t incarnation_ = 0;
        uint32_t index_ = invalid_index;
        basic_ref(uint64_t incarnation, uint32_t index) noexcept
            : incarnation_(incarnation)
            , index_(index) {
        }

    public:
        basic_ref() noexcept = default;
        uint32_t index() const noexcept {
            return index_;
        }
        bool operator==(basic_ref const&) const noexcept = default;
    };
    struct texture_tag;
    struct buffer_tag;
    struct pass_tag;
    using texture_ref = basic_ref<texture_tag>;
    using buffer_ref = basic_ref<buffer_tag>;
    using pass_ref = basic_ref<pass_tag>;
    struct resource_identity {
        uint64_t device = 0, object = 0;
        bool operator==(resource_identity const&) const noexcept = default;
    };
    enum class resource_kind { texture,
                               buffer };
    enum class initial_contents { undefined,
                                  defined };
    enum class content_access { read,
                                discard_write,
                                preserve_write,
                                read_write,
                                invalidate };
    enum class texture_aspect : uint32_t { none = 0,
                                           color = 1,
                                           depth = 2,
                                           stencil = 4,
                                           all = 0xffffffffU };
    constexpr texture_aspect operator|(texture_aspect a, texture_aspect b) noexcept {
        return static_cast<texture_aspect>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    inline constexpr uint32_t remaining_subresources = std::numeric_limits<uint32_t>::max();
    inline constexpr uint64_t remaining_bytes = std::numeric_limits<uint64_t>::max();
    struct texture_shape {
        uint32_t mip_levels = 1, array_layers = 1;
        texture_aspect aspects = texture_aspect::color;
        bool operator==(texture_shape const&) const noexcept = default;
    };
    struct buffer_shape {
        uint64_t byte_size = 1;
        bool operator==(buffer_shape const&) const noexcept = default;
    };
    using resource_shape = std::variant<texture_shape, buffer_shape>;
    struct texture_range {
        uint32_t base_mip = 0, mip_count = remaining_subresources;
        uint32_t base_layer = 0, layer_count = remaining_subresources;
        texture_aspect aspects = texture_aspect::all;
        bool operator==(texture_range const&) const noexcept = default;
    };
    struct buffer_range {
        uint64_t offset = 0, size = remaining_bytes;
        bool operator==(buffer_range const&) const noexcept = default;
    };
    using resource_range = std::variant<texture_range, buffer_range>;
    enum class pipeline_stage : uint32_t { none = 0,
                                           vertex_shader = 1,
                                           fragment_shader = 2,
                                           compute_shader = 4,
                                           color_output = 8,
                                           depth_stencil = 16,
                                           transfer = 32,
                                           vertex_input = 64,
                                           draw_indirect = 128,
                                           task_shader = 256,
                                           mesh_shader = 512,
                                           /// RT stage 1: a traceRays LAUNCH's stages. A ray query inside a compute
                                           /// shader names `compute_shader`; this value names the ray-tracing
                                           /// pipeline stages, which is what the RT shadow pass writes its
                                           /// visibility image from.
                                           ray_tracing = 1024 };
    constexpr pipeline_stage operator|(pipeline_stage a, pipeline_stage b) noexcept {
        return static_cast<pipeline_stage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    constexpr pipeline_stage operator&(pipeline_stage a, pipeline_stage b) noexcept {
        return static_cast<pipeline_stage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }
    enum class memory_access : uint32_t { none = 0,
                                          read = 1,
                                          write = 2 };
    constexpr memory_access operator|(memory_access a, memory_access b) noexcept {
        return static_cast<memory_access>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    enum class resource_usage { unspecified,
                                sampled,
                                storage,
                                color_attachment,
                                depth_stencil_attachment,
                                depth_stencil_read,
                                transfer_source,
                                transfer_destination,
                                uniform_buffer,
                                vertex_buffer,
                                index_buffer,
                                indirect_buffer,
                                present };
    // Abstract usage/layout semantics. Backend lowering may use different native layouts.
    enum class resource_layout { undefined,
                                 general,
                                 shader_read_only,
                                 color_attachment,
                                 depth_stencil_attachment,
                                 depth_stencil_read_only,
                                 transfer_source,
                                 transfer_destination,
                                 present };
    struct access_scope {
        resource_usage usage = resource_usage::unspecified;
        pipeline_stage stages = pipeline_stage::none;
        bool operator==(access_scope const&) const noexcept = default;
    };
    struct resource_state {
        // Frontend handoff state, never an assertion about a native VkImageLayout value.
        resource_layout layout = resource_layout::undefined;
        pipeline_stage stages = pipeline_stage::none;
        memory_access access = memory_access::none;
        bool operator==(resource_state const&) const noexcept = default;
    };
    struct resource_access {
        friend class builder;

    private:
        uint64_t incarnation_;
        uint32_t resource_;
        resource_kind kind_;
        content_access mode_;
        resource_range range_;
        access_scope scope_;

    public:
        resource_access(texture_ref ref, content_access mode, texture_range range = {}, access_scope scope = {}) noexcept
            : incarnation_(ref.incarnation_)
            , resource_(ref.index_)
            , kind_(resource_kind::texture)
            , mode_(mode)
            , range_(range)
            , scope_(scope) {
        }
        resource_access(buffer_ref ref, content_access mode, buffer_range range = {}, access_scope scope = {}) noexcept
            : incarnation_(ref.incarnation_)
            , resource_(ref.index_)
            , kind_(resource_kind::buffer)
            , mode_(mode)
            , range_(range)
            , scope_(scope) {
        }
    };
    struct pass_options {
        bool side_effect = false;
    };
    struct compile_options {
        bool synchronization = false;
    };
    enum class error_code { invalid_identity,
                            foreign_ref,
                            invalid_ref,
                            conflicting_import,
                            conflicting_access,
                            undefined_read,
                            undefined_export,
                            cycle,
                            frozen_builder,
                            capacity_exceeded,
                            invalid_shape,
                            invalid_range,
                            invalid_scope,
                            missing_scope,
                            missing_initial_state,
                            unsupported_access };
    struct graph_error {
        error_code code;
        uint32_t pass = invalid_index, resource = invalid_index;
        deren::utility::vessel::string pass_name, resource_name;
        std::optional<resource_range> range = std::nullopt;
    };
    struct resolved_access {
        uint32_t resource;
        content_access mode;
        resource_range range = texture_range{};
        access_scope scope = {};
    };
    struct range_contents {
        resource_range range;
        uint32_t producer = invalid_index;
        uint64_t version = 0;
        bool defined = false;
    };
    struct resource_record {
        resource_identity identity;
        resource_kind kind;
        initial_contents initial;
        deren::utility::vessel::string name;
        uint32_t last_producer = invalid_index;
        uint64_t last_version = 0;
        bool defined = false;
        resource_shape shape = texture_shape{};
        deren::utility::vessel::vector<range_contents> final_ranges = {};
        std::optional<resource_state> initial_state = std::nullopt;
    };
    /// Owns a const parameter value; copying a plan shares the same snapshot.
    class parameter_snapshot {
        friend class builder;
        std::shared_ptr<void const> value_;
        void const* type_ = nullptr;
        template <typename T>
        inline static unsigned char type_key = 0;
        template <typename T>
        static parameter_snapshot make(T value) {
            parameter_snapshot result;
            result.value_ = std::allocate_shared<T const>(deren::utility::memory::allocator<T const>{}, std::move(value));
            result.type_ = &type_key<T>;
            return result;
        }

    public:
        parameter_snapshot() noexcept = default;
        [[nodiscard]] explicit operator bool() const noexcept {
            return static_cast<bool>(value_);
        }
        template <typename T>
            requires(std::is_object_v<T> && !std::is_array_v<T>)
        [[nodiscard]] T const* get() const noexcept {
            return type_ == &type_key<std::remove_cv_t<T>> ? static_cast<T const*>(value_.get()) : nullptr;
        }
    };
    struct pass_record {
        deren::utility::vessel::string name;
        deren::utility::vessel::vector<resolved_access> accesses;
        bool side_effect = false, live = false;
        parameter_snapshot parameters = {};
    };
    enum class dependency_reason { value,
                                   raw,
                                   war,
                                   waw,
                                   explicit_order,
                                   state_transition };
    struct dependency {
        uint32_t from, to, resource;
        dependency_reason reason;
        std::optional<resource_range> range = std::nullopt;
    };
    struct content_read {
        uint32_t pass, resource, producer;
        uint64_t version;
        bool defined;
        resource_range range = texture_range{};
    };
    struct resource_export {
        uint32_t resource;
        resource_range range;
        bool operator==(resource_export const&) const noexcept = default;
    };
    struct resource_barrier {
        uint32_t resource, before_pass;
        resource_range range;
        resource_state before, after;
        bool layout_transition = false, memory_dependency = false;
    };
    struct final_resource_state {
        uint32_t resource;
        resource_range range;
        resource_state state;
    };
    struct compiled_graph {
        deren::utility::vessel::vector<resource_record> resources;
        deren::utility::vessel::vector<pass_record> passes;
        deren::utility::vessel::vector<dependency> dependencies;
        deren::utility::vessel::vector<content_read> reads;
        deren::utility::vessel::vector<uint32_t> execution_order;
        deren::utility::vessel::vector<resource_export> outputs = {};
        bool synchronization_planned = false;
        deren::utility::vessel::vector<resource_barrier> barriers = {};
        deren::utility::vessel::vector<final_resource_state> final_states = {};
    };
} // namespace deren::engine::render_graph
