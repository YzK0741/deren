module;

module deren.engine.subtilis.residency;
import deren.vstd;

namespace deren::engine::subtilis {
    namespace {
        constexpr std::uint32_t no_page = 0xffffffffu;
        constexpr std::uint16_t resident_flag = 1u << 0;
    } // namespace

    page_manager::page_manager(std::span<page_entry const> pages, std::span<std::uint32_t const> dependencies, residency_limits limits)
        : pages_(pages.begin(), pages.end())
        , dependencies_(dependencies.begin(), dependencies.end())
        , limits_(limits) {
        resident_.resize(pages_.size(), std::uint8_t{0});
        last_use_.resize(pages_.size(), std::uint64_t{0});
    }

    std::expected<void, residency_error> page_manager::initialize() {
        for (std::uint32_t i = 0; i < pages_.size(); ++i) {
            auto const& page = pages_[i];
            if (!page.count || !page.bytes || (page.flags & ~resident_flag) || page.dependency_first > dependencies_.size() || page.dependency_count > dependencies_.size() - page.dependency_first) {
                return std::unexpected(residency_error::invalid_page_table);
            }
            for (std::uint32_t d = page.dependency_first; d < page.dependency_first + page.dependency_count; ++d) {
                auto const dependency = dependencies_[d];
                if (dependency >= pages_.size() || dependency == i) {
                    return std::unexpected(residency_error::invalid_page_table);
                }
                if (pinned(i) && !pinned(dependency)) {
                    return std::unexpected(residency_error::invalid_page_table);
                }
            }
        }
        utility::vessel::vector<std::uint8_t> state(pages_.size(), std::uint8_t{0});
        utility::vessel::vector<std::uint32_t> order;
        order.reserve(pages_.size());
        for (std::uint32_t i = 0; i < pages_.size(); ++i) {
            if (auto collected = collect(i, state, order); !collected) {
                return collected;
            }
        }
        std::uint64_t pinned_bytes = 0;
        for (std::uint32_t i = 0; i < pages_.size(); ++i) {
            if (pinned(i)) {
                pinned_bytes += pages_[i].bytes;
            }
        }
        std::uint32_t pinned_pages = 0;
        for (std::uint32_t i = 0; i < pages_.size(); ++i) {
            pinned_pages += pinned(i) ? 1u : 0u;
        }
        if (pinned_bytes > limits_.max_resident_bytes ||
            (limits_.max_resident_pages != 0u && pinned_pages > limits_.max_resident_pages)) {
            return std::unexpected(residency_error::capacity);
        }
        for (std::uint32_t i = 0; i < pages_.size(); ++i) {
            if (pinned(i)) {
                resident_[i] = 1;
                ++resident_pages_;
                resident_bytes_ += pages_[i].bytes;
                touch(i);
            }
        }
        return {};
    }

    std::expected<void, residency_error> page_manager::collect(std::uint32_t page, utility::vessel::vector<std::uint8_t>& state, utility::vessel::vector<std::uint32_t>& order) const {
        if (state[page] == 1) {
            return std::unexpected(residency_error::invalid_page_table);
        }
        if (state[page] == 2) {
            return {};
        }
        state[page] = 1;
        auto const& record = pages_[page];
        for (std::uint32_t d = record.dependency_first; d < record.dependency_first + record.dependency_count; ++d) {
            if (auto collected = collect(dependencies_[d], state, order); !collected) {
                return collected;
            }
        }
        state[page] = 2;
        order.push_back(page);
        return {};
    }

    bool page_manager::pinned(std::uint32_t page) const noexcept {
        return (pages_[page].flags & resident_flag) != 0;
    }

    bool page_manager::depends_on(std::uint32_t dependent, std::uint32_t dependency) const noexcept {
        if (dependent >= pages_.size()) {
            return false;
        }
        auto const& record = pages_[dependent];
        for (std::uint32_t d = record.dependency_first; d < record.dependency_first + record.dependency_count; ++d) {
            if (dependencies_[d] == dependency) {
                return true;
            }
        }
        return false;
    }

    void page_manager::touch(std::uint32_t page) noexcept {
        last_use_[page] = ++clock_;
    }

    std::span<page_entry const> page_manager::pages() const noexcept {
        return {pages_.data(), pages_.size()};
    }

    std::span<std::uint32_t const> page_manager::dependencies() const noexcept {
        return {dependencies_.data(), dependencies_.size()};
    }

    residency_limits const& page_manager::limits() const noexcept {
        return limits_;
    }

    residency_stats page_manager::stats() const noexcept {
        return {resident_bytes_, resident_pages_, installs_, evictions_};
    }

    bool page_manager::resident(std::uint32_t page) const noexcept {
        return page < pages_.size() && resident_[page] != 0;
    }

    std::expected<residency_plan, residency_error> page_manager::plan_request(std::uint32_t page) const {
        return plan_requests(std::span(&page, 1));
    }

    std::expected<residency_plan, residency_error> page_manager::plan_requests(std::span<std::uint32_t const> targets) const {
        if (targets.empty()) {
            return std::unexpected(residency_error::invalid_page);
        }
        utility::vessel::vector<std::uint8_t> state(pages_.size(), std::uint8_t{0});
        utility::vessel::vector<std::uint32_t> order;
        order.reserve(pages_.size());
        for (auto page : targets) {
            if (page >= pages_.size()) {
                return std::unexpected(residency_error::invalid_page);
            }
            if (auto collected = collect(page, state, order); !collected) {
                return std::unexpected(collected.error());
            }
        }
        residency_plan plan;
        plan.target = targets.front();
        utility::vessel::vector<std::uint8_t> required(pages_.size(), std::uint8_t{0});
        for (auto id : order) {
            required[id] = 1;
            if (!resident_[id]) {
                plan.installs.push_back(id);
                plan.installed_bytes += pages_[id].bytes;
            }
        }
        std::uint64_t projected = resident_bytes_ + plan.installed_bytes;
        std::uint64_t projected_pages = resident_pages_ + plan.installs.size();
        utility::vessel::vector<std::uint8_t> evicted(pages_.size(), std::uint8_t{0});
        while (projected > limits_.max_resident_bytes ||
               (limits_.max_resident_pages != 0u && projected_pages > limits_.max_resident_pages)) {
            auto victim = no_page;
            auto victim_use = std::numeric_limits<std::uint64_t>::max();
            for (std::uint32_t candidate = 0; candidate < pages_.size(); ++candidate) {
                if (!resident_[candidate] || required[candidate] || pinned(candidate) || evicted[candidate]) {
                    continue;
                }
                bool blocked = false;
                for (std::uint32_t dependent = 0; dependent < pages_.size(); ++dependent) {
                    if (dependent == candidate || !resident_[dependent] || evicted[dependent]) {
                        continue;
                    }
                    if (depends_on(dependent, candidate)) {
                        blocked = true;
                        break;
                    }
                }
                if (blocked) {
                    continue;
                }
                if (last_use_[candidate] < victim_use) {
                    victim = candidate;
                    victim_use = last_use_[candidate];
                }
            }
            if (victim == no_page) {
                return std::unexpected(residency_error::capacity);
            }
            evicted[victim] = 1;
            plan.evictions.push_back(victim);
            plan.evicted_bytes += pages_[victim].bytes;
            projected -= pages_[victim].bytes;
            --projected_pages;
        }
        return plan;
    }

    std::expected<void, residency_error> page_manager::apply(residency_plan&& plan) {
        if (plan.target >= pages_.size()) {
            return std::unexpected(residency_error::invalid_page);
        }
        utility::vessel::vector<std::uint8_t> installing(pages_.size(), std::uint8_t{0});
        utility::vessel::vector<std::uint8_t> evicting(pages_.size(), std::uint8_t{0});
        for (auto id : plan.installs) {
            if (id >= pages_.size() || resident_[id] || installing[id] || evicting[id]) {
                return std::unexpected(residency_error::invalid_page);
            }
            installing[id] = 1;
        }
        for (auto id : plan.evictions) {
            if (id >= pages_.size() || !resident_[id] || pinned(id) || installing[id] || evicting[id]) {
                return std::unexpected(id < pages_.size() && pinned(id) ? residency_error::pinned : residency_error::invalid_page);
            }
            evicting[id] = 1;
        }
        if (evicting[plan.target]) {
            return std::unexpected(residency_error::invalid_page);
        }
        // The installs must be dependency-closed against the resident set that will remain.
        for (auto id : plan.installs) {
            auto const& record = pages_[id];
            for (std::uint32_t d = record.dependency_first; d < record.dependency_first + record.dependency_count; ++d) {
                auto const dependency = dependencies_[d];
                if (evicting[dependency] || (!resident_[dependency] && !installing[dependency])) {
                    return std::unexpected(residency_error::invalid_page);
                }
            }
        }
        // Every eviction must be safe for all pages that remain resident.
        for (auto id : plan.evictions) {
            for (std::uint32_t dependent = 0; dependent < pages_.size(); ++dependent) {
                if (dependent != id && resident_[dependent] && !evicting[dependent] && depends_on(dependent, id)) {
                    return std::unexpected(residency_error::dependency);
                }
            }
        }
        std::uint64_t installed_bytes = 0;
        std::uint64_t evicted_bytes = 0;
        for (auto id : plan.installs) {
            installed_bytes += pages_[id].bytes;
        }
        for (auto id : plan.evictions) {
            evicted_bytes += pages_[id].bytes;
        }
        std::uint64_t projected = resident_bytes_ + installed_bytes - evicted_bytes;
        std::uint64_t projected_pages = resident_pages_ + plan.installs.size() - plan.evictions.size();
        if (projected > limits_.max_resident_bytes ||
            (limits_.max_resident_pages != 0u && projected_pages > limits_.max_resident_pages)) {
            return std::unexpected(residency_error::capacity);
        }
        for (auto id : plan.evictions) {
            resident_[id] = 0;
            --resident_pages_;
            resident_bytes_ -= pages_[id].bytes;
            ++evictions_;
        }
        for (auto id : plan.installs) {
            resident_[id] = 1;
            ++resident_pages_;
            resident_bytes_ += pages_[id].bytes;
            ++installs_;
            touch(id);
        }
        touch(plan.target);
        return {};
    }

    std::expected<std::uint32_t, residency_error> page_manager::request(std::uint32_t page) {
        auto planned = plan_request(page);
        if (!planned) {
            return std::unexpected(planned.error());
        }
        auto installed = static_cast<std::uint32_t>(planned->installs.size());
        if (auto applied = apply(std::move(*planned)); !applied) {
            return std::unexpected(applied.error());
        }
        return installed;
    }

    std::expected<std::uint32_t, residency_error> page_manager::request_all() {
        if (resident_pages_ == pages_.size()) {
            return 0;
        }
        utility::vessel::vector<std::uint8_t> state(pages_.size(), std::uint8_t{0});
        utility::vessel::vector<std::uint32_t> order;
        order.reserve(pages_.size());
        // A resident set is dependency-closed, so resident pages and their dependencies can be skipped.
        for (std::uint32_t page = 0; page < pages_.size(); ++page) {
            if (resident_[page]) {
                state[page] = 2;
            }
        }
        for (std::uint32_t page = 0; page < pages_.size(); ++page) {
            if (!resident_[page]) {
                if (auto collected = collect(page, state, order); !collected) {
                    return std::unexpected(collected.error());
                }
            }
        }
        std::uint64_t needed = 0;
        for (auto id : order) {
            needed += pages_[id].bytes;
        }
        auto const projected_pages = static_cast<std::uint64_t>(resident_pages_) + order.size();
        if (resident_bytes_ + needed > limits_.max_resident_bytes ||
            (limits_.max_resident_pages != 0u && projected_pages > limits_.max_resident_pages)) {
            return std::unexpected(residency_error::capacity);
        }
        for (auto id : order) {
            resident_[id] = 1;
            ++resident_pages_;
            resident_bytes_ += pages_[id].bytes;
            ++installs_;
            touch(id);
        }
        return static_cast<std::uint32_t>(order.size());
    }

    std::expected<void, residency_error> page_manager::evict(std::uint32_t page) {
        if (page >= pages_.size() || !resident_[page]) {
            return std::unexpected(residency_error::invalid_page);
        }
        if (pinned(page)) {
            return std::unexpected(residency_error::pinned);
        }
        for (std::uint32_t dependent = 0; dependent < pages_.size(); ++dependent) {
            if (dependent != page && resident_[dependent] && depends_on(dependent, page)) {
                return std::unexpected(residency_error::dependency);
            }
        }
        resident_[page] = 0;
        --resident_pages_;
        resident_bytes_ -= pages_[page].bytes;
        ++evictions_;
        return {};
    }

    std::expected<page_manager, residency_error> make_page_manager(std::span<page_entry const> pages, std::span<std::uint32_t const> dependencies, residency_limits limits) {
        page_manager manager(pages, dependencies, limits);
        if (auto initialized = manager.initialize(); !initialized) {
            return std::unexpected(initialized.error());
        }
        return std::move(manager);
    }
} // namespace deren::engine::subtilis
