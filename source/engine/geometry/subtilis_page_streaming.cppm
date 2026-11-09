module;
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
export module deren.engine.subtilis.page_streaming;
export import deren.vstd;
export import deren.asset.subtilis.types;
export import deren.engine.subtilis.residency;

export namespace deren::engine::subtilis {
    using deren::asset::subtilis::page_entry;

    // M5c-1 keeps every page resident while it introduces the layouts and the request scheduler that
    // M5c-2 will bind to the cut. Nothing here touches a device: the arena layout is the address plan
    // for one asset's page payload, and the records below are the byte-stable GPU ABI that the
    // streaming cut will read. Keeping the plan device-free is what lets its invariants run in CI.
    class page_arena_layout {
        std::uint32_t alignment_ = 64;
        utility::vessel::vector<std::uint32_t> offsets_;
        std::uint64_t bytes_ = 0;
        page_arena_layout() = default;

    public:
        // Every page gets a naturally aligned, non-overlapping offset in one arena. The arena is
        // capped at 4 GiB because page offsets are 32-bit in the GPU page table.
        [[nodiscard]] static std::expected<page_arena_layout, residency_error>
        create(std::span<page_entry const> pages, std::uint32_t alignment = 64);
        [[nodiscard]] std::uint32_t alignment() const noexcept {
            return alignment_;
        }
        [[nodiscard]] std::uint64_t bytes() const noexcept {
            return bytes_;
        }
        [[nodiscard]] std::size_t page_count() const noexcept {
            return offsets_.size();
        }
        [[nodiscard]] std::optional<std::uint32_t> page_offset(std::uint32_t page) const noexcept {
            if (page >= offsets_.size()) {
                return std::nullopt;
            }
            return offsets_[page];
        }
    };

    enum class gpu_page_state : std::uint32_t {
        absent = 0,
        requested = 1,
        resident = 2,
    };
    /// One page-table row. `page` is the row's own index; the shader locates a section element by
    /// binary-searching (section, first). `offset` points into the single page arena.
    struct gpu_page_table_entry {
        std::uint32_t page = 0;
        std::uint32_t section = 0;
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        std::uint32_t offset = 0;
        std::uint32_t bytes = 0;
        std::uint32_t state = 0; // gpu_page_state
        std::uint32_t flags = 0; // page_flags
    };
    static_assert(sizeof(gpu_page_table_entry) == 32u);
    static_assert(offsetof(gpu_page_table_entry, section) == 4u);
    static_assert(offsetof(gpu_page_table_entry, offset) == 16u);
    static_assert(offsetof(gpu_page_table_entry, state) == 24u);
    static_assert(offsetof(gpu_page_table_entry, flags) == 28u);
    /// The cut's request lane writes this record; the host consumes it after the compute dispatch.
    struct gpu_page_request {
        std::uint32_t page = 0;
        std::uint32_t priority = 0;
        std::uint32_t item = 0;
        std::uint32_t frame = 0;
    };
    static_assert(sizeof(gpu_page_request) == 16u);
    /// Header at offset 0 of the request buffer, followed by `capacity` gpu_page_request records.
    struct gpu_page_request_header {
        std::uint32_t count = 0;
        std::uint32_t capacity = 0;
        std::uint32_t overflow = 0;
        std::uint32_t frame = 0;
    };
    static_assert(sizeof(gpu_page_request_header) == 16u);

    // Stable, dependency-ordered page table for M5c-2. Pages that are currently resident advertise
    // `resident`; all others advertise `absent`. The cut's request buffer is what promotes them.
    [[nodiscard]] std::expected<utility::vessel::vector<gpu_page_table_entry>, residency_error>
    make_gpu_page_table(std::span<page_entry const> pages, page_arena_layout const& layout, page_manager const& manager);

    /// Host-side image of the GPU request buffer. push() never reallocates and reports overflow in
    /// the header, so the shader-side append has a bounded target.
    class page_request_buffer {
        std::uint32_t capacity_ = 0;
        utility::vessel::vector<std::byte> storage_;

    public:
        explicit page_request_buffer(std::uint32_t capacity);
        [[nodiscard]] std::uint32_t capacity() const noexcept {
            return capacity_;
        }
        [[nodiscard]] std::span<std::byte const> bytes() const noexcept {
            return {storage_.data(), storage_.size()};
        }
        [[nodiscard]] gpu_page_request_header header() const noexcept;
        [[nodiscard]] std::optional<gpu_page_request> request(std::uint32_t index) const noexcept;
        [[nodiscard]] bool push(gpu_page_request const& request) noexcept;
        void clear(std::uint32_t frame = 0) noexcept;
    };

    struct page_transfer_request {
        std::uint64_t id = 0;
        std::uint32_t page = 0;
        std::uint32_t priority = 0;
        std::uint32_t item = 0;
        std::uint32_t frame = 0;
    };
    struct page_transfer {
        std::uint64_t id = 0;
        std::uint32_t page = 0;
        std::uint32_t bytes = 0;
    };
    struct streaming_stats {
        std::uint64_t requests = 0;
        std::uint64_t coalesced = 0;
        std::uint64_t starts = 0;
        std::uint64_t completions = 0;
        std::uint64_t failures = 0;
        std::uint32_t queued = 0;
    };
    /// Single-threaded async scheduler: request() only queues, begin() publishes the residency plan
    /// whose payload the renderer is about to copy, and complete() applies it after the transfer
    /// fence. At most one plan is in flight, so a stale plan can never race a second request.
    class page_streamer {
        page_manager manager_;
        std::uint32_t capacity_ = 0;
        utility::vessel::vector<page_transfer_request> queue_;
        std::optional<page_transfer_request> active_;
        std::optional<residency_plan> active_plan_;
        std::uint64_t next_id_ = 0;
        streaming_stats stats_;

    public:
        explicit page_streamer(page_manager manager, std::uint32_t capacity = 64);
        page_streamer(page_streamer const&) = delete ("page_streamer owns one in-flight plan; move it instead");
        page_streamer& operator=(page_streamer const&) = delete ("page_streamer owns one in-flight plan; move it instead");
        page_streamer(page_streamer&&) = default;
        page_streamer& operator=(page_streamer&&) = default;

        [[nodiscard]] std::expected<std::uint64_t, residency_error>
        request(std::uint32_t page, std::uint32_t priority = 0, std::uint32_t item = 0, std::uint32_t frame = 0);
        [[nodiscard]] std::expected<std::optional<page_transfer>, residency_error> begin();
        [[nodiscard]] std::expected<void, residency_error> complete();
        [[nodiscard]] bool pending(std::uint32_t page) const noexcept;
        [[nodiscard]] bool active() const noexcept {
            return active_.has_value();
        }
        [[nodiscard]] streaming_stats stats() const noexcept;
        [[nodiscard]] page_manager const& manager() const noexcept {
            return manager_;
        }
    };
} // namespace deren::engine::subtilis
