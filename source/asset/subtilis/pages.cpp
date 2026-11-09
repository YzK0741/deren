module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
module deren.asset.subtilis.pages;
import deren.vstd;

namespace deren::asset::subtilis {
    namespace {
        auto fail(error_code code, char const* context) {
            return std::unexpected(error{code, context, {}, {}});
        }
        constexpr uint32_t stride_of(uint32_t section) {
            switch (section) {
            case 1: return 4;
            case 2: return 32;
            case 3: return 48;
            case 4: return 4;
            case 5: return 4;
            case 6: return 4;
            default: return 0;
            }
        }
        uint64_t count_of(payload const& p, uint32_t section) {
            switch (section) {
            case 1: return p.indices.size();
            case 2: return p.clusters.size();
            case 3: return p.groups.size();
            case 4: return p.references.size();
            case 5: return p.leaves.size();
            case 6: return p.roots.size();
            default: return 0;
            }
        }
        uint32_t find_page(vector<page_entry> const& pages, uint32_t section, uint64_t element) {
            auto first = std::lower_bound(pages.begin(), pages.end(), section, [](page_entry const& page, uint32_t value) { return page.section < value; });
            auto last = std::upper_bound(first, pages.end(), section, [](uint32_t value, page_entry const& page) { return value < page.section; });
            auto it = std::upper_bound(first, last, element, [](uint64_t value, page_entry const& page) { return value < page.first; });
            if (it == first) {
                return none;
            }
            --it;
            if (element < it->first || element >= uint64_t(it->first) + it->count) {
                return none;
            }
            return static_cast<uint32_t>(it - pages.begin());
        }
    } // namespace

    result<void> validate_pages(payload const& p, limits const& lim) {
        if (p.pages.empty()) {
            if (!p.page_dependencies.empty()) {
                return fail(error_code::invalid_payload, "page dependencies without pages");
            }
            return {};
        }
        if (p.pages.size() > lim.max_records || p.pages.size() > UINT32_MAX) {
            return fail(error_code::capacity, "page count exceeds limit");
        }
        uint64_t counts[6]{count_of(p, 1), count_of(p, 2), count_of(p, 3), count_of(p, 4), count_of(p, 5), count_of(p, 6)};
        uint64_t cursor[6]{};
        vector<uint32_t> indegree(p.pages.size(), 0);
        uint32_t previous = 0;
        for (uint32_t i = 0; i < p.pages.size(); ++i) {
            auto const& page = p.pages[i];
            if (page.section < 1 || page.section > 6 || page.section < previous || !page.count || (page.flags & ~uint16_t(page_flags::always_resident)) || page.reserved) {
                return fail(error_code::invalid_payload, "invalid page record");
            }
            previous = page.section;
            auto const stride = stride_of(page.section);
            auto& at = cursor[page.section - 1];
            if (page.first != at || page.count > counts[page.section - 1] - at || page.bytes != page.count * stride || page.bytes > page_bytes) {
                return fail(error_code::invalid_payload, "invalid page range or byte count");
            }
            if (page.dependency_first > p.page_dependencies.size() || page.dependency_count > p.page_dependencies.size() - page.dependency_first) {
                return fail(error_code::invalid_payload, "page dependency range outside table");
            }
            uint32_t last_dependency = none;
            for (uint32_t d = page.dependency_first; d < page.dependency_first + page.dependency_count; ++d) {
                auto const dependency = p.page_dependencies[d];
                if (dependency >= p.pages.size() || dependency == i || (last_dependency != none && dependency <= last_dependency)) {
                    return fail(error_code::invalid_payload, "invalid page dependency");
                }
                last_dependency = dependency;
                ++indegree[dependency];
            }
            at += page.count;
        }
        for (uint32_t section = 0; section < 6; ++section) {
            if (cursor[section] != counts[section]) {
                return fail(error_code::invalid_payload, "page table does not cover a section");
            }
        }
        // Every always-resident page must have an always-resident dependency closure. This is what
        // makes the LOD0/root pages usable without a residency request.
        for (uint32_t i = 0; i < p.pages.size(); ++i) {
            auto const& page = p.pages[i];
            if (!(page.flags & uint16_t(page_flags::always_resident))) {
                continue;
            }
            for (uint32_t d = page.dependency_first; d < page.dependency_first + page.dependency_count; ++d) {
                if (!(p.pages[p.page_dependencies[d]].flags & uint16_t(page_flags::always_resident))) {
                    return fail(error_code::invalid_payload, "resident page depends on an evictable page");
                }
            }
        }
        // A cluster's whole index window must live in one page: an index page is the unit that will
        // later be installed, and a window split across two pages cannot be drawn conservatively.
        for (auto const& cluster : p.clusters) {
            auto const page_id = find_page(p.pages, 1, cluster.first_index);
            if (page_id == none) {
                return fail(error_code::invalid_payload, "cluster index window has no page");
            }
            auto const& page = p.pages[page_id];
            if (uint64_t(cluster.first_index) + cluster.index_count > uint64_t(page.first) + page.count) {
                return fail(error_code::invalid_payload, "cluster index window crosses a page boundary");
            }
        }
        // Kahn's algorithm over page -> dependency edges. Page dependencies must be a DAG so the
        // residency manager can install parents before children.
        vector<uint32_t> queue;
        queue.reserve(p.pages.size());
        for (uint32_t i = 0; i < indegree.size(); ++i) {
            if (!indegree[i]) {
                queue.push_back(i);
            }
        }
        size_t visited = 0;
        for (size_t head = 0; head < queue.size(); ++head) {
            auto const page = queue[head];
            ++visited;
            for (uint32_t d = p.pages[page].dependency_first; d < p.pages[page].dependency_first + p.pages[page].dependency_count; ++d) {
                if (--indegree[p.page_dependencies[d]] == 0) {
                    queue.push_back(p.page_dependencies[d]);
                }
            }
        }
        if (visited != p.pages.size()) {
            return fail(error_code::invalid_payload, "page dependency cycle");
        }
        return {};
    }

    result<void> compute_pages(payload& p, limits const& lim) {
        p.pages.clear();
        p.page_dependencies.clear();
        auto const add = [&](uint32_t section, uint64_t first, uint64_t count) -> bool {
            if (!count) {
                return true;
            }
            if (p.pages.size() >= lim.max_records || first + count > count_of(p, section)) {
                return false;
            }
            auto const bytes = count * stride_of(section);
            if (bytes > page_bytes) {
                return false;
            }
            p.pages.push_back({section, static_cast<uint32_t>(first), static_cast<uint32_t>(count), static_cast<uint32_t>(bytes), 0, 0, 0, 0, 0});
            return true;
        };

        // Index pages are cut at cluster-window starts so no window ever straddles a page. Gaps
        // between windows are split at the page capacity when they alone exceed it.
        vector<uint32_t> order(p.clusters.size());
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return p.clusters[a].first_index != p.clusters[b].first_index ? p.clusters[a].first_index < p.clusters[b].first_index : a < b; });
        uint64_t page_start = 0;
        auto flush_indexes = [&](uint64_t end) -> bool {
            while (end - page_start > page_bytes / 4u) {
                if (!add(1, page_start, page_bytes / 4u)) {
                    return false;
                }
                page_start += page_bytes / 4u;
            }
            if (end > page_start) {
                if (!add(1, page_start, end - page_start)) {
                    return false;
                }
                page_start = end;
            }
            return true;
        };
        for (auto id : order) {
            auto const& cluster = p.clusters[id];
            uint64_t const start = cluster.first_index, finish = start + cluster.index_count;
            if (start < page_start) {
                return fail(error_code::invalid_payload, "overlapping cluster index windows");
            }
            if (finish > page_start + page_bytes / 4u) {
                if (!flush_indexes(start) || finish > page_start + page_bytes / 4u) {
                    return fail(error_code::invalid_payload, "cluster index window is too large");
                }
            }
        }
        if (!flush_indexes(p.indices.size())) {
            return fail(error_code::capacity, "index page table exceeds limit");
        }
        for (uint32_t section = 2; section <= 6; ++section) {
            auto const total = count_of(p, section);
            auto const capacity = page_bytes / stride_of(section);
            for (uint64_t first = 0; first < total; first += capacity) {
                if (!add(section, first, std::min<uint64_t>(capacity, total - first))) {
                    return fail(error_code::capacity, "record page table exceeds limit");
                }
            }
        }

        vector<bool> resident(p.clusters.size(), false);
        for (auto id : p.leaves) {
            if (id >= resident.size()) {
                return fail(error_code::invalid_payload, "leaf outside cluster table");
            }
            resident[id] = true;
        }
        for (auto id : p.roots) {
            if (id >= resident.size()) {
                return fail(error_code::invalid_payload, "root outside cluster table");
            }
            resident[id] = true;
        }
        for (uint32_t i = 0; i < p.pages.size(); ++i) {
            auto& page = p.pages[i];
            if (page.section == 5 || page.section == 6) {
                page.flags |= uint16_t(page_flags::always_resident);
            } else if (page.section == 2) {
                for (uint32_t id = page.first; id < page.first + page.count; ++id) {
                    if (resident[id]) {
                        page.flags |= uint16_t(page_flags::always_resident);
                    }
                }
            }
        }
        // Dependencies point from the page that consumes an element to the page that stores it.
        // They are one hop (groups -> references -> clusters -> indices); ancestors are derived.
        auto append_dependencies = [&](uint32_t page_id, vector<uint32_t> dependencies) {
            std::sort(dependencies.begin(), dependencies.end());
            dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
            p.pages[page_id].dependency_first = static_cast<uint32_t>(p.page_dependencies.size());
            p.pages[page_id].dependency_count = static_cast<uint32_t>(dependencies.size());
            p.page_dependencies.insert(p.page_dependencies.end(), dependencies.begin(), dependencies.end());
        };
        auto add_section_pages = [&](vector<uint32_t>& out, uint32_t section, uint64_t first, uint64_t count) {
            auto const end = first + count;
            for (uint32_t i = 0; i < p.pages.size(); ++i) {
                auto const& page = p.pages[i];
                if (page.section == section && page.first < end && first < uint64_t(page.first) + page.count) {
                    out.push_back(i);
                }
            }
        };
        for (uint32_t i = 0; i < p.pages.size(); ++i) {
            auto const page = p.pages[i];
            vector<uint32_t> dependencies;
            if (page.section == 2) {
                for (uint32_t id = page.first; id < page.first + page.count; ++id) {
                    auto const index_page = find_page(p.pages, 1, p.clusters[id].first_index);
                    if (index_page == none) {
                        return fail(error_code::invalid_payload, "cluster has no index page");
                    }
                    dependencies.push_back(index_page);
                }
            } else if (page.section == 3) {
                for (uint32_t id = page.first; id < page.first + page.count; ++id) {
                    auto const& group = p.groups[id];
                    if (uint64_t(group.fine_first) + group.fine_count > p.references.size() || uint64_t(group.coarse_first) + group.coarse_count > p.references.size()) {
                        return fail(error_code::invalid_payload, "group reference range outside table");
                    }
                    add_section_pages(dependencies, 4, group.fine_first, group.fine_count);
                    add_section_pages(dependencies, 4, group.coarse_first, group.coarse_count);
                }
            } else if (page.section == 4) {
                for (uint32_t id = page.first; id < page.first + page.count; ++id) {
                    auto const cluster = p.references[id];
                    if (cluster >= p.clusters.size()) {
                        return fail(error_code::invalid_payload, "reference outside cluster table");
                    }
                    auto const cluster_page = find_page(p.pages, 2, cluster);
                    if (cluster_page == none) {
                        return fail(error_code::invalid_payload, "reference has no cluster page");
                    }
                    dependencies.push_back(cluster_page);
                }
            } else if (page.section == 5 || page.section == 6) {
                auto const& ids = page.section == 5 ? p.leaves : p.roots;
                for (uint32_t id = page.first; id < page.first + page.count; ++id) {
                    auto const cluster_page = find_page(p.pages, 2, ids[id]);
                    if (cluster_page == none) {
                        return fail(error_code::invalid_payload, "leaf or root has no cluster page");
                    }
                    dependencies.push_back(cluster_page);
                }
            }
            append_dependencies(i, std::move(dependencies));
        }
        // Residency is a closure: every page a resident page depends on must also be resident.
        for (bool changed = true; changed;) {
            changed = false;
            for (auto const& page : p.pages) {
                if (!(page.flags & uint16_t(page_flags::always_resident))) {
                    continue;
                }
                for (uint32_t d = page.dependency_first; d < page.dependency_first + page.dependency_count; ++d) {
                    auto& dependency = p.pages[p.page_dependencies[d]];
                    if (!(dependency.flags & uint16_t(page_flags::always_resident))) {
                        dependency.flags |= uint16_t(page_flags::always_resident);
                        changed = true;
                    }
                }
            }
        }
        for (auto& page : p.pages) {
            page.priority = page.flags ? page_priority_always_resident : static_cast<uint16_t>(page.section);
        }
        return validate_pages(p, lim);
    }
} // namespace deren::asset::subtilis
