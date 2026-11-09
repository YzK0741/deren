// ============================================================================
// module: deren.engine.visibility.software:tiling  (the 16x16 tile binning:
//         the fixed-capacity measurement plan, the CSR production plan, and
//         the GPU partition checks)
//
// One of the domains of deren.engine.visibility.software; the top-level
// facade re-exports it, so consumers keep importing the same module.
// ============================================================================

module;
#include <cstdint>
export module deren.engine.visibility.software:tiling;
export import deren.vstd;

import deren.engine.visibility;
import deren.engine.meshlet;
import deren.utility.shared;

import :classification;

export namespace deren::engine::visibility::software {
    /// one (item, meshlet) pair filed under a tile; meshlet_id is the run-local index the packed id will carry
    struct tile_entry {
        std::uint32_t item_index = 0u;
        std::uint32_t meshlet_id = 0u;
    };

    /**
     * @brief the CPU tile list: header fields + per-tile exact counts + the fixed-capacity entry table, tile-major
     * @note `counts` is ALWAYS exact (measured without truncation); `entries` holds at most entry_capacity slots per
     *       tile, so it is complete only when !overflow. A production caller must fall back to the whole-frame
     *       hardware path on overflow (spec §5.4); the Task 1 probe uses the exact counts to size the default.
     */
    struct software_tile_plan {
        tile_grid grid{};
        std::uint32_t entry_capacity = tile_entry_capacity_default;
        std::uint32_t item_count = 0u;
        std::uint32_t cluster_count = 0u; ///< total software records filed
        std::uint64_t entry_count = 0u;   ///< sum of counts == cluster_count * covered tiles
        std::uint32_t max_tile_entries = 0u;
        std::uint32_t overflow_tiles = 0u;
        bool overflow = false;
        deren::utility::vessel::vector<item_split> items;
        deren::utility::vessel::vector<std::uint32_t> counts;
        deren::utility::vessel::vector<tile_entry> entries;
    };

    namespace detail {
        /// the one binning pass both entry points share; capacity overflow is REPORTED, never silently dropped
        [[nodiscard]] inline software_tile_plan bin_tile_lists(std::span<item_records const> const items,
                                                               camera_metrics const& camera, float const threshold_px,
                                                               std::uint32_t const entry_capacity) {
            software_tile_plan plan{};
            plan.grid = make_tile_grid(camera.width, camera.height);
            plan.entry_capacity = entry_capacity;
            plan.item_count = static_cast<std::uint32_t>(items.size());
            std::uint32_t const tiles = plan.grid.tile_count();
            plan.counts.assign(tiles, 0u);
            plan.entries.assign(static_cast<std::size_t>(tiles) * entry_capacity, tile_entry{});
            plan.items.reserve(items.size());
            deren::utility::vessel::vector<projected_record> projections;
            deren::utility::vessel::vector<std::uint8_t> software;
            for (item_records const& item : items) {
                item_split split{};
                split.item_index = item.item_index;
                split.record_count = static_cast<std::uint32_t>(item.records.size());
                projections.clear();
                projections.reserve(item.records.size());
                software.clear();
                software.resize(item.records.size(), 0u);
                if (item.records.size() > visibility::id_meshlet_capacity) {
                    // the packed id cannot carry a suffix: keep the item wholly on the hardware path (safe direction)
                    for (meshlet const& record : item.records) {
                        split.hardware_triangles += record.index_count / 3u;
                    }
                    split.hardware_count = split.record_count;
                    plan.items.push_back(split);
                    continue;
                }
                for (std::size_t index = 0u; index < item.records.size(); ++index) {
                    auto const projection = project(item.records[index], item.model, camera, plan.grid);
                    bool const subpixel = projection.finite && projection.front_of_near &&
                                          projection.diameter_upper_px < static_cast<double>(threshold_px);
                    software[index] = subpixel ? 1u : 0u;
                    if (subpixel) {
                        ++split.software_count;
                        split.software_triangles += item.records[index].index_count / 3u;
                    } else {
                        ++split.hardware_count;
                        split.hardware_triangles += item.records[index].index_count / 3u;
                    }
                    projections.push_back(projection);
                }
                plan.cluster_count += split.software_count;
                plan.items.push_back(split);
                // file the software suffix, in suffix order, with the id its lane position will carry
                std::uint32_t suffix = 0u;
                for (std::size_t index = 0u; index < item.records.size(); ++index) {
                    if (software[index] == 0u) {
                        continue;
                    }
                    projected_record const& projection = projections[index];
                    std::uint32_t const meshlet_id = split.hardware_count + suffix;
                    ++suffix;
                    if (!projection.on_screen) {
                        continue; // a subpixel record off the frame covers no tile (still counted/moved)
                    }
                    for (std::int32_t tile_y = projection.tile_min_y; tile_y <= projection.tile_max_y; ++tile_y) {
                        for (std::int32_t tile_x = projection.tile_min_x; tile_x <= projection.tile_max_x; ++tile_x) {
                            std::size_t const tile = static_cast<std::size_t>(tile_y) * plan.grid.tiles_x +
                                                     static_cast<std::size_t>(tile_x);
                            std::uint32_t const slot = plan.counts[tile];
                            ++plan.counts[tile];
                            ++plan.entry_count;
                            if (slot < entry_capacity) {
                                plan.entries[tile * entry_capacity + slot] = tile_entry{.item_index = item.item_index,
                                                                                        .meshlet_id = meshlet_id};
                            }
                        }
                    }
                }
            }
            for (std::uint32_t const count : plan.counts) {
                plan.max_tile_entries = std::max(plan.max_tile_entries, count);
                if (count > entry_capacity) {
                    ++plan.overflow_tiles;
                }
            }
            plan.overflow = plan.overflow_tiles != 0u;
            return plan;
        }
    } // namespace detail

    /**
     * @brief THE MEASUREMENT form (Task 1): bin the frame's subpixel records and report over-capacity tiles EXACTLY
     * @note this entry point exists for the GO/NO-GO workload probe and for the capacity decision (32 vs 64): it must
     *       see the true per-tile maximum even when it exceeds the production capacity, which the strict form refuses.
     *       It is NOT a production path - the probe is capture-only and eval-gated, and the production caller uses
     *       build_tile_lists, where overflow means "whole frame on the hardware path".
     */
    [[nodiscard]] inline software_tile_plan build_tile_lists_measure(std::span<item_records const> const items,
                                                                     camera_parameters const& parameters,
                                                                     float const threshold_px,
                                                                     std::uint32_t const entry_capacity = tile_entry_capacity_default) {
        auto const camera = prepare_camera(parameters);
        if (!camera.has_value()) {
            return software_tile_plan{};
        }
        return detail::bin_tile_lists(items, *camera, threshold_px, std::max(1u, entry_capacity));
    }

    // ================================================================================================
    // THE PRODUCTION TILE LIST: CSR (spec §3.4/§9 Q7, user-approved 2026-10-22)
    //
    // Task 1's measurement (see build_tile_lists_measure above) FALSIFIED the fixed per-tile capacity: far's
    // densest tile held 977 entries at 1.0 px and 1,907 at 2.0 px while the frame's whole total was only 4,572 /
    // 12,557 entries over 3,600 tiles - a long-tailed distribution where any fixed per-tile cap either overflows
    // or wastes tens of MiB. The approved replacement is this CSR (compressed sparse row) list: every tile holds a
    // RANGE of ONE shared entries array, located by an exclusive prefix sum of the per-tile counts. There is NO
    // per-tile capacity and NO per-tile overflow anywhere on the production path; the only capacity is the global
    // `software_entry_budget`, and exceeding it refuses the WHOLE frame (never truncates: spec §5.4's "no partial
    // software image, no half-new frame"). The GPU software raster reads exactly `offsets[t] .. offsets[t+1]` for
    // its tile, so a sparse tile is a zero-length range rather than a gap in a fixed stride.
    //
    // THE ORDER IS A CONTRACT: entries are appended in ITEM order -> RUN order -> TILE order (the same order the
    // classification walks), so the list is byte-identical for identical inputs. That determinism is what lets
    // the CPU reference and the GPU probe compare a captured list entry by entry (Task 3) and what makes the
    // `(depth, id)` total order's independence from list order a property rather than a hope.
    // ================================================================================================
    struct software_tile_lists {
        tile_grid grid{};
        std::uint32_t entry_budget = software_entry_budget;    ///< the global cap `entry_count` was checked against
        std::uint32_t item_count = 0u;                         ///< items the builder was given
        std::uint32_t cluster_count = 0u;                      ///< software (subpixel) records filed into the list
        std::uint32_t entry_count = 0u;                        ///< CSR entries actually used (== offsets[tiles])
        std::uint32_t max_tile_entries = 0u;                   ///< DIAGNOSTICS only: the busiest tile's count
        bool overflow = false;                                 ///< measurement-only form; the production builder refuses instead
        deren::utility::vessel::vector<item_split> items;      ///< per item: the [hardware prefix | software suffix] split
        deren::utility::vessel::vector<std::uint32_t> counts;  ///< per tile: exact entry count (== offsets[t+1] - offsets[t])
        deren::utility::vessel::vector<std::uint32_t> offsets; ///< tiles + 1 exclusive prefix sums; offsets[0] == 0
        deren::utility::vessel::vector<tile_entry> entries;    ///< entry_count entries, grouped by tile, in the order above

        /// @brief the CSR range of tile (@p x, @p y), as A separate span entries (empty for a sparse tile)
        [[nodiscard]] inline std::span<tile_entry const> tile_span(std::uint32_t const x, std::uint32_t const y) const noexcept {
            if (x >= this->grid.tiles_x || y >= this->grid.tiles_y || this->offsets.size() != static_cast<std::size_t>(this->grid.tile_count()) + 1u) {
                return {};
            }
            std::size_t const tile = static_cast<std::size_t>(y) * this->grid.tiles_x + x;
            std::uint32_t const begin = this->offsets[tile];
            std::uint32_t const end = this->offsets[tile + 1u];
            if (begin > end || end > this->entries.size()) {
                return {};
            }
            return std::span<tile_entry const>(this->entries.data() + begin, end - begin);
        }

        [[nodiscard]] bool empty() const noexcept {
            return this->cluster_count == 0u || this->entry_count == 0u;
        }
    };

    /// @brief serialize one frame's CSR into the exact uint-word layout consumed by software_visibility.slang
    /// @return false for malformed lists or an undersized lane; no partial write occurs on failure
    [[nodiscard]] inline bool write_tile_lists(std::span<std::uint32_t> lane, software_tile_lists const& lists) noexcept {
        std::uint32_t const tiles = lists.grid.tile_count();
        std::uint64_t const needed_words = static_cast<std::uint64_t>(software_tiles_entries_word(tiles)) +
                                           static_cast<std::uint64_t>(lists.entry_budget) * (software_tiles_entry_bytes / sizeof(std::uint32_t));
        if (lists.overflow || lists.entry_count > lists.entry_budget || lists.entries.size() != lists.entry_count ||
            lists.counts.size() != tiles || lists.offsets.size() != static_cast<std::size_t>(tiles) + 1u || lists.offsets.empty() ||
            lists.offsets.front() != 0u || lists.offsets.back() != lists.entry_count || lane.size() < needed_words) {
            return false;
        }
        for (std::uint32_t tile = 0u; tile < tiles; ++tile) {
            if (lists.offsets[tile] > lists.offsets[tile + 1u] || lists.offsets[tile + 1u] > lists.entry_count ||
                lists.counts[tile] != lists.offsets[tile + 1u] - lists.offsets[tile]) {
                return false;
            }
        }
        std::fill(lane.begin(), lane.end(), 0u);
        lane[0] = lists.grid.tiles_x;
        lane[1] = lists.grid.tiles_y;
        lane[2] = lists.entry_count;
        lane[3] = 0u;
        lane[4] = lists.cluster_count;
        std::uint32_t active_tile_count = 0u;
        for (std::uint32_t tile = 0u; tile < tiles; ++tile) {
            if (lists.counts[tile] != 0u) {
                lane[software_tiles_active_tiles_word(tiles) + active_tile_count] = tile;
                ++active_tile_count;
            }
        }
        lane[5] = active_tile_count;
        std::copy(lists.counts.begin(), lists.counts.end(), lane.begin() + software_tiles_counts_word(tiles));
        std::copy(lists.offsets.begin(), lists.offsets.end(), lane.begin() + software_tiles_offsets_word(tiles));
        std::uint32_t const entries_word = software_tiles_entries_word(tiles);
        for (std::uint32_t index = 0u; index < lists.entry_count; ++index) {
            lane[entries_word + index * 2u] = lists.entries[index].item_index;
            lane[entries_word + index * 2u + 1u] = lists.entries[index].meshlet_id;
        }
        return true;
    }

    /// Packed ordering for normal-Z atomic min: depth and ID are one indivisible candidate.
    [[nodiscard]] constexpr std::uint64_t atomic_candidate(std::uint32_t depth, std::uint32_t id) noexcept {
        return (static_cast<std::uint64_t>(depth) << 32u) | id;
    }

    [[nodiscard]] inline std::expected<deren::utility::vessel::vector<tile_entry>, software_error>
    triangle_dispatch_entries(software_tile_lists const& lists) {
        if (lists.overflow || lists.entry_count != lists.entries.size() || lists.entry_count > software_entry_budget) {
            return std::unexpected(software_error::invalid_reference);
        }
        auto entries = lists.entries;
        auto less = [](tile_entry const& a, tile_entry const& b) {
            return a.item_index < b.item_index || (a.item_index == b.item_index && a.meshlet_id < b.meshlet_id);
        };
        std::sort(entries.begin(), entries.end(), less);
        entries.erase(std::unique(entries.begin(), entries.end(), [](tile_entry const& a, tile_entry const& b) {
                          return a.item_index == b.item_index && a.meshlet_id == b.meshlet_id;
                      }),
                      entries.end());
        return entries;
    }

    /// Number of non-empty CSR tiles dispatched by the sparse software raster pass.
    [[nodiscard]] inline std::uint32_t active_tile_count(software_tile_lists const& lists) noexcept {
        return static_cast<std::uint32_t>(std::count_if(lists.counts.begin(), lists.counts.end(), [](std::uint32_t const count) { return count != 0u; }));
    }

    struct gpu_partition_item {
        std::uint32_t item_index = 0u;
        std::span<meshlet const> candidates;
        std::span<std::uint32_t const> flags;
        std::span<meshlet const> hardware;
        std::uint32_t primary_count = 0u, recovery_count = 0u;
    };
    /// Capture-only independent coverage oracle. HW must equal the stable primary/recovery survivors
    /// after subtracting actual SW IDs; duplicates, missing candidates and temporary flags are refused.
    [[nodiscard]] inline bool validate_gpu_partition(std::span<tile_entry const> software, std::span<gpu_partition_item const> items) {
        std::array<int32_t, visibility::id_item_capacity> lookup{};
        lookup.fill(-1);
        deren::utility::vessel::vector<deren::utility::vessel::vector<std::uint8_t>> masks(items.size());
        for (std::size_t i = 0u; i < items.size(); ++i) {
            auto const& item = items[i];
            if (item.item_index >= lookup.size() || lookup[item.item_index] >= 0 || item.candidates.size() > visibility::id_meshlet_capacity ||
                item.flags.size() != item.candidates.size() || item.primary_count > item.candidates.size() ||
                item.recovery_count > item.candidates.size() - item.primary_count ||
                item.hardware.size() < item.primary_count + item.recovery_count) {
                return false;
            }
            for (auto flag : item.flags) {
                if (flag != 0u && flag != 1u && flag != 3u) {
                    return false;
                }
            }
            lookup[item.item_index] = static_cast<int32_t>(i);
            masks[i].resize(item.candidates.size(), 0u);
        }
        for (auto const& entry : software) {
            if (entry.item_index >= lookup.size() || lookup[entry.item_index] < 0) {
                return false;
            }
            auto const i = static_cast<std::size_t>(lookup[entry.item_index]);
            auto const& item = items[i];
            if (entry.meshlet_id >= item.candidates.size() || masks[i][entry.meshlet_id] || item.flags[entry.meshlet_id] == 1u) {
                return false;
            }
            masks[i][entry.meshlet_id] = 1u;
        }
        for (std::size_t i = 0u; i < items.size(); ++i) {
            auto const& item = items[i];
            for (std::uint32_t phase : {0u, 3u}) {
                auto const offset = phase == 0u ? 0u : item.primary_count;
                auto const expected = phase == 0u ? item.primary_count : item.recovery_count;
                std::uint32_t count = 0u;
                for (std::size_t c = 0u; c < item.candidates.size(); ++c) {
                    if (item.flags[c] != phase || masks[i][c]) {
                        continue;
                    }
                    if (count >= expected || std::bit_cast<std::array<std::uint32_t, 12>>(item.hardware[offset + count]) !=
                                                 std::bit_cast<std::array<std::uint32_t, 12>>(item.candidates[c])) {
                        return false;
                    }
                    ++count;
                }
                if (count != expected) {
                    return false;
                }
            }
        }
        return true;
    }

    /// Append one already-classified item exactly once. IDs are suffix POSITIONS, including offscreen gaps.
    /// All validation/budget checks precede mutation; caller restores full hardware on false.
    [[nodiscard]] inline bool append_triangle_dispatch(item_split const& split, std::span<std::uint32_t const> order,
                                                       std::span<projected_record const> projections, deren::utility::vessel::vector<tile_entry>& output,
                                                       std::uint32_t budget = software_entry_budget) {
        if (budget == 0u || budget > software_entry_budget || output.size() > budget ||
            split.hardware_count > split.record_count || split.software_count != split.record_count - split.hardware_count) {
            return false;
        }
        if (split.software_count == 0u) {
            return true; // includes oversized all-hardware runs
        }
        if (split.item_index >= visibility::id_item_capacity || split.record_count > visibility::id_meshlet_capacity ||
            order.size() != split.record_count || projections.size() != split.record_count) {
            return false;
        }
        std::array<std::uint64_t, visibility::id_meshlet_capacity / 64u> seen{};
        std::uint32_t needed = 0u;
        for (std::uint32_t position = 0u; position < split.record_count; ++position) {
            auto const original = order[position];
            if (original >= split.record_count) {
                return false;
            }
            auto const mask = std::uint64_t{1u} << (original % 64u);
            if ((seen[original / 64u] & mask) != 0u) {
                return false;
            }
            seen[original / 64u] |= mask;
            if (position >= split.hardware_count) {
                auto const& projection = projections[original];
                if (!projection.finite || !projection.front_of_near) {
                    return false;
                }
                if (projection.on_screen) {
                    ++needed;
                }
            }
        }
        if (needed > budget - output.size()) {
            return false;
        }
        output.reserve(output.size() + needed);
        for (std::uint32_t position = split.hardware_count; position < split.record_count; ++position) {
            if (projections[order[position]].on_screen) {
                output.push_back({.item_index = split.item_index, .meshlet_id = position});
            }
        }
        return true;
    }

    /// Triangle mode keeps the existing frame stride/entry offset but never reads CSR padding.
    /// Only header and used entries are written; unused counts/offsets/entries need no host clearing.
    [[nodiscard]] inline bool write_triangle_dispatch(std::span<std::uint32_t> lane, software_tile_lists const& list) noexcept {
        auto const offset = software_tiles_entries_word(list.grid.tile_count());
        if (list.overflow || list.entry_budget == 0u || list.entry_budget > software_entry_budget ||
            list.entry_count != list.entries.size() || list.entry_count > list.entry_budget ||
            static_cast<std::uint64_t>(offset) + list.entry_count * 2ull > lane.size()) {
            return false;
        }
        for (auto const& entry : list.entries) {
            if (entry.item_index >= visibility::id_item_capacity || entry.meshlet_id >= visibility::id_meshlet_capacity) {
                return false;
            }
        }
        lane[0] = list.grid.tiles_x;
        lane[1] = list.grid.tiles_y;
        lane[2] = list.entry_count;
        lane[3] = 0u;
        lane[4] = list.cluster_count;
        lane[5] = list.entry_count;
        for (std::uint32_t i = 0u; i < list.entry_count; ++i) {
            lane[offset + i * 2u] = list.entries[i].item_index;
            lane[offset + i * 2u + 1u] = list.entries[i].meshlet_id;
        }
        return true;
    }

    namespace detail {
        /// the CSR producer's one binning body: two passes over deterministic order, prefix sum between them
        [[nodiscard]] inline std::expected<software_tile_lists, software_error> bin_csr_tile_lists(
            std::span<item_records const> const items, camera_metrics const& camera, float const threshold_px,
            std::uint32_t const entry_budget, routing_metric metric) {
            software_tile_lists lists{};
            lists.grid = make_tile_grid(camera.width, camera.height);
            lists.entry_budget = entry_budget;
            lists.item_count = static_cast<std::uint32_t>(items.size());
            std::uint32_t const tiles = lists.grid.tile_count();
            lists.counts.assign(tiles, 0u);
            lists.items.reserve(items.size());
            // what the second pass needs per item: its split, which original indices are software records, and each
            // software record's conservative tile box (computed once, in the first pass, from the same projection)
            struct filed_item {
                item_split split{};
                deren::utility::vessel::vector<std::uint8_t> software;
                deren::utility::vessel::vector<projected_record> projection_storage;
                std::span<projected_record const> projections; // indexed by ORIGINAL record index
            };
            deren::utility::vessel::vector<filed_item> filed;
            filed.reserve(items.size());
            std::uint64_t total_entries = 0u;
            for (item_records const& item : items) {
                filed_item entry_set{};
                if (!item.cached_projections.empty() && item.cached_projections.size() != item.records.size()) {
                    return std::unexpected{software_error::invalid_reference};
                }
                item_split& split = entry_set.split;
                split.item_index = item.item_index;
                split.record_count = static_cast<std::uint32_t>(item.records.size());
                entry_set.software.assign(item.records.size(), 0u);
                if (item.cached_projections.empty()) {
                    entry_set.projection_storage.resize(item.records.size());
                    entry_set.projections = entry_set.projection_storage;
                } else {
                    entry_set.projections = item.cached_projections;
                }
                if (item.records.size() > visibility::id_meshlet_capacity) {
                    // the packed id cannot carry a suffix: the item stays wholly on the hardware path (safe direction)
                    for (meshlet const& record : item.records) {
                        split.hardware_triangles += record.index_count / 3u;
                    }
                    split.hardware_count = split.record_count;
                    lists.items.push_back(split);
                    filed.push_back(std::move(entry_set));
                    continue;
                }
                for (std::size_t index = 0u; index < item.records.size(); ++index) {
                    auto const projection = item.cached_projections.empty()
                                                ? project(item.records[index], item.model, camera, lists.grid)
                                                : item.cached_projections[index];
                    bool const subpixel = projection.finite && projection.front_of_near &&
                                          (metric == routing_metric::triangle_edge ? projection.triangle_edge_upper_px : projection.diameter_upper_px) < static_cast<double>(threshold_px);
                    entry_set.software[index] = subpixel ? 1u : 0u;
                    if (item.cached_projections.empty()) {
                        entry_set.projection_storage[index] = projection;
                    }
                    if (subpixel) {
                        ++split.software_count;
                        split.software_triangles += item.records[index].index_count / 3u;
                    } else {
                        ++split.hardware_count;
                        split.hardware_triangles += item.records[index].index_count / 3u;
                    }
                }
                lists.cluster_count += split.software_count;
                // PASS 1: exact per-tile counts (no capacity to check against - the only check is the global budget)
                for (std::size_t index = 0u; index < item.records.size(); ++index) {
                    if (entry_set.software[index] == 0u) {
                        continue;
                    }
                    projected_record const& projection = entry_set.projections[index];
                    if (!projection.on_screen) {
                        continue; // a subpixel record off the frame covers no tile (still counted/moved)
                    }
                    for (std::int32_t tile_y = projection.tile_min_y; tile_y <= projection.tile_max_y; ++tile_y) {
                        for (std::int32_t tile_x = projection.tile_min_x; tile_x <= projection.tile_max_x; ++tile_x) {
                            std::size_t const tile = static_cast<std::size_t>(tile_y) * lists.grid.tiles_x + static_cast<std::size_t>(tile_x);
                            ++lists.counts[tile];
                            ++total_entries;
                        }
                    }
                }
                if (total_entries > entry_budget) {
                    // GLOBAL budget: one capacity, one failure mode (spec §5.4). The caller runs the complete
                    // hardware path; no partial CSR list is ever produced.
                    return std::unexpected{software_error::entry_budget_exceeded};
                }
                lists.items.push_back(split);
                filed.push_back(std::move(entry_set));
            }
            // ---- the exclusive prefix sum: offsets[t] is tile t's first entry, offsets[tiles] is entry_count ----
            lists.offsets.assign(static_cast<std::size_t>(tiles) + 1u, 0u);
            for (std::uint32_t tile = 0u; tile < tiles; ++tile) {
                lists.offsets[tile + 1u] = lists.offsets[tile] + lists.counts[tile];
            }
            lists.entry_count = lists.offsets[tiles];
            lists.entries.assign(lists.entry_count, tile_entry{});
            // ---- PASS 2: fill tile ranges in item -> run -> tile order ----
            deren::utility::vessel::vector<std::uint32_t> cursor = lists.offsets; // per-tile write head
            for (std::size_t item_position = 0u; item_position < filed.size(); ++item_position) {
                filed_item const& entry_set = filed[item_position];
                item_split const& split = entry_set.split;
                std::uint32_t suffix = 0u;
                for (std::size_t index = 0u; index < entry_set.software.size(); ++index) {
                    if (entry_set.software[index] == 0u) {
                        continue;
                    }
                    std::uint32_t const meshlet_id = split.hardware_count + suffix;
                    ++suffix;
                    projected_record const& projection = entry_set.projections[index];
                    if (!projection.on_screen) {
                        continue;
                    }
                    for (std::int32_t tile_y = projection.tile_min_y; tile_y <= projection.tile_max_y; ++tile_y) {
                        for (std::int32_t tile_x = projection.tile_min_x; tile_x <= projection.tile_max_x; ++tile_x) {
                            std::size_t const tile = static_cast<std::size_t>(tile_y) * lists.grid.tiles_x + static_cast<std::size_t>(tile_x);
                            lists.entries[cursor[tile]++] = tile_entry{.item_index = split.item_index, .meshlet_id = meshlet_id};
                        }
                    }
                }
            }
            for (std::uint32_t const count : lists.counts) {
                lists.max_tile_entries = std::max(lists.max_tile_entries, count);
            }
            return lists;
        }
    } // namespace detail

    /**
     * @brief THE PRODUCTION TILE LIST BUILDER: bin the frame's subpixel records into the CSR sparse list, or refuse
     *        the frame
     * @param items the frame's classified items (the same span the split uses)
     * @param parameters the frame's camera (view/proj/extent)
     * @param threshold_px the subpixel threshold (1.0 is the primary band)
     * @param entry_budget the global CSR entry budget; the single capacity of this design (`software_entry_budget`)
     * @return the CSR plan, or the named reason this frame must run the COMPLETE hardware path
     * @note `entry_budget_exceeded` is an ERROR rather than a truncated list: a partial list would silently drop
     *       visible subpixel clusters (spec §5.4). A zero budget is refused as invalid; every overflow decision is
     *       deterministic because the two passes walk the same item -> run -> tile order.
     */
    [[nodiscard]] inline std::expected<software_tile_lists, software_error> build_tile_lists(
        std::span<item_records const> const items, camera_parameters const& parameters, float const threshold_px,
        std::uint32_t const entry_budget = software_entry_budget, routing_metric metric = routing_metric::cluster_diameter) {
        if (!std::isfinite(threshold_px) || threshold_px <= 0.0f) {
            return std::unexpected{software_error::invalid_threshold};
        }
        if (entry_budget == 0u) {
            return std::unexpected{software_error::invalid_capacity};
        }
        auto const camera = prepare_camera(parameters);
        if (!camera.has_value()) {
            return std::unexpected{camera.error()};
        }
        return detail::bin_csr_tile_lists(items, *camera, threshold_px, entry_budget, metric);
    }

} // namespace deren::engine::visibility::software
