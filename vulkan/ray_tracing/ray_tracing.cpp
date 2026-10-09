// THE STRUCTURE PHASE'S IMPLEMENTATION: see the header for why this is a value the renderer owns rather than a
// pass. The two functions below are the ones that used to be `runtime::record_acceleration_structures` and
// `runtime::record_top_level_structure`, moved with their reasoning; the only changes are that the knobs and the
// two jobs arrive as `build_inputs` and that the failures are RETURNED rather than logged and swallowed, because
// what a failure means for a knob is the renderer's decision.
module;

#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

module deren.vulkan.ray_tracing;

import deren.utility;

namespace deren::vulkan::ray_tracing {

    namespace rhi = deren::promise::rhi;

    namespace {
        /// ... and the address ability the same way (`device_address` is its own tier-2 ability).
        rhi::device_address* address_of(rhi::api_core& face) {
            return static_cast<rhi::device_address*>(face.query_extension(rhi::extension_kind::device_address));
        }

        // `native_buffer_of` STOOD HERE, and its callers were the micromap's storage and the two structure
        // storages - all of them the BACKEND's own objects since plan S1's P2/P4, so this module never narrows a
        // buffer handle any more.

        /// The device address of a contract buffer created with `rhi::buffer_flag::device_address`; 0 when
        /// the address could not be answered (the flag was not set, or the ability is not announced).
        std::uintptr_t buffer_address_of(rhi::api_core& face, rhi::buffer const& buffer) {
            auto* const addresses = address_of(face);
            return addresses == nullptr ? 0 : static_cast<std::uintptr_t>(addresses->buffer_address(buffer, 0));
        }

        /// The pair every buffer this module builds a structure FROM carries: an address, and the
        /// acceleration-structure build-input capability (the renderer's own `build_input_usage`).
        constexpr rhi::buffer_flags build_input_flags = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::acceleration_structure_input);

        // `addressable_flag` STOOD HERE for the micromap scratch and storage; the BACKEND allocates both now
        // (and it knows which flags they need), so this module declares only `build_input_flags` below - what a
        // CALLER's geometry buffer must carry.
    } // namespace

    structure_set::structure_set(rhi::api_core& face) noexcept
        : contract(&face) {
    }

    bool structure_set::attempted() const noexcept {
        return this->build_attempted;
    }

    bool structure_set::ready() const noexcept {
        return this->bottom.has_value() && this->top_level.has_value();
    }

    std::uint64_t structure_set::structure_size(uint32_t const frame_slot) const noexcept {
        // The top level is `top_level`'s object (see the member block in the header): this is the forwarding half,
        // for the descriptor heap, whose acceleration-structure descriptor is an address range that must carry a
        // REAL size (see docs/descriptor_heap_migration.md - the heap's payload union has no AS member).
        return this->top_level.has_value() ? this->top_level->structure_size(frame_slot) : 0;
    }

    std::uint64_t structure_set::instance_table_size(uint32_t const frame_slot) const noexcept {
        // ... and the same for the instance table at binding 17, which the set path may write with VK_WHOLE_SIZE
        // and a heap range may not.
        return this->top_level.has_value() ? this->top_level->instance_table_size(frame_slot) : 0;
    }

    deren::promise::rhi::acceleration_structure* structure_set::handle(uint32_t const frame_slot) const noexcept {
        // TIER-1 SINCE ABI 26: the handle IS the object now, so a caller that needs its address or its size asks
        // `device_address()` / `size_bytes()` instead of narrowing a `VkAccelerationStructureKHR` and resolving
        // an entry point to read an address out of it - which is exactly what runtime.cpp used to do.
        return this->top_level.has_value() ? this->top_level->structure(frame_slot) : nullptr;
    }

    rhi::buffer const* structure_set::instance_table_buffer(uint32_t const frame_slot) const noexcept {
        // Forwarded, not re-derived (the table is `top_level`'s), and nullable for the same reason the
        // accessor it forwards is: "this slot has no top level structure yet" is a state the caller has to
        // be able to see through the nullable contract buffer.
        return this->top_level.has_value() ? this->top_level->instance_table_buffer(frame_slot) : nullptr;
    }

    std::span<caster_level const> structure_set::casters() const noexcept {
        return this->caster_list;
    }

    void structure_set::release_micromaps() noexcept {
        // THE OBJECTS RELEASE THEMSELVES NOW (plan S1's P4): each `micromap_resource` holds a contract owner, so
        // its destruction is the backend's `release()` - the structure, its storage, its scratch and the setup
        // buffers all go with it. What stood here was a `vkDestroyMicromapEXT` loop over an entry point this
        // module resolved itself.
        this->micromap_resources.clear();
    }
    structure_set::~structure_set() {
        this->release_micromaps();
    }

    void structure_set::abandon() noexcept {
        this->bottom.reset();
        this->top_level.reset();
        // The expansion buffers and the caster map go with the structures they belong to: a stale mapping would
        // have the instance list read geometry no structure was built from.
        this->mask_buffers.clear();
        this->skin_buffers.clear();
        this->skin_levels.clear();
        this->caster_list.clear();
        // The micromaps go too, and they are the ONE thing here that does not free itself: VkMicromapEXT has no
        // RAII wrapper in this project, so the handle is destroyed explicitly before the buffers that back it.
        this->release_micromaps();
    }

    /**
     * @brief create and build ONE opacity micromap for a MASK caster's triangles
     *
     * WHAT IS IN IT, for this first step: every micro-triangle is written as UNKNOWN. A 4-state micromap at
     * subdivision level 0 has one micro-triangle per triangle (two bits, so one byte of the packed array per
     * triangle, which this writes with a 4-byte stride to keep every dataOffset 4-byte aligned), and an unknown
     * micro-triangle is what makes the traversal invoke the any-hit shader - so a micromap that says "unknown"
     * everywhere leaves the image EXACTLY as it was and can be attached, built and debugged on its own. That is
     * the point of this step: create, size, allocate, build and synchronize the object, with an acceptance that
     * cannot be confused with a rendering change.
     *
     * WHY THE LAYOUT IS EXPLICIT: `triangleArray` carries a per-triangle {dataOffset, subdivisionLevel, format}
     * record, which is the one layout this pass controls completely (the alternative, usage counts alone, fixes
     * the packing) and the one the spec documents byte for byte: 4-byte offset, then 2-byte level, then 2-byte
     * format.
     *
     * @param vk the device (and its allocator)
     * @param triangle_count the caster's triangles; 0 makes this a no-op that returns nothing
     * @return the resource, or nullopt when the device does not publish the entry points or an allocation fails
     */
    // ---- ONE MICROMAP, THROUGH THE TIER-1 INTERFACE (plan S1's P4) ---------------------------------------
    //
    // WHAT THIS FUNCTION IS NOW: it decides WHAT the micromap says - every micro-triangle UNKNOWN, which is
    // what makes the any-hit shader decide (see the note above) - and hands the contract's description to the
    // backend. The parts that used to be here and are NOT any more: the entry points, the 256-byte address
    // alignment, the three setup buffers and their mappings, the size queries, the storage and the scratch.
    // The backend owns all of it, so a caller cannot get the alignment wrong and a second backend can lay the
    // whole thing out differently.
    std::optional<structure_set::micromap_resource> make_micromap(rhi::api_core& vk, uint32_t const triangle_count) {
        if (triangle_count == 0) {
            return std::nullopt;
        }
        // The two attribute records, both 4-byte-per-triangle so the dataOffsets are aligned: 0x03 is the 4-state
        // "unknown" pair (see the spec's Ray Opacity Micromap table), one micro-triangle per triangle.
        constexpr uint32_t data_stride = 4u;
        constexpr uint8_t unknown_state = 0x03u;
        std::vector<std::byte> const data(static_cast<std::size_t>(triangle_count) * data_stride, static_cast<std::byte>(unknown_state));
        std::vector<deren::promise::rhi::micromap_triangle> const triangles = [triangle_count] {
            std::vector<deren::promise::rhi::micromap_triangle> records(static_cast<std::size_t>(triangle_count));
            for (uint32_t i = 0; i < triangle_count; ++i) {
                records[i] = deren::promise::rhi::micromap_triangle{
                    .data_offset = i * data_stride,
                    .subdivision_level = 0u,
                    .format = static_cast<std::uint16_t>(deren::promise::rhi::micromap_format::four_state),
                };
            }
            return records;
        }();
        std::vector<uint32_t> const indices(static_cast<std::size_t>(triangle_count), 0u); // identity is not needed: one micromap triangle each

        structure_set::micromap_resource out;
        out.handle = deren::promise::rhi::object_manager<deren::promise::rhi::micromap>{vk.create_micromap(deren::promise::rhi::micromap_desc{
            .triangle_count = triangle_count,
            .data = data,
            .data_stride = data_stride,
            .triangles = triangles,
            .indices = indices,
            .format = deren::promise::rhi::micromap_format::four_state,
        })};
        if (!out.handle) {
            return std::nullopt; // the backend logged why (no VK_EXT_opacity_micromap, or an allocation failed)
        }
        out.triangle_count = triangle_count;
        return out;
    }
    acceleration_structure::geometry_source structure_set::caster_geometry(primitive const& caster,
                                                                           std::uintptr_t const source_vertex_address,
                                                                           std::uintptr_t const source_index_address,
                                                                           std::uintptr_t const mask_address,
                                                                           uint32_t const mask_stride,
                                                                           std::uintptr_t const skin_address,
                                                                           uint32_t const skin_stride,
                                                                           micromap_resource const* const micromap) const noexcept {
        // The three cases, in the order they take precedence: a MASK bake replaces the geometry entirely (an
        // expanded, NON-INDEXED triangle list), a skinned caster keeps the primitive's index buffer because its
        // vertex ORDER is unchanged, and everything else is the primitive's own memory.
        acceleration_structure::geometry_source source = {};
        if (mask_address != 0) {
            source = acceleration_structure::geometry_source{.vertex_address = mask_address,
                                                             .vertex_stride = mask_stride,
                                                             .vertex_count = caster.draw_index_count,
                                                             .index_address = 0,
                                                             .index_type = caster.index_type,
                                                             .index_count = caster.draw_index_count};
        } else if (skin_address != 0) {
            source = acceleration_structure::geometry_source{.vertex_address = skin_address,
                                                             .vertex_stride = skin_stride,
                                                             .vertex_count = caster.vertex_count,
                                                             .index_address = source_index_address,
                                                             .index_type = caster.index_type,
                                                             .index_count = caster.draw_index_count};
        } else {
            source = acceleration_structure::geometry_source{.vertex_address = source_vertex_address,
                                                             .vertex_stride = caster.vertex_stride,
                                                             .vertex_count = caster.vertex_count,
                                                             .index_address = source_index_address,
                                                             .index_type = caster.index_type,
                                                             .index_count = caster.draw_index_count};
        }
        // THE MICROMAP RIDES ALONG WHATEVER GEOMETRY WAS CHOSEN, because it is indexed by the triangle rather than
        // by a vertex: the geometry's primitive `i` is the same triangle the micromap's element `i` describes, so
        // it applies to the baked, the skinned and the plain case alike. That is also why the micromap is built
        // from the CASTER's triangle count rather than from either of those buffers' vertex count.
        if (micromap != nullptr) {
            // ONE HANDLE (plan S1's P4): the micromap owns its usage record and the index array the traversal
            // reads, so the four fields this used to copy across - an address, a stride, an index type and a
            // usage record - are gone from the contract and from this line.
            source.opacity_micromap = micromap->handle.get();
        }
        return source;
    }

    std::expected<void, failure> structure_set::build(rhi::command_buffer& commands, build_inputs const& inputs) {
        // BUILT ONCE, SUCCESS OR FAILURE: a device that refused the build is not asked again, and no log repeats.
        if (this->build_attempted) {
            return {};
        }
        this->build_attempted = true;

        rhi::api_core& vk = *this->contract;
        // Builds, barriers and bake callbacks share the caller-owned contract buffer.
        auto const start = std::chrono::steady_clock::now();
        this->bottom.emplace(vk);
        // The top level structure is per FRAME SLOT (see its class docs): with frames in flight one buffer would
        // be rewritten by the frame being recorded while the previous one still reads it.
        this->top_level.emplace(vk, deren::promise::rhi::max_frames_in_flight);
        auto& structures = *this->bottom;

        uint32_t skipped_no_address = 0;
        uint32_t skipped_no_stride = 0;
        // The mask bake: how many casters had an alphaMode MASK baked into their geometry, and how many could not
        // be (an allocation failure falls back to the documented solid behaviour rather than failing the build).
        uint32_t mask_baked = 0;
        uint32_t skipped_mask_buffers = 0;
        bool mask_bakes_recorded = false;
        // ... and the same two counters for the skinned casters (see the SKINNED branch below).
        uint32_t skinned_baked = 0;
        uint32_t skipped_skin_buffers = 0;
        // ... and the micromaps: how many were created and built, how many triangles they describe, and how many
        // casters could not have one (an allocation or a missing entry point, which is again a log line rather
        // than a failed build).
        uint32_t micromap_triangles = 0;
        uint32_t skipped_micromaps = 0;
        for (primitive const* caster : inputs.casters) {
            if (caster == nullptr) {
                continue;
            }
            if (!caster->vertex_buffer || !caster->index_buffer) {
                ++skipped_no_address;
                continue;
            }
            // A buffer only has a device address when it was created with SHADER_DEVICE_ADDRESS_BIT, which the
            // primitive uploads add when this device has the extensions - so this is a check on a device that has
            // ray queries but whose buffers were uploaded before the flag... which cannot happen: the buffers are
            // uploaded with the bits whenever the device supports them, regardless of the config. Kept as a guard
            // because the alternative is a validation error per frame instead of one line in the log.
            if (caster->vertex_stride == 0) {
                ++skipped_no_stride;
                continue;
            }
            std::uintptr_t const source_vertex_address = buffer_address_of(vk, *caster->vertex_buffer);
            std::uintptr_t const source_index_address = buffer_address_of(vk, *caster->index_buffer);

            // alphaMode MASK: bake the material's holes into an EXPANDED copy of this caster's vertices and build
            // the structure from that. An inline ray query has no any-hit stage, so a traversal cannot run the
            // material's discard - this bake is where the mask is applied instead, and it is startup work because
            // the structures are built once and a MASK material is a property of the file (see
            // shaders/mask_bake.slang for the rule and for what the mechanism cannot represent).
            //
            // WHAT IS THE POLICY HERE and what is the JOB's: which casters carry a MASK material is this loop's,
            // and the allocation of the expanded buffer each one is baked into is this object's (the buffer
            // outlives the loop: the build below reads it, and hit shading reads its vertices through the instance
            // table for as long as the structures live). The pipeline, its layout, the set it binds and the
            // dispatch are the JOB's (vulkan.pass.mask_bake_job), reached through the hook.
            // ---- THE OPACITY MICROMAP for this caster, created HERE and independently of the bake above ----
            //
            // The bake is off by default and the micromap is the mechanism that replaces it, so this must not sit
            // behind the same gate: it is created for every caster that carries an alphaMode MASK material, which
            // is the same test the bake's rule makes ("material_record::flags bit 4").
            //
            // MEASURED, AND IT IS A NEGATIVE RESULT WORTH KEEPING: the attachment is legal and validation is
            // silent, but on this device (NVIDIA RTX 4060, 591.59.0.0) the micromap does NOT change traversal.
            // Two arms, same config and pose, on the AlphaBlendModeTest asset (3 MASK materials, 6 triangles):
            //   - all-UNKNOWN 4-state (what this writes) against no micromap at all: byte-identical, which is
            //     expected by construction - an unknown micro-triangle is the state that asks the any-hit shader;
            //   - all-TRANSPARENT 2-state against the same, WITH THE ANY-HIT NEUTRALISED so that only the micromap
            //     could decide: byte-identical as well (123.11 mean both ways, mean|d| = 0.0000). A transparent
            //     micro-triangle must skip the hit entirely, so this says the micromap is not being CONSULTED
            //     rather than that its content is wrong - the other 2-state value is opaque, and an all-opaque
            //     micromap would have reverted the any-hit's cut, which is not what happens either.
            // BOTH WERE THEN FALSIFIED BY MEASUREMENT, one arm each and each byte-identical to the same arm with no
            // micromap at all: dropping gl_RayFlagsTerminateOnFirstHitEXT (123.11 both ways, mean|d| = 0.0000) and
            // adding gl_RayFlagsForceOpacityMicromap2StateEXT (mean|d| = 0.0000 again), validation silent in both.
            // What is left to try, in the order worth trying: gl_RayFlagsForceOpacityMicromap2StateEXT (the flag
            // the spec provides for exactly this mechanism); dropping gl_RayFlagsTerminateOnFirstHitEXT for one
            // arm, since a traversal that may stop at the first hit can take a path that never asks about opacity;
            // and a scene with a larger MASK footprint, because six triangles from three small quads is a weak
            // instrument. Until one of those shows an effect, what makes MASK surfaces correct is the any-hit
            // stage's own cut (shaders/rt_shadow.rahit, 3.0/255 from the raster shadow over the pixels it changes
            // against 136.6/255 without it), and this micromap is architecture that is in place and verified to be
            // LEGAL rather than a working feature. It costs one build per MASK caster at load and is inert after.
            uint32_t micromap_index = caster_level::micromap_none;
            if (caster->draw_index_count >= 3u) {
                uint32_t const material_index = caster->push.material_index.value;
                material_record const* const material = material_index < inputs.materials.size() ? &inputs.materials[material_index] : nullptr;
                if (material != nullptr && (material->flags & 16u) != 0u) {
                    if (auto resource = make_micromap(vk, caster->draw_index_count / 3u); resource.has_value()) {
                        micromap_triangles += resource->triangle_count;
                        micromap_index = static_cast<uint32_t>(this->micromap_resources.size());
                        this->micromap_resources.push_back(std::move(*resource));
                    } else {
                        ++skipped_micromaps;
                    }
                }
            }

            std::uintptr_t mask_address = 0;
            uint32_t mask_stride = 0;
            if (inputs.mask_bake && inputs.hooks.mask_ready != nullptr && inputs.hooks.mask_ready(inputs.hooks.owner)) {
                // material_record::flags bit 4 is alphaMode MASK (see vulkan/primitive.cppm; the bits are literals
                // in register_material, so they are literals here too).
                uint32_t const material_index = caster->push.material_index.value;
                material_record const* const material = material_index < inputs.materials.size() ? &inputs.materials[material_index] : nullptr;
                if (material != nullptr && (material->flags & 16u) != 0u && caster->draw_index_count >= 3u) {
                    // Three vertices per triangle, 32 bytes each: position(3) + normal(3) + uv(2), which is what
                    // the hit shading reads (offsets 0, 3 and 6). GPU-only and never mapped - the bake fills it and
                    // the build reads it.
                    constexpr uint32_t mask_vertex_stride = 32u;
                    uint64_t const expanded_bytes = static_cast<uint64_t>(caster->draw_index_count) * mask_vertex_stride;
                    rhi::object_manager<rhi::buffer> expanded{vk.create_buffer(
                        rhi::buffer_desc{.size = expanded_bytes, .usage = rhi::buffer_usage::storage_gpu_only, .flags = build_input_flags})};
                    // The factory answering non-null IS the old "the allocator has a detail record" test:
                    // `create_buffer()` only returns an object after the allocation succeeded, and it is the
                    // contract's own way of saying the descriptor was honoured.
                    if (expanded) {
                        mask_address = buffer_address_of(vk, *expanded);
                        mask_stride = mask_vertex_stride;
                        inputs.hooks.record_mask_bake(inputs.hooks.owner,
                                                      commands,
                                                      pass::mask_bake_request{
                                                          .source_vertices = source_vertex_address,
                                                          .source_indices = source_index_address,
                                                          .destination = mask_address,
                                                          .source_stride = caster->vertex_stride,
                                                          .destination_stride = mask_vertex_stride,
                                                          .index_type = static_cast<uint32_t>(caster->index_type),
                                                          .triangle_count = caster->draw_index_count / 3u,
                                                          .material_index = material_index,
                                                      });
                        mask_bakes_recorded = true;
                        ++mask_baked;
                        // The buffer outlives this loop: the build below reads it, and a hit's shading reads its
                        // vertices through the instance table for as long as the structures live.
                        this->mask_buffers.push_back(std::move(expanded));
                    } else {
                        ++skipped_mask_buffers;
                    }
                }
            }

            // SKINNED: the job writes this caster's deformed vertices into a buffer of its own, the structure is
            // built from that buffer, and every frame after it is REFITTED - which is legal because the vertex
            // order, the index buffer and the triangle count are all the primitive's own: only the bytes change.
            // `skin_base != 0` is the test for "skinned", because index 0 is the identity block every unskinned
            // draw uses (see set_skin_matrices). The stride test is the shader's precondition, not a heuristic:
            // shaders/compute_skin.slang reads the joints at byte 32 and the weights at byte 48 of the engine's
            // 64-byte interleaved vertex, so a caster whose vertices are packed differently is REFUSED (it keeps
            // its bind pose and is counted in the log) rather than skinned with the wrong words.
            constexpr uint32_t skin_source_stride_expected = 64u;
            std::uintptr_t skin_address = 0;
            uint32_t skin_stride = 0;
            uint32_t skin_source_stride = 0;
            uint32_t skin_vertex_count = 0;
            uint32_t skin_base = 0;
            if (mask_address == 0 && inputs.skin_bake && inputs.hooks.skin_ready != nullptr && inputs.hooks.skin_ready(inputs.hooks.owner) &&
                caster->push.skin_base != 0 && caster->vertex_count != 0 && caster->vertex_stride == skin_source_stride_expected) {
                constexpr uint32_t skin_vertex_stride = 32u; // position, normal, uv - what hit shading reads
                uint64_t const skinned_bytes = static_cast<uint64_t>(caster->vertex_count) * skin_vertex_stride;
                rhi::object_manager<rhi::buffer> skinned_vertices{vk.create_buffer(
                    rhi::buffer_desc{.size = skinned_bytes, .usage = rhi::buffer_usage::storage_gpu_only, .flags = build_input_flags})};
                if (skinned_vertices) {
                    skin_address = buffer_address_of(vk, *skinned_vertices);
                    skin_stride = skin_vertex_stride;
                    skin_source_stride = caster->vertex_stride;
                    skin_vertex_count = caster->vertex_count;
                    skin_base = caster->push.skin_base;
                    this->skin_buffers.push_back(std::move(skinned_vertices));
                    ++skinned_baked;
                } else {
                    ++skipped_skin_buffers;
                }
            }

            acceleration_structure::geometry_source const source =
                this->caster_geometry(*caster,
                                      source_vertex_address,
                                      source_index_address,
                                      mask_address,
                                      mask_stride,
                                      skin_address,
                                      skin_stride,
                                      micromap_index != caster_level::micromap_none ? &this->micromap_resources[micromap_index] : nullptr);
            // A skinned structure is built ALLOW_UPDATE so the per-frame refit is legal; everything else is built
            // once and never touched again.
            auto const added = structures.add(source, skin_address != 0);
            if (!added) {
                this->abandon();
                return std::unexpected(failure{.message = added.error()});
            }
            // Remember which caster got which index: the per-frame instance list walks THIS, so a caster that was
            // skipped above is skipped there too and the two walks cannot disagree. The mask and skin addresses
            // ride along, because that list is what a hit's shading reads the geometry through - a baked or
            // skinned caster must be read from the copy it was built from.
            this->caster_list.emplace_back(caster_level{.caster = caster,
                                                        .blas_index = added.value(),
                                                        .mask_stride = mask_stride,
                                                        .mask_vertex_address = mask_address,
                                                        .skin_source_address = source_vertex_address,
                                                        .skin_destination_address = skin_address,
                                                        .skin_source_stride = skin_source_stride,
                                                        .skin_destination_stride = skin_stride,
                                                        .skin_vertex_count = skin_vertex_count,
                                                        .skin_base = skin_base,
                                                        .micromap_index = micromap_index});
        }

        // The skinned casters' first skinning pass, recorded here because the BUILD below has to read skinned
        // vertices - and every frame after this one re-skins and REFITS in update(). The refit is not recorded
        // here: this is the frame the structures are created, and a refit against a structure that does not exist
        // yet is illegal.
        if (skinned_baked != 0 && inputs.hooks.record_skin != nullptr) {
            for (auto const& built : this->caster_list) {
                if (built.skin_destination_address != 0) {
                    this->skin_levels.push_back(built.blas_index);
                }
            }
            static_cast<void>(inputs.hooks.record_skin(inputs.hooks.owner, commands, this->caster_list));
        }

        // Every bake wrote a buffer the build below reads: one barrier covers them all, because every dispatch is
        // recorded before the first build (add() only sizes and allocates; record_build() records). A compute
        // WRITE is not visible to an acceleration structure build without it, and the symptom would be a structure
        // built from an empty buffer - i.e. geometry that stops casting.
        if (mask_bakes_recorded) {
            // THE BUILD ORDERING RIDES THE CONTRACT NOW (the tenth batch): the pair is the measurement, and it
            // is the SAME pair `vulkan/pass/compute_skin.cpp` records - (shader_write, acceleration_structure_read)
            // is exactly the raw masks this replaced (COMPUTE_SHADER/SHADER_WRITE ->
            // ACCELERATION_STRUCTURE_BUILD/SHADER_READ). `command_buffer` is the contract buffer this build
            // records into, so the memory barrier is a `barrier_group` with `has_memory`.
            deren::promise::rhi::barrier_group const bake_order{
                .struct_size = sizeof(deren::promise::rhi::barrier_group),
                .images = {},
                .buffers = {},
                .stage = deren::promise::rhi::stage_hint::none,
                .has_memory = true,
                .memory = deren::promise::rhi::memory_barrier{.from = deren::promise::rhi::buffer_use::shader_write,
                                                              .to = deren::promise::rhi::buffer_use::acceleration_structure_read},
            };
            if (commands.barrier(bake_order) != deren::promise::rhi::error::ok) {
                deren::utility::log("ray tracing: the mask bake's build-ordering barrier was refused");
            }
        }

        // ---- THE MICROMAP BUILDS, recorded here because the structure builds below READ them ----
        //
        // The two barriers are the ones the spec names for exactly this pair of operations, and they are not
        // symmetric: the micromap's INPUT buffers were written by the HOST (they are host-visible and coherent, and
        // filled at setup), so they need HOST_WRITE -> MICROMAP_BUILD/SHADER_READ; the micromap itself is written
        // by MICROMAP_BUILD/MICROMAP_WRITE and read by ACCELERATION_STRUCTURE_BUILD/MICROMAP_READ, which is what
        // makes the attachment in the next step legal. Getting the first one wrong reads a micromap built from
        // memory the host had not published; getting the second wrong reads a micromap that is still being built.
        if (!this->micromap_resources.empty()) {
            // THE BUILDS ARE THE CONTRACT'S NOW (plan S1's P4): one `build_micromap` per micromap, and the
            // backend records its own HOST_WRITE -> MICROMAP_BUILD barrier - which is why the raw
            // `vkCmdPipelineBarrier2` that stood here is gone (it was this module's last Vulkan call).
            // THE ORDER IS THE CALLER'S AND IT MATTERS: every micromap is built before the geometry that
            // consults it, which is the order below.
            bool micromap_build_refused = false;
            for (micromap_resource const& resource : this->micromap_resources) {
                if (!resource.handle) {
                    continue;
                }
                if (commands.build_micromap(*resource.handle) != deren::promise::rhi::error::ok) {
                    micromap_build_refused = true;
                }
            }
            // THE SECOND HALF RIDES THE CONTRACT (the micromap roles are values the `buffer_use` enum carries):
            // (micromap_write, micromap_read) is exactly this barrier's mask pair
            // (MICROMAP_BUILD/MICROMAP_WRITE -> ACCELERATION_STRUCTURE_BUILD/MICROMAP_READ), and it is what makes
            // the attachment legal in the build that follows.
            deren::promise::rhi::barrier_group const micromaps_ready{
                .struct_size = sizeof(deren::promise::rhi::barrier_group),
                .images = {},
                .buffers = {},
                .stage = deren::promise::rhi::stage_hint::none,
                .has_memory = true,
                .memory = deren::promise::rhi::memory_barrier{.from = deren::promise::rhi::buffer_use::micromap_write,
                                                              .to = deren::promise::rhi::buffer_use::micromap_read},
            };
            if (commands.barrier(micromaps_ready) != deren::promise::rhi::error::ok) {
                deren::utility::log("ray tracing: the micromap build-ordering barrier was refused");
            }
            if (micromap_build_refused) {
                deren::utility::log("ray tracing: a micromap build was refused - the casters that consult one fall back to opaque");
            }
            deren::utility::log("ray-traced shadows: built {} opacity micromaps ({} triangles, subdivision level 0, 4-state, every micro-triangle UNKNOWN, {} casters skipped - so this step cannot change a pixel)",
                                this->micromap_resources.size(),
                                micromap_triangles,
                                skipped_micromaps);
        }
        if (auto const built = structures.record_build(commands); !built) {
            // The two structures go, the COPIES stay: this is the same asymmetry the renderer had, and it is kept
            // deliberately - a failed record of a build does not invalidate the buffers the map points at, and
            // dropping them would be a second, unrelated change of behaviour.
            this->bottom.reset();
            this->top_level.reset();
            return std::unexpected(failure{.message = built.error()});
        }

        acceleration_structure::build_stats const& stats = structures.last_stats();
        double const host_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        deren::utility::log("ray-traced shadows: built {} bottom level structures ({} triangles, {:.1f} MiB + {:.1f} MiB scratch) in {:.1f} ms",
                            stats.geometry_count,
                            stats.triangle_count,
                            static_cast<double>(stats.structure_bytes) / (1024.0 * 1024.0),
                            static_cast<double>(stats.scratch_bytes) / (1024.0 * 1024.0),
                            host_ms);
        if (skipped_no_stride != 0) {
            deren::utility::log("  {} casters skipped (no vertex stride recorded - a primitive not created by make_primitive)", skipped_no_stride);
        }
        if (skipped_no_address != 0) {
            deren::utility::log("  {} casters skipped (no vertex/index buffer)", skipped_no_address);
        }
        if (mask_baked != 0 || skipped_mask_buffers != 0) {
            // The measurement this feature is read with: how much geometry the mask actually removed is a property
            // of the asset (a two-quad MASK plane whose pattern is in the middle keeps every triangle; a vase of
            // flowers loses 40% of them).
            deren::utility::log("ray-traced shadows: {} MASK casters baked into their structures ({} could not be - those are still cut per hit by the any-hit stage)", mask_baked, skipped_mask_buffers);
        }
        if (skinned_baked != 0 || skipped_skin_buffers != 0) {
            // The skinned casters are re-skinned and REFITTED every frame (see update), so this count is also the
            // number of structures a frame's refit touches.
            deren::utility::log("ray-traced shadows: {} skinned casters re-skinned and REFITTED from their deformed vertices every frame ({} could not be - those keep their bind pose)", skinned_baked, skipped_skin_buffers);
        }
        return {};
    }

    std::expected<void, failure> structure_set::update(rhi::command_buffer& commands, uint32_t const frame_slot, build_inputs const& inputs) {
        if (!this->ready()) {
            return {}; // nothing was built (or the build failed): there is nothing to refit or to instance
        }
        rhi::api_core& vk = *this->contract;
        // Builds, barriers and bake callbacks share the caller-owned contract buffer.
        auto& levels = *this->bottom;
        auto& top = *this->top_level;

        // The skinned casters are deformed and their structures REFITTED here, before the instance list is walked
        // (the addresses do not change, so the order does not matter to correctness - but the refit has to be
        // recorded before this frame writes the scene block's binding 16, the ordering the mask bake's own-set
        // comment explains).
        if (inputs.skin_bake && !this->skin_levels.empty() && inputs.hooks.skin_ready != nullptr && inputs.hooks.skin_ready(inputs.hooks.owner)) {
            if (inputs.hooks.record_skin(inputs.hooks.owner, commands, this->caster_list)) {
                if (auto const updated = levels.record_update(commands, this->skin_levels); !updated) {
                    // Once, and off: a failure here would otherwise log every frame, and a refit is not something
                    // to keep attempting against structures the device refused. The knob is the CALLER's, so the
                    // decision travels back with the failure.
                    return std::unexpected(failure{.message = updated.error(), .disable_skin_bake = true});
                }
            }
        }

        if (auto const begun = top.begin(frame_slot); !begun) {
            return std::unexpected(failure{.message = begun.error()});
        }
        // The instance list is the caster set the shadow pass draws, with the world matrix the raster passes use
        // for each caster - the same matrix shadow_geometry_signature() hashes, which is why an animated or moved
        // caster is reflected here for free.
        for (auto const& built : this->caster_list) {
            primitive const* const caster = built.caster;
            // The addresses a hit-shading path reads the hit triangle from: the same buffers, and the same
            // device addresses, the bottom level build already used for this caster - so the triangle
            // a shader fetches with them IS the triangle the ray hit. They are the buffers' base addresses (the
            // build applies no offset), which is also what makes them legal as a buffer reference: a buffer's
            // address is aligned, an offset into one need not be.
            //
            // A baked or skinned caster is read from the copy its structure was built from: the mask bake's
            // expanded, non-indexed one (zero index address = a flat vertex list), or the skinned one, which keeps
            // the primitive's own index buffer because its vertex ORDER is unchanged.
            std::uintptr_t vertex_address = built.mask_vertex_address != 0 ? built.mask_vertex_address : built.skin_destination_address;
            std::uintptr_t index_address = 0;
            uint32_t vertex_stride = built.mask_vertex_address != 0 ? built.mask_stride : built.skin_destination_stride;
            if (vertex_address == 0) {
                vertex_address = buffer_address_of(vk, *caster->vertex_buffer);
                index_address = buffer_address_of(vk, *caster->index_buffer);
                vertex_stride = caster->vertex_stride;
            } else if (built.skin_destination_address != 0) {
                index_address = buffer_address_of(vk, *caster->index_buffer);
            }
            acceleration_structure::instance_source const instance = {
                .transform = caster->push.model,
                .blas_index = built.blas_index,
                .record = {.vertex_address = vertex_address,
                           .index_address = index_address,
                           .model = caster->push.model,
                           .vertex_stride = vertex_stride,
                           .index_type = static_cast<uint32_t>(caster->index_type),
                           .material_index = caster->push.material_index.value,
                           .primitive_index = built.blas_index},
            };
            if (auto const added = top.add(levels, instance); !added) {
                return std::unexpected(failure{.message = added.error()});
            }
        }

        // The top level reads the BOTTOM levels, and on the frame that creates them the two builds are in the same
        // command buffer with nothing between them: without this barrier the driver is free to run the second
        // build's reads against writes the first one has not published. It costs a no-op on every later frame
        // (nothing wrote a bottom level in this buffer), which is cheaper than a flag that would have to track
        // "which frame built them".
        // THE CONTRACT'S PAIR, AND THE VALUE IT NEEDED: (acceleration_structure_write,
        // acceleration_structure_read) is the measured mask pair (ACCELERATION_STRUCTURE_BUILD with the
        // extension's WRITE access -> the same stage with the READ access). `acceleration_structure_read` named
        // the reader already; the WRITER is the value appended with this site.
        deren::promise::rhi::barrier_group const build_order{
            .struct_size = sizeof(deren::promise::rhi::barrier_group),
            .images = {},
            .buffers = {},
            .stage = deren::promise::rhi::stage_hint::none,
            .has_memory = true,
            .memory = deren::promise::rhi::memory_barrier{.from = deren::promise::rhi::buffer_use::acceleration_structure_write,
                                                          .to = deren::promise::rhi::buffer_use::acceleration_structure_read},
        };
        if (commands.barrier(build_order) != deren::promise::rhi::error::ok) {
            deren::utility::log("ray tracing: the top-level build-ordering barrier was refused");
        }

        if (auto const built = top.record_build(commands); !built) {
            return std::unexpected(failure{.message = built.error()});
        }

        if (!this->top_logged) {
            this->top_logged = true;
            // The class measured the host cost of the build itself (see build_stats); reporting that rather than a
            // second timer around it keeps one definition of "what the build costs".
            deren::utility::log("ray-traced shadows: {} instances in the top level structure, one instance table entry each ({:.3f} ms host per frame)",
                                top.instance_count(frame_slot),
                                top.last_stats().build_ms);
        }
        return {};
    }

} // namespace deren::vulkan::ray_tracing
