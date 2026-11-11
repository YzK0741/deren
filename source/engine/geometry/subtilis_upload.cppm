module;
#include <cstddef>
#include <cstdint>
export module deren.engine.subtilis.upload;
export import deren.vstd;
import deren.engine.subtilis.builder;
import deren.engine.meshlet;
import deren.engine.subtilis.page_streaming;
import deren.utility.shared;
import deren.promise.rhi;
import deren.engine.render_resource.instances;

export namespace deren::engine::subtilis {
    namespace upload_rhi = deren::promise::rhi;
    /// Conservative object-space maximum edge; malformed/nonfinite geometry returns infinity.
    [[nodiscard]] double maximum_triangle_edge(meshlet const&, std::span<std::uint8_t const> vertices,
                                               std::span<std::uint32_t const> indices, std::uint32_t stride) noexcept;
    struct triangle_edge_record {
        std::uint32_t first_index, index_count;
        std::int32_t base_vertex;
        double maximum_edge;
    };
    enum class geometry_upload_mode { automatic,
                                      synchronous,
                                      batched,
    };
    /// One frame's page-install pass over the cut's request buffer. `requests` counts entries the
    /// completed slot wrote, `installed` counts completed pages, `submitted` counts queued pages,
    /// and `bytes` charges only newly submitted copies. Pending demand includes in-flight pages.
    /// `in_flight_bytes` counts reserved upload payload, not resident arena bytes. `abandoned`
    /// counts pages whose upload failed repeatedly and were returned to absent for a new request.
    /// `evicted` counts pages the residency manager chose as victims for this install.
    /// `page_misses` counts cut items that fell back to LOD0 because an index page was absent.
    struct page_install_stats {
        upload_rhi::error result = upload_rhi::error::ok;
        std::uint32_t requests = 0;
        std::uint32_t installed = 0;
        std::uint32_t submitted = 0;
        std::uint32_t in_flight = 0;
        std::uint32_t pending = 0;
        std::uint32_t overflow = 0;
        std::uint32_t abandoned = 0;
        std::uint32_t evicted = 0;
        std::uint32_t page_misses = 0;
        std::uint64_t bytes = 0;
        std::uint64_t in_flight_bytes = 0;
    };
    /// Immutable GPU cut input: one record per DAG group. Kept separate from the 48-byte
    /// meshlet ABI so the render-side record can never grow into the meshlet contract.
    struct cut_group_record {
        std::uint32_t fine_first = 0, fine_count = 0, coarse_first = 0, coarse_count = 0;
        float center_x = 0, center_y = 0, center_z = 0, radius = 0;
        float error = 0;
    };
    static_assert(sizeof(cut_group_record) == 36u);
    static_assert(offsetof(cut_group_record, fine_first) == 0u);
    static_assert(offsetof(cut_group_record, coarse_count) == 12u);
    static_assert(offsetof(cut_group_record, center_x) == 16u);
    static_assert(offsetof(cut_group_record, error) == 32u);
    /// One DAG cluster: the exact meshlet record the visibility pass consumes plus the
    /// producer/consumer group ids used by the cut predicate.
    struct cut_cluster_record {
        meshlet record;
        std::uint32_t producer = 0, consumer = 0;
    };
    static_assert(sizeof(cut_cluster_record) == 56u);
    class uploaded_geometry;
    struct page_state;
    /// The source must remain immutable through every alias after publication;
    /// do not move-assign or modify it through a second, mutable shared handle.
    /// Returns only after both GPU buffers are ready. `resident_page_bytes` caps the device slot
    /// arena for streaming pages (default 64 MiB). Automatic selects a single transfer batch when
    /// the device, submission and scoped synchronization are available; otherwise it uses
    /// synchronous creation. Forced batched mode refuses missing capabilities; failures are propagated.
    /// Separate batched uploads do not share the backend's initial-content cache: reuse the returned uploaded_geometry.
    /// @details The feature overview, the measured numbers and the document index are in @ref md_docs_2subtilis.
    /// One device-wide pool of fixed-size subtilis page slots. Passing the same pool to every
    /// upload makes `resident_page_bytes` a real global budget instead of a per-source allowance.
    class page_arena_pool {
        struct state;
        std::shared_ptr<state> state_;

        explicit page_arena_pool(std::shared_ptr<state> state) noexcept;

    public:
        static constexpr std::uint32_t slot_bytes = 64u << 10;
        /// A page-table entry stores a 32-bit byte offset, so an arena may not contain a slot whose
        /// offset would wrap. The last addressable slot starts at UINT32_MAX / 64 KiB * 64 KiB,
        /// making 65536 slots and exactly 4 GiB the architectural maximum.
        static constexpr std::uint64_t max_slots = 0xFFFFFFFFull / slot_bytes + 1u;
        static constexpr std::uint64_t max_bytes = max_slots * slot_bytes;

        [[nodiscard]] static std::expected<std::shared_ptr<page_arena_pool>, upload_rhi::error>
        create(upload_rhi::shared_object<upload_rhi::api_core> const& device, std::uint64_t bytes);
        ~page_arena_pool();
        page_arena_pool(page_arena_pool const&) = delete ("page_arena_pool owns one shared GPU arena; share the handle instead");
        page_arena_pool& operator=(page_arena_pool const&) = delete ("page_arena_pool owns one shared GPU arena; share the handle instead");

        [[nodiscard]] std::uint32_t capacity_slots() const noexcept;
        [[nodiscard]] std::shared_ptr<render_resource::instances::generation const> const& arena() const noexcept;
        using slot_evict_fn = bool (*)(void*, std::uint32_t);

        /// Removes `count` free slots from the pool. When the pool is full, owners of unreferenced
        /// slots are asked to evict one page; `skip_owner` keeps the requesting source out of that
        /// search because its residency plan is already in flight.
        [[nodiscard]] bool acquire(std::uint32_t count, deren::utility::vessel::vector<std::uint32_t>& out, void const* skip_owner = nullptr, std::uint32_t* evicted = nullptr);
        void set_owner(std::uint32_t slot, void* context, slot_evict_fn evict);
        void clear_owner(std::uint32_t slot);
        /// Returns a slot to the pool. Releasing an already-free slot is a no-op: a page can be
        /// returned by both its owner and a failed transaction without duplicating the free entry.
        void release(std::uint32_t slot);
        void release(std::span<std::uint32_t const> slots);
        void retain(std::uint32_t slot);
        void drop(std::uint32_t slot);
        /// Copies initial bytes into a slot. The arena is host-visible/coherent.
        [[nodiscard]] bool write(std::uint32_t slot, std::span<std::byte const> bytes);
    };

    [[nodiscard]] std::expected<std::shared_ptr<uploaded_geometry const>, upload_rhi::error>
    upload_geometry(upload_rhi::shared_object<upload_rhi::api_core> const& device,
                    std::shared_ptr<built_geometry const> source,
                    upload_rhi::buffer_flags flags = upload_rhi::to_bits(upload_rhi::buffer_flag::device_address),
                    geometry_upload_mode mode = geometry_upload_mode::automatic,
                    std::uint64_t resident_page_bytes = 64ull << 20,
                    std::shared_ptr<page_arena_pool> arena_pool = {});
    /// One immutable CPU hierarchy and its complete GPU payload. Buffers reachable through it
    /// are valid while this generation, or a view that shares it, still exists.
    class uploaded_geometry {
        struct construction_key {
            explicit construction_key() = default;
        };
        std::shared_ptr<void const> resources_;
        std::shared_ptr<built_geometry const> source_;
        deren::utility::vessel::vector<triangle_edge_record> triangle_edges_;
        std::shared_ptr<render_resource::instances::generation const> vertices_, indices_;
        std::shared_ptr<render_resource::instances::generation const> cut_groups_, cut_references_, cut_clusters_;
        std::shared_ptr<render_resource::instances::generation const> page_arena_, page_table_, page_requests_;
        std::uint32_t cut_group_count_ = 0, cut_reference_count_ = 0, cut_cluster_count_ = 0;
        std::uint32_t page_count_ = 0, page_request_capacity_ = 0;
        std::shared_ptr<page_state> page_state_;
        friend std::expected<std::shared_ptr<uploaded_geometry const>, upload_rhi::error>
        upload_geometry(upload_rhi::shared_object<upload_rhi::api_core> const&, std::shared_ptr<built_geometry const>, upload_rhi::buffer_flags,
                        geometry_upload_mode, std::uint64_t, std::shared_ptr<page_arena_pool>);

    public:
        uploaded_geometry(construction_key /*unused*/, std::shared_ptr<void const> resources, std::shared_ptr<built_geometry const> source,
                          std::shared_ptr<render_resource::instances::generation const> vertices,
                          std::shared_ptr<render_resource::instances::generation const> indices,
                          std::shared_ptr<render_resource::instances::generation const> cut_groups,
                          std::shared_ptr<render_resource::instances::generation const> cut_references,
                          std::shared_ptr<render_resource::instances::generation const> cut_clusters, std::uint32_t cut_group_count,
                          std::uint32_t cut_reference_count, std::uint32_t cut_cluster_count,
                          std::shared_ptr<render_resource::instances::generation const> page_arena,
                          std::shared_ptr<render_resource::instances::generation const> page_table,
                          std::shared_ptr<render_resource::instances::generation const> page_requests,
                          std::uint32_t page_count, std::uint32_t page_request_capacity,
                          deren::utility::vessel::vector<triangle_edge_record> edges, std::shared_ptr<page_state> page_state)
            : resources_(std::move(resources))
            , source_(std::move(source))
            , triangle_edges_(std::move(edges))
            , vertices_(std::move(vertices))
            , indices_(std::move(indices))
            , cut_groups_(std::move(cut_groups))
            , cut_references_(std::move(cut_references))
            , cut_clusters_(std::move(cut_clusters))
            , page_arena_(std::move(page_arena))
            , page_table_(std::move(page_table))
            , page_requests_(std::move(page_requests))
            , cut_group_count_(cut_group_count)
            , cut_reference_count_(cut_reference_count)
            , cut_cluster_count_(cut_cluster_count)
            , page_count_(page_count)
            , page_request_capacity_(page_request_capacity)
            , page_state_(std::move(page_state)) {
        }

        uploaded_geometry(uploaded_geometry const&) = delete ("uploaded_geometry is non-copyable: pass it by reference or construct a new instance");
        uploaded_geometry& operator=(uploaded_geometry const&) = delete ("uploaded_geometry is non-copyable: modify the existing instance instead");
        [[nodiscard]] double triangle_edge(meshlet const& record) const noexcept;
        [[nodiscard]] built_geometry const& source() const noexcept {
            return *source_;
        }
        [[nodiscard]] std::shared_ptr<built_geometry const> const& source_handle() const noexcept {
            return source_;
        }
        [[nodiscard]] upload_rhi::buffer* vertices() const noexcept {
            return vertices_->objects[0].buffer.get();
        }
        [[nodiscard]] upload_rhi::buffer* indices() const noexcept {
            return indices_->objects[0].buffer.get();
        }
        /// Null when the source has no DAG payload (a plain LOD0 mesh).
        [[nodiscard]] upload_rhi::buffer* cut_groups() const noexcept {
            return cut_groups_ ? cut_groups_->objects[0].buffer.get() : nullptr;
        }
        [[nodiscard]] upload_rhi::buffer* cut_references() const noexcept {
            return cut_references_ ? cut_references_->objects[0].buffer.get() : nullptr;
        }
        [[nodiscard]] upload_rhi::buffer* cut_clusters() const noexcept {
            return cut_clusters_ ? cut_clusters_->objects[0].buffer.get() : nullptr;
        }
        [[nodiscard]] std::uint32_t cut_group_count() const noexcept {
            return cut_group_count_;
        }
        [[nodiscard]] std::uint32_t cut_reference_count() const noexcept {
            return cut_reference_count_;
        }
        [[nodiscard]] std::uint32_t cut_cluster_count() const noexcept {
            return cut_cluster_count_;
        }
        /// Null when the source has no streaming pages (a locally built LOD0 mesh or an imported
        /// version-1.0 payload). The arena bytes use the encoded page-record layout, not engine structs.
        [[nodiscard]] upload_rhi::buffer* page_arena() const noexcept {
            return page_arena_ ? page_arena_->objects[0].buffer.get() : nullptr;
        }
        [[nodiscard]] upload_rhi::buffer* page_table() const noexcept {
            return page_table_ ? page_table_->objects[0].buffer.get() : nullptr;
        }
        [[nodiscard]] upload_rhi::buffer* page_requests() const noexcept {
            return page_requests_ ? page_requests_->objects[0].buffer.get() : nullptr;
        }
        /// Configure before any snapshot is published. Growth preserves slot-zero buffers.
        [[nodiscard]] upload_rhi::error configure_page_frames(std::uint32_t count) const;
        [[nodiscard]] upload_rhi::buffer* page_table(std::uint32_t slot) const noexcept;
        [[nodiscard]] upload_rhi::buffer* page_requests(std::uint32_t slot) const noexcept;
        /// Caller must have completed this slot's GPU submission, or abandoned its recording.
        /// Collect requests, release references and reopen this slot for preparation.
        [[nodiscard]] upload_rhi::error complete_page_frame(std::uint32_t slot) const;
        /// Freeze a snapshot and protect every arena slot it can access until completion.
        [[nodiscard]] upload_rhi::error publish_page_frame(std::uint32_t slot) const;
        /// Poll only. Successful completion updates canonical residency, never a live snapshot.
        [[nodiscard]] page_install_stats collect_page_uploads() const;
        [[nodiscard]] bool has_pending_page_upload() const noexcept;
        [[nodiscard]] std::uint32_t page_count() const noexcept {
            return page_count_;
        }
        [[nodiscard]] std::uint32_t page_request_capacity() const noexcept {
            return page_request_capacity_;
        }
        /// Prepare a completed, unpublished slot under a byte budget. Pending requests
        /// survive in a shared CPU backlog. Copies submit without waiting; only unreferenced
        /// physical slots can be reused. Completion updates future snapshots only.
        /// `max_install_pages` bounds how many explicitly requested pages this call admits;
        /// dependency pages may still be added by the residency plan. Call publish_page_frame
        /// before recording GPU use.
        [[nodiscard]] page_install_stats install_requested_pages(std::uint64_t budget_bytes, std::uint32_t sequence = 0,
                                                                 std::uint32_t slot = 0,
                                                                 std::uint32_t max_install_pages = 0xFFFFFFFFu) const noexcept;
    };
} // namespace deren::engine::subtilis
