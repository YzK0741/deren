module;
#include <cstdint>
#include <expected>
#include <span>
export module deren.engine.subtilis.residency;
export import deren.vstd;
export import deren.asset.subtilis.types;

export namespace deren::engine::subtilis {
    using deren::asset::subtilis::page_entry;

    // M5b keeps residency synchronous and single-asset: a request either returns a fully installed
    // dependency closure or leaves the resident set untouched. The manager exists to prove the page
    // table and its bookkeeping; no GPU upload or cut request is attached yet.
    struct residency_limits {
        std::uint64_t max_resident_bytes = 64ull << 20;
        // Zero means byte-budget only. The device page arena is a fixed slot set, so a byte budget
        // alone would admit more tiny pages than there are slots.
        std::uint32_t max_resident_pages = 0;
    };
    enum class residency_error { invalid_page_table,
                                 invalid_page,
                                 capacity,
                                 pinned,
                                 dependency,
    };
    struct residency_stats {
        std::uint64_t resident_bytes = 0;
        std::uint32_t resident_pages = 0;
        std::uint64_t installs = 0;
        std::uint64_t evictions = 0;
    };
    /// A non-mutating residency decision. Installs are in dependency order; evictions are the pages
    /// whose bytes must be released before the installs fit. Applying a stale plan is rejected
    /// without changing the resident set.
    struct residency_plan {
        std::uint32_t target = 0;
        utility::vessel::vector<std::uint32_t> installs;
        utility::vessel::vector<std::uint32_t> evictions;
        std::uint64_t installed_bytes = 0;
        std::uint64_t evicted_bytes = 0;
    };
    class page_manager;
    std::expected<page_manager, residency_error> make_page_manager(std::span<page_entry const> pages, std::span<std::uint32_t const> dependencies, residency_limits limits = {});
    class page_manager {
        utility::vessel::vector<page_entry> pages_;
        utility::vessel::vector<std::uint32_t> dependencies_;
        utility::vessel::vector<std::uint8_t> resident_;
        utility::vessel::vector<std::uint64_t> last_use_;
        residency_limits limits_;
        std::uint64_t resident_bytes_ = 0;
        std::uint64_t clock_ = 0;
        std::uint64_t installs_ = 0;
        std::uint64_t evictions_ = 0;
        std::uint32_t resident_pages_ = 0;

        page_manager(std::span<page_entry const> pages, std::span<std::uint32_t const> dependencies, residency_limits limits);
        [[nodiscard]] std::expected<void, residency_error> initialize();
        [[nodiscard]] std::expected<void, residency_error> collect(std::uint32_t page, utility::vessel::vector<std::uint8_t>& state, utility::vessel::vector<std::uint32_t>& order) const;
        [[nodiscard]] bool pinned(std::uint32_t page) const noexcept;
        [[nodiscard]] bool depends_on(std::uint32_t dependent, std::uint32_t dependency) const noexcept;
        void touch(std::uint32_t page) noexcept;

        friend std::expected<page_manager, residency_error> make_page_manager(std::span<page_entry const>, std::span<std::uint32_t const>, residency_limits);

    public:
        page_manager(page_manager const&) = delete ("page_manager stores the resident table and its LRU state; move or share it instead");
        page_manager& operator=(page_manager const&) = delete ("page_manager stores the resident table and its LRU state; move or share it instead");
        page_manager(page_manager&&) = default;
        page_manager& operator=(page_manager&&) = default;

        [[nodiscard]] std::span<page_entry const> pages() const noexcept;
        [[nodiscard]] std::span<std::uint32_t const> dependencies() const noexcept;
        [[nodiscard]] residency_limits const& limits() const noexcept;
        [[nodiscard]] residency_stats stats() const noexcept;
        [[nodiscard]] bool resident(std::uint32_t page) const noexcept;

        // Computes the dependency closure and LRU victims for `page` without touching the resident
        // set. The plan can be applied by a later async completion; a plan computed against an older
        // resident set is rejected by apply().
        [[nodiscard]] std::expected<residency_plan, residency_error> plan_request(std::uint32_t page) const;
        /// Union the targets' dependency closures; duplicate/shared pages install once.
        [[nodiscard]] std::expected<residency_plan, residency_error> plan_requests(std::span<std::uint32_t const> targets) const;
        // Applies a plan atomically: validates page identities, the dependency closure, eviction
        // safety and the byte budget before changing resident state. Returns invalid_page on a stale
        // or malformed plan and capacity when the plan no longer fits.
        [[nodiscard]] std::expected<void, residency_error> apply(residency_plan&& plan);
        // Installs `page` and every transitive page it needs, evicting least-recently-used pages that
        // no remaining resident page depends on. Returns the number of newly installed pages.
        [[nodiscard]] std::expected<std::uint32_t, residency_error> request(std::uint32_t page);
        // Installs the whole table in dependency order. M5b loads every page before first use, so a
        // capacity failure here leaves the resident set untouched instead of exposing a partial asset.
        [[nodiscard]] std::expected<std::uint32_t, residency_error> request_all();
        // Explicit eviction is only legal for an unpinned page that no resident page depends on.
        [[nodiscard]] std::expected<void, residency_error> evict(std::uint32_t page);
    };
} // namespace deren::engine::subtilis
