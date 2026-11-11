/**
 * @file source/engine/geometry/subtilis_cut.cppm
 * @brief The per-frame GPU cut payload: the immutable DAG input is addressed by the item record, and the
 *        compute shader writes the selected meshlet run and its mesh-task command.
 *
 * The item record is deliberately separate from the 48-byte meshlet ABI: it is a control record, not a
 * meshlet, and the render-side visibility table must not grow to carry addresses and counts.
 */
module;

#include <cstddef>

export module deren.engine.subtilis.cut;

export import deren.vstd;

export namespace deren::engine::subtilis {

    /// The compute shader's workgroup width. One item is one persistent workgroup; each lane holds one
    /// 32-bit word of the coarsening bitset, and cluster emission compacts in 64 contiguous chunks.
    inline constexpr std::uint32_t cut_group_size = 64u;

    /// One item's maximum group count. The copy of the coarsening bits lives in the shader's groupshared
    /// memory (64 words), so the host refuses larger DAGs and keeps the CPU fallback for them.
    inline constexpr std::uint32_t cut_max_groups = 2048u;

    /// Per-frame item slots, matching `visibility::id_item_capacity`.
    inline constexpr std::uint32_t cut_item_capacity = 4096u;

    /// The immutable per-frame camera header. `view` and `projection` are the renderer's column-major arrays,
    /// exactly the bytes `make_item_view` copies.
    struct cut_frame {
        float view[16] = {};
        float projection[16] = {};
        std::uint32_t width = 0, height = 0;
        float near_plane = .1f, target_error_px = 1.0f;
        std::uint32_t kind = 0; // 1 = perspective, 2 = orthographic
        std::uint32_t item_offset = 0u, item_stride = 0u, item_capacity = 0u;
    };
    static_assert(sizeof(cut_frame) == 160u);

    /// One cut item. Every input and output is a raw device address: the mesh path is already BDA-based, so
    /// adding descriptors here would only add a hot-path bind to maintain.
    struct cut_item {
        float model[16] = {};
        std::uint64_t groups = 0, references = 0, clusters = 0;
        std::uint64_t meshlets = 0, command = 0;
        std::uint32_t group_count = 0, cluster_count = 0;
        std::uint32_t output_base = 0, output_capacity = 0;
        std::uint32_t command_index = 0, flags = 0;
        /// The streamed-index-page control block. `page_arena` and `page_table` address the uploaded page
        /// cover; `page_requests` is the GPU-written request buffer the host drains next frame. Zero means
        /// this geometry has no page cover and the cut keeps its old direct-index path.
        std::uint64_t page_arena = 0, page_table = 0, page_requests = 0;
        std::uint32_t page_count = 0;
        std::uint32_t request_capacity = 0;
        std::uint32_t request_frame = 0;
        /// The number of real LOD0 records preloaded into this item's output slot. The shader falls back to
        /// exactly this many records when any selected cluster needs an index page that is not resident.
        std::uint32_t lod0_count = 0;
    };
    static_assert(sizeof(cut_item) == 168u);
    static_assert(offsetof(cut_item, groups) == 64u);
    static_assert(offsetof(cut_item, command) == 96u);
    static_assert(offsetof(cut_item, group_count) == 104u);
    static_assert(offsetof(cut_item, page_arena) == 128u);
    static_assert(offsetof(cut_item, page_count) == 152u);
    static_assert(offsetof(cut_item, lod0_count) == 164u);

    /// Bytes in one frame's slice: the header followed by the item array.
    [[nodiscard]] constexpr std::uint64_t cut_frame_bytes() noexcept {
        return sizeof(cut_frame) + static_cast<std::uint64_t>(cut_item_capacity) * sizeof(cut_item);
    }
} // namespace deren::engine::subtilis
