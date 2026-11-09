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
        page_manager(page_manager const&) = delete ("page_manager owns the resident table and its LRU state; move or share it instead");
        page_manager& operator=(page_manager const&) = delete ("page_manager owns the resident table and its LRU state; move or share it instead");
        page_manager(page_manager&&) = default;
        page_manager& operator=(page_manager&&) = default;

        [[nodiscard]] std::span<page_entry const> pages() const noexcept;
        [[nodiscard]] std::span<std::uint32_t const> dependencies() const noexcept;
        [[nodiscard]] residency_limits const& limits() const noexcept;
        [[nodiscard]] residency_stats stats() const noexcept;
        [[nodiscard]] bool resident(std::uint32_t page) const noexcept;

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
