module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <glm/glm.hpp>
#include <string>
#include <utility>

module deren.vulkan.acceleration_structure;

import deren.utility;

// ============================================================================
// THE ACCELERATION-STRUCTURE MODULE, AFTER PLAN S1's P1b-2.
//
// WHAT IT IS NOW: a THIN FRONT END over the tier-1 `rhi::acceleration_structure` interface. Everything that
// used to live here - the size query, the storage allocation, the shared scratch with its aligned per-geometry
// ranges, the instance buffers, the `vkCreateAccelerationStructureKHR`/`vkCmdBuildAccelerationStructuresKHR`
// calls, the entry points resolved through `vkGetDeviceProcAddr` - is the BACKEND's business now, because that
// is what makes the interface portable: a backend with no explicit acceleration structures at all can answer
// the same four verbs however it must.
//
// WHAT IT STILL OWNS, and this is the honest list: the SCENE-SHAPED knowledge - which geometries and instances
// exist, the per-geometry refit flag, the growth of a top level's capacity, and the instance table the SHADER
// reads (through `instanceCustomIndex`), which is the engine's own data and not a driver structure. It also
// still measures what a build cost, because the runtime reports that.
//
// THE ONE THING IT CANNOT EXPRESS YET is the opacity-micromap attachment: the contract's
// `acceleration_structure_geometry` has no field for it, so a geometry that carries one is REFUSED BY NAME
// rather than built without it (plan S1's P4 gives micromaps the same tier-1 treatment, at which point the
// attachment rides the description and this refusal goes away).
// ============================================================================

namespace deren::vulkan::acceleration_structure {
    namespace rhi = deren::promise::rhi;

    namespace {
        /// THE DEVICE'S INSTANCE LIMIT, from the `device_capabilities` ability: the check that used to need a
        /// properties query of this module's own. Zero means "the device did not answer", which the callers
        /// treat as "no limit known" - the allocation is the real constraint either way.
        std::uint64_t max_instances_of(rhi::api_core& face) {
            rhi::device_capabilities* const capabilities = rhi::query_extension<rhi::device_capabilities>(face);
            return capabilities == nullptr ? 0u : capabilities->max_acceleration_structure_instances();
        }

        /// THE WORLD MATRIX AS THE INSTANCE WANTS IT: three rows of four, ROW-major. glm is column-major
        /// (`m[column][row]`) and the instance record is row-major, so the indices swap - the one place a
        /// transposed instance would silently mirror the whole scene.
        void fill_instance_transform(rhi::acceleration_structure_instance& out, glm::mat4 const& matrix) noexcept {
            for (std::uint32_t row = 0; row < 3u; ++row) {
                for (std::uint32_t column = 0; column < 4u; ++column) {
                    out.transform[row * 4u + column] = matrix[column][row];
                }
            }
        }

        /// how many triangles a source describes (an unindexed geometry counts its vertices)
        [[nodiscard]] std::uint32_t triangle_count_of(geometry_source const& source) noexcept {
            return (source.index_address == 0 ? source.vertex_count : source.index_count) / 3u;
        }
    } // namespace

    // ---- bottom level structure ---------------------------------------------------------------------------

    bottom_level_structures::bottom_level_structures(rhi::api_core& face)
        : contract(&face) {
    }

    // The entries hold `object_manager<rhi::acceleration_structure>`, so the reference each structure carries is
    // given back here - and the BACKEND's `release()` is what destroys the driver's object, in the order its own
    // destructor documents.
    bottom_level_structures::~bottom_level_structures() = default;

    std::expected<uint32_t, std::string> bottom_level_structures::add(geometry_source const& source, bool const refittable) {
        if (this->contract == nullptr) {
            return std::unexpected(std::string("acceleration structure: this structure set has no contract face"));
        }
        // THE MICROMAP RIDES THE DESCRIPTION NOW (plan S1's P4): the contract's geometry carries the object, and
        // nothing here refuses it any more - the BACKEND owns the 256-byte address alignment, the setup buffers
        // and the index array the traversal reads, which is exactly what made this inexpressible before.
        entry item{};
        item.refittable = refittable;
        std::uint32_t const triangles = triangle_count_of(source);
        if (triangles == 0) {
            // A SOURCE WITH NO TRIANGLES IS SKIPPED, and its index is still taken so the caller's own arrays
            // stay aligned with this set's indices (see the class note).
            this->entries.push_back(std::move(item));
            this->stats.geometry_count += 1u;
            return static_cast<std::uint32_t>(this->entries.size() - 1u);
        }

        // THE GEOMETRY IN THE CONTRACT'S VOCABULARY: addresses as `uint64_t` (the shape
        // `shader_binding_table_region` uses), the index width as the contract's own enum - so the description
        // this hands over names no Vulkan type.
        rhi::acceleration_structure_geometry const geometry{
            .vertex_address = static_cast<std::uint64_t>(source.vertex_address),
            .vertex_stride = source.vertex_stride,
            .vertex_count = source.vertex_count,
            .index_address = static_cast<std::uint64_t>(source.index_address),
            .index_format = source.index_type,
            .index_count = source.index_count,
            .opacity_micromap = source.opacity_micromap,
        };
        item.structure = rhi::object_manager<rhi::acceleration_structure>{this->contract->create_acceleration_structure(
            rhi::acceleration_structure_desc{
                .type = rhi::acceleration_structure_type::bottom_level,
                .flags = refittable ? rhi::to_bits(rhi::acceleration_structure_flag::allow_update) : rhi::no_acceleration_structure_flags,
                .geometries = &geometry,
                .geometry_count = 1u,
            })};
        if (!item.structure) {
            return std::unexpected(std::string("acceleration structure: the backend could not create the bottom level structure "
                                               "(see its own log line for why)"));
        }

        this->stats.geometry_count += 1u;
        this->stats.triangle_count += triangles;
        this->stats.structure_bytes += item.structure->size_bytes();
        this->entries.push_back(std::move(item));
        return static_cast<std::uint32_t>(this->entries.size() - 1u);
    }

    std::expected<void, std::string> bottom_level_structures::record_build(rhi::command_buffer& commands) {
        std::chrono::steady_clock::time_point const start = std::chrono::steady_clock::now();
        for (entry const& item : this->entries) {
            if (!item.structure) {
                continue; // a geometry with no triangles: nothing was created, so there is nothing to build
            }
            // ONE CALL PER STRUCTURE, where this module used to batch every build into one
            // `vkCmdBuildAccelerationStructuresKHR`. That batching was a CPU-side win and its own note says so;
            // what it cost was the BACKEND owning the scratch, which is exactly the trade this interface makes.
            if (rhi::error const recorded = commands.build_acceleration_structure(*item.structure); recorded != rhi::error::ok) {
                return std::unexpected(std::string("acceleration structure: recording a bottom level build failed with error ") + std::to_string(static_cast<std::uint32_t>(recorded)));
            }
        }
        this->stats.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return {};
    }

    std::expected<void, std::string> bottom_level_structures::record_update(rhi::command_buffer& commands, std::span<uint32_t const> const indices) {
        for (std::uint32_t const index : indices) {
            if (index >= this->entries.size()) {
                continue;
            }
            entry const& item = this->entries[index];
            if (!item.structure || !item.refittable) {
                continue; // not built, or built without ALLOW_UPDATE: a refit against it is not legal (and the
                          // backend refuses it by name anyway - see `refit_acceleration_structure`)
            }
            if (rhi::error const recorded = commands.refit_acceleration_structure(*item.structure); recorded != rhi::error::ok) {
                return std::unexpected(std::string("acceleration structure: recording a bottom level refit failed with error ") + std::to_string(static_cast<std::uint32_t>(recorded)));
            }
        }
        return {};
    }

    // ---- top level structure ------------------------------------------------------------------------------

    // Spelled exactly as the declaration in the module interface is: doxygen matches a definition against
    // its declaration by the written signature, and `std::uint32_t` here against `uint32_t` there made it
    // report "no matching class member found" for this one constructor (the only warning in the doc build).
    top_level_structure::top_level_structure(rhi::api_core& face, uint32_t const frame_slot_count)
        : contract(&face)
        , max_instances(max_instances_of(face))
        , slots(frame_slot_count) {
    }

    top_level_structure::~top_level_structure() = default;

    std::expected<void, std::string> top_level_structure::begin(std::uint32_t const frame_slot) {
        if (frame_slot >= this->slots.size()) {
            return std::unexpected(std::string("acceleration structures: frame slot out of range"));
        }
        this->current_slot = frame_slot;
        this->slots[frame_slot].count = 0;
        this->slots[frame_slot].pending.clear();
        return {};
    }

    std::expected<void, std::string> top_level_structure::add(bottom_level_structures const& levels, instance_source const& source) {
        if (this->contract == nullptr || this->current_slot >= this->slots.size()) {
            return std::unexpected(std::string("acceleration structures: no frame slot is being built"));
        }
        rhi::acceleration_structure* const blas = levels.structure(source.blas_index);
        if (blas == nullptr) {
            return {}; // a geometry with no triangles: no instance, and the caller's indices stay put
        }
        slot& target = this->slots[this->current_slot];

        // THE STRUCTURE IS SIZED FOR ITS CAPACITY, so outgrowing the capacity means a NEW object (the backend
        // cannot grow one in place): doubling keeps that rare, and the instance limit the device reports is
        // checked before the allocation rather than after the driver's refusal.
        if (target.count >= target.capacity) {
            std::uint32_t const wanted = std::max(target.capacity * 2u, 256u);
            if (this->max_instances != 0u && static_cast<std::uint64_t>(wanted) > this->max_instances) {
                return std::unexpected(std::string("acceleration structures: the scene has more instances than the device allows in one top level structure"));
            }
            target.records = rhi::object_manager<rhi::buffer>{this->contract->create_buffer(rhi::buffer_desc{
                .size = static_cast<std::uint64_t>(wanted) * sizeof(instance_record),
                .usage = rhi::buffer_usage::storage_coherent,
                .flags = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::acceleration_structure_input)})};
            if (!target.records || target.records->mapped().data() == nullptr) {
                return std::unexpected(std::string("acceleration structures: the instance table could not be allocated"));
            }
            rhi::object_manager<rhi::acceleration_structure> next{this->contract->create_acceleration_structure(
                rhi::acceleration_structure_desc{.type = rhi::acceleration_structure_type::top_level, .instance_capacity = wanted})};
            if (!next) {
                return std::unexpected(std::string("acceleration structures: the backend could not create the top level structure"));
            }
            target.structure = std::move(next); // the old object's reference is dropped HERE, inside the backend
            target.capacity = wanted;
            target.pending.reserve(wanted);
            this->stats.structure_bytes += target.structure->size_bytes();
        }

        // THE INSTANCE, IN THE CONTRACT'S RECORD: the world transform, the reference a traversal follows
        // (`device_address()`), and the index the hit shader resolves through the instance table below.
        rhi::acceleration_structure_instance instance{};
        fill_instance_transform(instance, source.transform);
        instance.instance_custom_index = target.count;    // == the slot in the instance table
        instance.mask = 0xFFu;                            // visible to every ray: nothing here is ray-type specific
        instance.shader_binding_table_record_offset = 0u; // unused by a ray query (no SBT exists)
        instance.flags = rhi::acceleration_structure_instance_facing_cull_disable;
        instance.structure_reference = blas->device_address();
        target.pending.push_back(instance);

        // ... and the ENGINE's own record beside it, which is what a shader reads at a hit.
        std::memcpy(target.records->mapped().data() + static_cast<std::size_t>(target.count) * sizeof(instance_record),
                    &source.record,
                    sizeof(source.record));
        target.count += 1u;
        return {};
    }

    std::expected<void, std::string> top_level_structure::record_build(rhi::command_buffer& commands) {
        std::chrono::steady_clock::time_point const start = std::chrono::steady_clock::now();
        slot& target = this->slots[this->current_slot];
        if (target.count == 0u || !target.structure) {
            return {}; // an empty scene has an empty top level structure, and nothing to trace against
        }
        // THE RECORDS GO TO THE BACKEND FIRST, then the build records: `write_instances` is host work on the
        // backend's own memory, and the build reads it by address.
        if (rhi::error const written = target.structure->write_instances(target.pending); written != rhi::error::ok) {
            return std::unexpected(std::string("acceleration structures: writing the instance records failed with error ") + std::to_string(static_cast<std::uint32_t>(written)));
        }
        if (rhi::error const built = commands.build_acceleration_structure(*target.structure); built != rhi::error::ok) {
            return std::unexpected(std::string("acceleration structures: recording the top level build failed with error ") + std::to_string(static_cast<std::uint32_t>(built)));
        }
        this->stats.geometry_count = target.count;
        this->stats.build_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return {};
    }

    rhi::buffer const* top_level_structure::instance_table_buffer(std::uint32_t const frame_slot) const noexcept {
        // THE SAME SLOT, ANSWERED AS THE CONTRACT HANDLE, so a caller that needs the buffer's device address
        // asks the `device_address` ability instead of narrowing a handle itself.
        if (frame_slot >= this->slots.size()) {
            return nullptr;
        }
        slot const& target = this->slots[frame_slot];
        return target.records ? target.records.get() : nullptr;
    }
} // namespace deren::vulkan::acceleration_structure
