module;
#include <cstdint>
export module deren.promise.rhi.synchronization;
// Scope-only vocabulary. No handles, virtual recording entry points or native masks.
export namespace deren::promise::rhi::synchronization {
    enum class error { invalid_argument,
                       unsupported };
    enum class stage : uint32_t { none = 0,
                                  vertex_shader = 1,
                                  fragment_shader = 2,
                                  compute_shader = 4,
                                  color_output = 8,
                                  depth_stencil = 16,
                                  transfer = 32,
                                  vertex_input = 64,
                                  draw_indirect = 128,
                                  task_shader = 256,
                                  mesh_shader = 512 };
    constexpr stage operator|(stage a, stage b) noexcept {
        return static_cast<stage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    enum class access : uint32_t { none = 0,
                                   read = 1,
                                   write = 2 };
    constexpr access operator|(access a, access b) noexcept {
        return static_cast<access>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    enum class aspect : uint32_t { color = 1,
                                   depth = 2,
                                   stencil = 4 };
    constexpr aspect operator|(aspect a, aspect b) noexcept {
        return static_cast<aspect>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    /// Abstract state semantics; backend layout spelling is deliberately independent.
    enum class image_layout { undefined,
                              general,
                              shader_read_only,
                              color_attachment,
                              depth_stencil_attachment,
                              depth_stencil_read_only,
                              transfer_source,
                              transfer_destination,
                              present };
    struct scope {
        stage stages = stage::none;
        access accesses = access::none;
    };
    enum class usage : uint32_t { sampled = 1,
                                  storage = 2,
                                  color_attachment = 4,
                                  depth_stencil_attachment = 8,
                                  depth_stencil_read = 16,
                                  transfer_source = 32,
                                  transfer_destination = 64,
                                  uniform_buffer = 128,
                                  vertex_buffer = 256,
                                  index_buffer = 512,
                                  indirect_buffer = 1024,
                                  present = 2048 };
    struct image_state {
        image_layout layout = image_layout::undefined;
        synchronization::scope scope = {};
    };
    struct image_range {
        uint32_t base_mip = 0, mip_count = 1, base_layer = 0, layer_count = 1;
        aspect aspects = aspect::color;
    };
    struct image_transition {
        image_state before, after;
        image_range range;
    };
    struct buffer_transition {
        scope before, after;
        uint64_t offset = 0, size = 0;
    };
} // namespace deren::promise::rhi::synchronization
