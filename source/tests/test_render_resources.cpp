// Headless unit tests: deren.engine.render_resource (pure CPU) ======================
// The description layer exists so that a pass's resources are stated ONCE and the layout, the descriptor
// writes and the pool counts are generated from that statement instead of being kept in agreement by hand.
// These tests pin the invariants that make that worth doing, and they can do it without a device - which is
// the point of keeping the layer Vulkan-free: the capture gate cannot run in CI at all (its references are
// tied to one machine's driver), and this can.
//
// THE FAILURE CASES ARE THE TEST. A validator that only ever says "ok" would pass every check here, so each
// malformed declaration below is asserted to be REJECTED: an unset resource, a binding kind that does not
// fit the resource, an access the kind cannot perform, an element outside the family, a descriptor count of
// zero, a sampled image with no sampler (and a uniform buffer with one), a duplicated binding, a gap in the
// own bindings, and a push block that does not fit.
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

import deren.utility.shared;
import deren.engine.render_resource;
import deren.engine.render_layout;             // the slot grid: its texture run IS the bindless array's capacity, checked below
import deren.engine.pass.upscale;              // make_easu_constants: the ported FsrEasuCon, checked as arithmetic below
import deren.engine.pass.output_targets;       // the software raster's separate surface: its two image declarations and the CSR tile-list buffer
import deren.promise.rhi;                      // the image/buffer request vocabulary the software surface's requests are checked against
import deren.engine.render_resource.instances; // the request/lifetime vocabulary the software surface's declarations use
import deren.engine.visibility.software;       // the CSR tile-list layout the buffer size is derived from

namespace {
    namespace rr = deren::engine::render_resource;
    namespace up = deren::engine::pass;

    /// validate a declaration that holds exactly one binding, so a wrong one can be built in one line
    std::expected<void, deren::utility::vessel::string> validate_one(rr::pass_binding const& binding) {
        rr::pass_io const io = {
            .name = "probe",
            .bindings = std::span<rr::pass_binding const>(&binding, 1),
            .push = std::nullopt,
        };
        return rr::validate(io);
    }

    /// a well-formed own binding, the starting point of every malformed one below
    constexpr rr::pass_binding good_binding = {
        .binding = 0,
        .owner = rr::binding_owner::own,
        .kind = rr::binding_kind::sampled_image,
        .resource = rr::resource_id::taa_history,
        .element = 0,
        .descriptor_count = 1,
        .access = rr::binding_access::read,
        .sampler = rr::sampler_hint::post,
        .stages = rr::stage_flag::compute,
    };
    /// THE SOFTWARE RASTER'S SURFACE (M4 Task 2, historical design record
    /// §3.2/§4.2): two per-swapchain-image R32_UINT images and one CSR tile-list buffer per frame lane. It is a
    /// SEPARATE surface rather than two more `ids` entries precisely so an OFF frame allocates nothing, and every field
    /// below is a fact a later edit could silently widen - hence the checks, in the same spirit as the schema tests.
    void software_resource_families() {
        namespace rhi = deren::promise::rhi;
        namespace layout = deren::engine::render_layout;
        namespace sw = deren::engine::visibility::software;
        using rr::resource_id;
        using otp = up::output_targets_pass;

        // ---- output families include the software pair, but default selection keeps them off ----
        CHECK(otp::resource_count == 17u);
        CHECK(otp::ids.size() == otp::resource_count);
        CHECK(otp::ids[15] == resource_id::software_depth);
        CHECK(otp::ids[16] == resource_id::software_id);

        // ---- the declaration: exactly two IMAGE families, in this order, with these heap slots ----
        CHECK(otp::software_resource_count == 2u);
        CHECK(otp::software_ids.size() == otp::software_resource_count);
        CHECK(otp::software_heap_slots.size() == otp::software_resource_count);
        CHECK(otp::software_storage_heap_slots.size() == otp::software_resource_count);
        CHECK(otp::software_ids[0] == resource_id::software_depth);
        CHECK(otp::software_ids[1] == resource_id::software_id);
        auto const families = otp::software_resource_families();
        CHECK(families.size() == otp::software_resource_count);
        auto const flags = rhi::to_bits(rhi::image_flag::sampled) | rhi::to_bits(rhi::image_flag::storage) |
                           rhi::to_bits(rhi::image_flag::transfer_source);
        for (std::size_t i = 0; i < families.size(); ++i) {
            CHECK(families[i].id == otp::software_ids[i]);
            CHECK(families[i].heap_slot == otp::software_heap_slots[i]);
            CHECK(families[i].storage_heap_slot == otp::software_storage_heap_slots[i]);
            CHECK(families[i].heap_slot == (i == 0 ? layout::heap_slots::software_depth : layout::heap_slots::software_id));
            CHECK(families[i].storage_heap_slot == (i == 0 ? layout::heap_slots::software_depth_storage : layout::heap_slots::software_id_storage));
            CHECK(families[i].format == rhi::image_format::r32_uint);
            CHECK(families[i].flags == flags);
            CHECK(families[i].scope == rr::resource_scope::per_swapchain_image);
            CHECK(families[i].life == rr::instances::lifetime::output_size);
            CHECK(families[i].debug_name != nullptr);
        }
        // THE GRID'S SPACING IS THE DECLARATION'S OWN: the depth family lives in the range visibility-buffer v0.5
        // RESERVED for it (1239..1246, one per-image run) and is therefore not adjacent to the id family, which follows
        // the HZB tables; only the id run and the tile-list lane are one capacity apart (the layout's static_asserts
        // state the same relations on the other side).
        CHECK(layout::heap_slots::software_depth == layout::heap_slot_base + 1239u); // the range v0.5 reserved for it
        CHECK(layout::heap_slots::software_depth + layout::heap_image_capacity <= layout::heap_slots::software_id);
        CHECK(layout::heap_slots::software_atomic == layout::heap_slots::software_id_storage + layout::heap_image_capacity);
        CHECK(layout::heap_slots::software_tiles - layout::heap_slots::software_id == layout::heap_image_capacity);
        CHECK(layout::heap_slots::software_depth_storage == layout::heap_slots::software_tiles + 2u);
        CHECK(layout::heap_slots::software_id_storage == layout::heap_slots::software_depth_storage + layout::heap_image_capacity);

        // ---- the schema: the families themselves, and the buffer ----
        auto const* const depth = rr::find(resource_id::software_depth);
        auto const* const id = rr::find(resource_id::software_id);
        auto const* const tiles = rr::find(resource_id::software_tiles);
        CHECK(depth != nullptr && id != nullptr && tiles != nullptr);
        if (depth != nullptr && id != nullptr && tiles != nullptr) {
            CHECK(depth->kind == rr::resource_kind::image2d && depth->scope == rr::resource_scope::per_swapchain_image);
            CHECK(depth->lifetime == rr::resource_lifetime::per_frame);
            CHECK(depth->name == "software_depth");
            CHECK(id->kind == rr::resource_kind::image2d && id->scope == rr::resource_scope::per_swapchain_image);
            CHECK(id->lifetime == rr::resource_lifetime::per_frame);
            CHECK(id->name == "software_id");
            CHECK(tiles->kind == rr::resource_kind::buffer);
            CHECK(tiles->scope == rr::resource_scope::device_wide); // ONE buffer with a lane per frame slot
            CHECK(tiles->lifetime == rr::resource_lifetime::per_frame);
            CHECK(tiles->name == "software_tiles");
        }

        // ---- the requests Task 4 will hand the table: one image per swapchain image, at the render extent ----
        rr::instances::resource_context const context{.frame_slots = 2u, .output_images = 3u, .render_extent = {.width = 1280u, .height = 720u, .depth = 1u}};
        auto const requests = otp::software_descriptions(context);
        CHECK(requests.size() == otp::software_resource_count);
        for (std::size_t i = 0; i < requests.size(); ++i) {
            CHECK(requests[i].id == otp::software_ids[i]);
            CHECK(requests[i].scope == rr::resource_scope::per_swapchain_image);
            CHECK(requests[i].life == rr::instances::lifetime::output_size);
            CHECK(requests[i].image.extent.width == context.render_extent.width);
            CHECK(requests[i].image.extent.height == context.render_extent.height);
            CHECK(requests[i].image.format == rhi::image_format::r32_uint);
            CHECK(requests[i].image.flags == flags);
        }
        otp provider;
        deren::utility::vessel::vector<rr::instances::request> selected;
        provider.describe_resources(context, selected);
        CHECK(selected.size() == 15u); // software targets stay unrequested by default
        provider.select_software_resources(true);
        selected.clear();
        provider.describe_resources(context, selected);
        CHECK(selected.size() == 18u);
        CHECK(selected[15].id == resource_id::software_depth);
        CHECK(selected[16].id == resource_id::software_id);
        CHECK(selected[17].id == resource_id::software_tiles);
        CHECK(selected[17].type == rr::instances::kind::buffer);

        // ---- THE CSR TILE-LIST BUFFER: one lane per frame slot, sized ONLY from the shared layout ----
        auto const tile_request = otp::software_tiles_description(context);
        CHECK(tile_request.id == resource_id::software_tiles);
        CHECK(tile_request.type == rr::instances::kind::buffer);
        CHECK(tile_request.scope == rr::resource_scope::device_wide);
        CHECK(tile_request.life == rr::instances::lifetime::frame_slot);
        CHECK(tile_request.buffer.usage == rhi::buffer_usage::storage_coherent);
        CHECK(tile_request.buffer.flags == (rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::transfer_source)));
        CHECK(tile_request.zero_on_create);
        // THE LAYOUT ITSELF, pinned at 720p: header 6 words + counts 3600 + offsets 3601 + active tiles 3600 =
        // 10,807 words (43,228 B), then the GLOBAL budget of 65,536 8-byte entries (524,288 B) = 567,516 B per lane;
        // two frame lanes = 1,135,032 B.
        // (3600 tiles = 80 x 45 for 1280x720 at 16x16; the budget is the module's one adjustable constant.)
        CHECK(sw::tile_size == 16u);
        CHECK(sw::software_entry_budget == 65'536u);
        CHECK(sw::software_tiles_header_words == 6u);
        CHECK(sw::software_tiles_entry_bytes == 8u);
        CHECK(sw::software_tiles_bytes(1280u, 720u) == (6u + 3600u + 3601u + 3600u) * 4u + 65'536u * 8u);
        CHECK(sw::software_tiles_bytes(1280u, 720u) == 567'516u);
        CHECK(rr::software_tiles_buffer_bytes(1280u, 720u) == sw::software_tiles_bytes(1280u, 720u));
        CHECK(tile_request.buffer.size == context.frame_slots * rr::software_tiles_buffer_bytes(1280u, 720u));
        CHECK(tile_request.buffer.size == 2u * 567'516u);
    }
} // namespace

int32_t main() {
    // ---- the schema describes itself, completely ----
    auto const schema = rr::validate_schema();
    CHECK_MSG(schema.has_value(), schema.has_value() ? "" : schema.error().c_str());
    CHECK(rr::resource_schema.size() == 80);                    // 76 + M4 Task 2's three software-raster families
    CHECK(static_cast<uint32_t>(rr::resource_id::count) == 81); // 80 families plus `none`
    CHECK(rr::find(rr::resource_id::material_table)->scope == rr::resource_scope::device_wide);
    CHECK(rr::find(rr::resource_id::instance_table)->scope == rr::resource_scope::device_wide);
    CHECK(rr::find(rr::resource_id::skin_matrices_previous)->scope == rr::resource_scope::per_frame_slot);
    CHECK(rr::find(rr::resource_id::none) == nullptr);
    CHECK(rr::find(rr::resource_id::head_ubo) != nullptr && rr::find(rr::resource_id::head_ubo)->scope == rr::resource_scope::per_frame_slot);
    CHECK(rr::find(rr::resource_id::furnace_cube) != nullptr);
    CHECK(rr::find(rr::resource_id::shadow_map)->count == 4);      // one layer per cascade, as core indexes it
    CHECK(rr::find(rr::resource_id::gbuffer_targets)->count == 3); // albedo, normal+roughness, material+AO
    // the bloom chain's FOUR levels are elements of one family (core::bloom_images is an array of four vectors),
    // and the count is what makes `element = 3` legal and `element = 4` refused - the contract the post chain's
    // declarations are written against, asserted where it lives rather than assumed by them
    CHECK(rr::find(rr::resource_id::bloom)->count == 4);
    {
        deren::utility::vessel::array<rr::render_target, 1> const last_level = {rr::render_target{.resource = rr::resource_id::bloom, .element = 3, .kind = rr::target_kind::color}};
        rr::pass_io const io = {.name = "bloom", .bindings = {}, .targets = last_level, .push = std::nullopt};
        CHECK(rr::validate(io).has_value()); // the deepest level a pass may render into
        deren::utility::vessel::array<rr::render_target, 1> const past_the_end = {rr::render_target{.resource = rr::resource_id::bloom, .element = 4, .kind = rr::target_kind::color}};
        rr::pass_io const bad = {.name = "bloom", .bindings = {}, .targets = past_the_end, .push = std::nullopt};
        CHECK(!rr::validate(bad).has_value()); // ... and the one past it, which names no image at all
    }
    CHECK(rr::find(rr::resource_id::top_level_structure)->kind == rr::resource_kind::accel_struct);
    CHECK(rr::find(rr::resource_id::top_level_structure)->scope == rr::resource_scope::per_frame_slot);
    CHECK(rr::find(rr::resource_id::swapchain_image)->lifetime == rr::resource_lifetime::imported);
    // the two scopes this project has been bitten by, asserted where they live
    CHECK(rr::find(rr::resource_id::shadow_map)->scope == rr::resource_scope::per_frame_slot);
    CHECK(rr::find(rr::resource_id::taa_history)->scope == rr::resource_scope::per_swapchain_image);
    CHECK(rr::find(rr::resource_id::furnace_cube)->scope == rr::resource_scope::device_wide);

    // The binding owner decides which rules apply: the pass's OWN bindings must be numbered contiguously from
    // zero, while a `shared` binding only USES a resource the frame's heap provides and carries no such
    // requirement - which is the distinction the resolver and this declaration layer key on.
    {
        deren::utility::vessel::array<rr::pass_binding, 2> const mixed = {
            good_binding, // the pass's own binding 0
            // One of the SCENE's resources, reached through the frame's heap rather than owned - deliberately
            // numbered 4, which would be a contiguity error if it were an `own` binding
            {.binding = 4,
             .owner = rr::binding_owner::shared,
             .kind = rr::binding_kind::sampled_image,
             .resource = rr::resource_id::scene_textures,
             .access = rr::binding_access::read,
             .sampler = rr::sampler_hint::post},
        };
        rr::pass_io const io = {.name = "shared-like",
                                .bindings = mixed,
                                .targets = {},
                                .push = rr::push_block{.offset = 0, .size = 56, .stages = rr::stage_flag::compute}};
        CHECK(rr::validate(io).has_value());
        // ... and the SAME pair with the shared binding marked `own` is refused, because 0,4 is not contiguous
        deren::utility::vessel::array<rr::pass_binding, 2> const not_contiguous = {good_binding, {.binding = 4, .owner = rr::binding_owner::own, .kind = rr::binding_kind::sampled_image, .resource = rr::resource_id::scene_textures, .access = rr::binding_access::read, .sampler = rr::sampler_hint::post}};
        rr::pass_io const gap = {.name = "shared-like", .bindings = not_contiguous, .targets = {}, .push = std::nullopt};
        CHECK(!rr::validate(gap).has_value());
    }

    // ---- the well-formed binding, so the failures below mean something ----
    CHECK(validate_one(good_binding).has_value());
    CHECK(rr::compatible(rr::binding_kind::storage_buffer, rr::resource_kind::buffer));
    CHECK(!rr::compatible(rr::binding_kind::storage_image, rr::resource_kind::buffer));
    CHECK(rr::has_stage(rr::stage_flag::compute | rr::stage_flag::fragment, rr::stage_flag::fragment));
    CHECK(!rr::has_stage(rr::stage_flag::compute, rr::stage_flag::fragment));
    CHECK(rr::name_of(rr::binding_kind::acceleration_structure) == "acceleration_structure");

    // ---- every malformed declaration is REJECTED (a validator that cannot fail is not one) ----
    {
        rr::pass_binding b = good_binding;
        b.resource = rr::resource_id::none;
        CHECK(!validate_one(b).has_value()); // an unset resource
    }
    {
        rr::pass_binding b = good_binding;
        b.kind = rr::binding_kind::sampled_image;
        b.resource = rr::resource_id::light_ubo; // a buffer bound as an image
        CHECK(!validate_one(b).has_value());
    }
    {
        rr::pass_binding b = good_binding;
        b.kind = rr::binding_kind::uniform_buffer;
        b.resource = rr::resource_id::light_ubo;
        b.access = rr::binding_access::write; // a uniform buffer cannot be written
        b.sampler = rr::sampler_hint::none;
        CHECK(!validate_one(b).has_value());
    }
    {
        rr::pass_binding b = good_binding;
        b.resource = rr::resource_id::shadow_map;
        b.element = 4; // the family holds 0..3
        CHECK(!validate_one(b).has_value());
    }
    {
        rr::pass_binding b = good_binding;
        b.sampler = rr::sampler_hint::none; // a sampled image must name one
        CHECK(!validate_one(b).has_value());
    }
    {
        rr::pass_binding b = good_binding;
        b.kind = rr::binding_kind::storage_image;
        b.resource = rr::resource_id::taa_history;
        b.access = rr::binding_access::write;
        CHECK(!validate_one(b).has_value()); // ... and a storage image must not
    }
    {
        // A storage image with write access is VALID and has nothing left to declare: with unified layouts
        // every image is in GENERAL, so the kind plus the access says everything (this case used to assert
        // the opposite - that such a declaration FAILED because it did not name GENERAL).
        rr::pass_binding b = good_binding;
        b.kind = rr::binding_kind::storage_image;
        b.resource = rr::resource_id::taa_history;
        b.access = rr::binding_access::write;
        b.sampler = rr::sampler_hint::none;
        CHECK(validate_one(b).has_value());
    }
    {
        rr::pass_binding b = good_binding;
        b.descriptor_count = 0;
        CHECK(!validate_one(b).has_value());
    }
    {
        // The case that stood here ("an own binding outside the pass's own set") went with the set index itself:
        // there is no set to be outside any more, and the surviving rule - own bindings numbered contiguously
        // from zero - is the next case.
        rr::pass_binding b = good_binding;
        b.binding = 3; // ... numbered contiguously from zero
        CHECK(!validate_one(b).has_value());
    }
    {
        deren::utility::vessel::array<rr::pass_binding, 2> const dup = {good_binding, good_binding};
        rr::pass_io const io = {.name = "probe", .bindings = dup, .push = std::nullopt};
        CHECK(!rr::validate(io).has_value()); // the same binding twice
    }
    {
        rr::pass_io const io = {.name = {}, .bindings = {}, .push = std::nullopt};
        CHECK(!rr::validate(io).has_value()); // an unnamed pass
    }
    {
        rr::pass_io const io = {.name = "probe", .bindings = {}, .push = rr::push_block{.offset = 0, .size = 132, .stages = rr::stage_flag::compute}};
        CHECK(!rr::validate(io).has_value()); // past the 128-byte guaranteed minimum
    }
    {
        rr::pass_io const io = {.name = "probe", .bindings = {}, .push = rr::push_block{.offset = 0, .size = 6, .stages = rr::stage_flag::compute}};
        CHECK(!rr::validate(io).has_value()); // not a whole number of 4-byte lanes
    }

    // ---- the render TARGETS: an attachment is a use that cannot be a descriptor, so it is declared here ----
    rr::render_target const hdr_target = {.resource = rr::resource_id::hdr, .element = 0};
    rr::render_target const no_target = {.resource = rr::resource_id::none, .element = 0};
    rr::render_target const past_the_family = {.resource = rr::resource_id::hdr, .element = 4};
    rr::render_target const not_an_image = {.resource = rr::resource_id::light_ubo, .element = 0};
    {
        deren::utility::vessel::array<rr::render_target, 1> const target = {hdr_target};
        rr::pass_io const io = {.name = "taa", .bindings = {}, .targets = target, .push = std::nullopt};
        CHECK(rr::validate(io).has_value()); // the frame's HDR target, written by a fullscreen resolve
    }
    {
        deren::utility::vessel::array<rr::render_target, 1> const target = {no_target};
        rr::pass_io const io = {.name = "taa", .bindings = {}, .targets = target, .push = std::nullopt};
        CHECK(!rr::validate(io).has_value()); // an unset resource, exactly as for a binding
    }
    {
        deren::utility::vessel::array<rr::render_target, 1> const target = {past_the_family};
        rr::pass_io const io = {.name = "taa", .bindings = {}, .targets = target, .push = std::nullopt};
        CHECK(!rr::validate(io).has_value()); // element 4 of a per-swapchain-image family that holds one
    }
    {
        deren::utility::vessel::array<rr::render_target, 1> const target = {not_an_image};
        rr::pass_io const io = {.name = "taa", .bindings = {}, .targets = target, .push = std::nullopt};
        CHECK(!rr::validate(io).has_value()); // a buffer is not an image a pass can render into
    }
    {
        deren::utility::vessel::array<rr::render_target, 2> const target = {hdr_target, hdr_target};
        rr::pass_io const io = {.name = "taa", .bindings = {}, .targets = target, .push = std::nullopt};
        CHECK(!rr::validate(io).has_value()); // the same image twice
    }
    {
        // a DEPTH target: the scene pass declares one, and an instance has exactly one
        rr::render_target const depth_target = {.resource = rr::resource_id::gbuffer_depth, .element = 0, .kind = rr::target_kind::depth};
        deren::utility::vessel::array<rr::render_target, 2> const one_depth = {hdr_target, depth_target};
        rr::pass_io const io = {.name = "scene", .bindings = {}, .targets = one_depth, .push = std::nullopt};
        CHECK(rr::validate(io).has_value()); // one colour plus one depth is what a scene instance is
    }
    {
        rr::render_target const depth_a = {.resource = rr::resource_id::gbuffer_depth, .element = 0, .kind = rr::target_kind::depth};
        rr::render_target const depth_b = {.resource = rr::resource_id::shadow_map, .element = 0, .kind = rr::target_kind::depth};
        deren::utility::vessel::array<rr::render_target, 2> const two_depths = {depth_a, depth_b};
        rr::pass_io const io = {.name = "scene", .bindings = {}, .targets = two_depths, .push = std::nullopt};
        CHECK(!rr::validate(io).has_value()); // two depth attachments cannot be recorded
    }

    // ---- the SECOND declaration: the TAA resolve, whose bindings are FRAGMENT and all its own with nothing
    //      shared beside them, and which is the first one to declare a render TARGET ----
    {
        CHECK(rr::validate(rr::taa_io).has_value());
        CHECK(rr::taa_io.bindings.size() == 4);
        for (rr::pass_binding const& b : rr::taa_io.bindings) {
            CHECK(b.owner == rr::binding_owner::own);
            CHECK(b.kind == rr::binding_kind::sampled_image);
            CHECK(b.sampler == rr::sampler_hint::taa);
            // THE STAGE FLAGS COME FROM THE DECLARATION, so a fragment binding declared compute would be one the
            // fragment stage cannot see: every declaration before this one was a compute pass and took the
            // default.
            CHECK(rr::has_stage(b.stages, rr::stage_flag::fragment));
            CHECK(!rr::has_stage(b.stages, rr::stage_flag::compute));
        }
        CHECK(rr::taa_io.targets.size() == 1);
        CHECK(rr::taa_io.targets[0].resource == rr::resource_id::hdr);
        CHECK(rr::taa_io.push->size == 32); // eight floats: the history flag, two weights, texel size, two depth terms
        CHECK(rr::taa_io.push->stages == rr::stage_flag::fragment);
        // Four bindings, one descriptor each - the count the deleted pool-sizing helper used to return, now read
        // straight off the declaration the resolver reads.
        uint32_t descriptors = 0;
        for (rr::pass_binding const& b : rr::taa_io.bindings) {
            descriptors += b.descriptor_count;
        }
        CHECK(descriptors == 4);
        // nothing is shared: the resolve's four inputs are all its own
        CHECK(std::none_of(rr::taa_io.bindings.begin(), rr::taa_io.bindings.end(), [](rr::pass_binding const& b) { return b.owner != rr::binding_owner::own; }));
    }

    // ---- the THIRD declaration: the scene pass, the first one with NO own bindings (everything it reads is a
    //      shared resource and it writes the frame's surface) and the first with six targets including DEPTH ----
    {
        CHECK(rr::validate(rr::scene_io).has_value());
        CHECK(rr::scene_io.bindings.empty()); // it owns no binding: everything arrives through the frame's heap
        CHECK(rr::scene_io.targets.size() == 6);
        CHECK(rr::scene_io.targets[0].resource == rr::resource_id::gbuffer_targets);
        CHECK(rr::scene_io.targets[2].element == 2); // all three stored surface targets
        CHECK(rr::scene_io.targets[3].resource == rr::resource_id::velocity);
        CHECK(rr::scene_io.targets[4].resource == rr::resource_id::scene_color); // the frame decision (see the doc)
        CHECK(rr::scene_io.targets[5].resource == rr::resource_id::gbuffer_depth);
        CHECK(rr::scene_io.targets[5].kind == rr::target_kind::depth); // exactly one depth, as an instance wants
        std::size_t depth_targets = 0;
        for (rr::render_target const& t : rr::scene_io.targets) {
            if (t.kind == rr::target_kind::depth) {
                ++depth_targets;
            }
        }
        CHECK(depth_targets == 1);
        CHECK(!rr::scene_io.push.has_value()); // the per-leaf pushes belong to the leaves
        // The per-set descriptor-count assertion that stood here went with the pool-sizing helper; the fact it
        // was about is above: this pass declares no binding at all.
    }
    {
        // The two cases that stood here - a set declared as both its own and a shared one, and the same shared
        // set declared twice - went with the set vocabulary. Their surviving subject is covered: `binding_owner`
        // exemption from contiguity above, and duplicate binding numbers just below.
        deren::utility::vessel::array<rr::pass_binding, 2> const twice = {rr::pass_binding{.binding = 2, .owner = rr::binding_owner::shared, .kind = rr::binding_kind::storage_buffer, .resource = rr::resource_id::cluster_counts, .access = rr::binding_access::write},
                                                                          rr::pass_binding{.binding = 2, .owner = rr::binding_owner::shared, .kind = rr::binding_kind::storage_buffer, .resource = rr::resource_id::cluster_indices, .access = rr::binding_access::write}};
        rr::pass_io const io = {.name = "scene", .bindings = twice, .targets = {}, .push = std::nullopt};
        CHECK(!rr::validate(io).has_value()); // two bindings may not share a number, whatever their owner
    }

    // The GI tracer's declaration was asserted here (two shared sets, no own binding, twelve barrier images).
    // The pass is gone, so what remains is the validator rule it exercised.
    {
        // the barrier-image rule the validator enforces: they must be images the schema declares, and a pass
        // indexes them by POSITION, so the same one twice is a declaration that cannot be read
        deren::utility::vessel::array<rr::barrier_image, 1> const barrier_not_an_image = {rr::barrier_image{.resource = rr::resource_id::camera_ubo, .element = 0}};
        rr::pass_io const io = {.name = "barrier-like", .bindings = {}, .targets = {}, .barrier_images = barrier_not_an_image, .push = std::nullopt};
        CHECK(!rr::validate(io).has_value());
        deren::utility::vessel::array<rr::barrier_image, 2> const twice = {rr::barrier_image{.resource = rr::resource_id::hdr, .element = 0},
                                                                           rr::barrier_image{.resource = rr::resource_id::hdr, .element = 0}};
        rr::pass_io const dup = {.name = "barrier-like", .bindings = {}, .targets = {}, .barrier_images = twice, .push = std::nullopt};
        CHECK(!rr::validate(dup).has_value());
    }

    // ---- the SIXTH declaration: the stochastic punctual lighting chain's temporal resolve ----
    // (the GI denoiser's declaration stood here; it went with that chain) - the stochastic punctual
    // lighting chain's resolve - is a declaration of its own (megalights_temporal_io) with the same shape, and
    // it is covered by the lighting chain's own tests rather than by a GI scenario.

    // The spatial filter's declaration was asserted here - the third pass on the shared-sets shape. It went with
    // the chain; the shape itself is still covered by the ray-traced shadow's declaration below.

    // ---- the EIGHTH declaration, and the first one OUTSIDE the chain: the ray-traced shadow, the same
    //      shared-resource shape as the tracer and the spatial filter but at the FRAME's resolution, and its one
    //      barrier image is a per-frame-slot resource rather than a per-swapchain-image family ----
    {
        CHECK(rr::validate(rr::rt_shadow_io).has_value());
        CHECK(rr::rt_shadow_io.bindings.empty()); // the camera, the light UBO and the TLAS are shared heap resources
        CHECK(rr::rt_shadow_io.targets.empty());  // a compute pass
        // The two shared sets this declaration used to name are gone with that vocabulary; what survives is that
        // the declaration binds nothing itself, and that its one barrier image is per-frame-slot.
        CHECK(rr::rt_shadow_io.barrier_images.size() == 1);
        CHECK(rr::rt_shadow_io.barrier_images[0].resource == rr::resource_id::rt_shadow_visibility);
        CHECK(rr::find(rr::resource_id::rt_shadow_visibility)->scope == rr::resource_scope::per_frame_slot);
        CHECK(rr::rt_shadow_io.push.has_value());
        CHECK(rr::rt_shadow_io.push->size == 80); // inv_view_proj (64) + the four ray-offset terms (16)
        CHECK(rr::rt_shadow_io.push->stages == rr::stage_flag::compute);
    }

    // ---- the NINTH declaration: the clustered-light sort, the first pass whose resources are BUFFERS it
    //      orders without binding (they are part of the frame's shared scene resources, bindings 11 and 12) -
    //      which is what the barrier_buffers channel was added for ----
    {
        CHECK(rr::validate(rr::cluster_io).has_value());
        CHECK(rr::cluster_io.bindings.empty()); // it reaches the shared scene resources through the heap
        CHECK(rr::cluster_io.targets.empty());  // a compute pass
        CHECK(rr::cluster_io.push.has_value()); // heap frame/image indices used by the compute shader
        if (rr::cluster_io.push)
            CHECK(rr::cluster_io.push->offset == 0 && rr::cluster_io.push->size == 8);
        CHECK(rr::cluster_io.barrier_images.empty()); // it moves no image
        CHECK(rr::cluster_io.barrier_buffers.size() == 2);
        CHECK(rr::cluster_io.barrier_buffers[0].resource == rr::resource_id::cluster_counts);
        CHECK(rr::cluster_io.barrier_buffers[1].resource == rr::resource_id::cluster_indices);
        CHECK(rr::find(rr::resource_id::cluster_counts)->kind == rr::resource_kind::buffer);
        CHECK(rr::find(rr::resource_id::cluster_indices)->kind == rr::resource_kind::buffer);
        CHECK(rr::find(rr::resource_id::cluster_counts)->scope == rr::resource_scope::per_frame_slot);
    }
    {
        // the barrier-BUFFER rule, the same three checks one resource class over: it must be a buffer the
        // schema declares, and a pass indexes these by position so the same one twice cannot be read
        deren::utility::vessel::array<rr::barrier_buffer, 1> const not_a_buffer = {rr::barrier_buffer{.resource = rr::resource_id::hdr, .element = 0}};
        rr::pass_io const io = {.name = "cluster", .bindings = {}, .targets = {}, .barrier_buffers = not_a_buffer, .push = std::nullopt};
        CHECK(!rr::validate(io).has_value());
        deren::utility::vessel::array<rr::barrier_buffer, 2> const twice = {rr::barrier_buffer{.resource = rr::resource_id::cluster_counts, .element = 0},
                                                                            rr::barrier_buffer{.resource = rr::resource_id::cluster_counts, .element = 0}};
        rr::pass_io const dup = {.name = "cluster", .bindings = {}, .targets = {}, .barrier_buffers = twice, .push = std::nullopt};
        CHECK(!rr::validate(dup).has_value());
        // ... and an element the family does not hold is rejected too (the schema's count is the contract)
        deren::utility::vessel::array<rr::barrier_buffer, 1> const out_of_range = {rr::barrier_buffer{.resource = rr::resource_id::cluster_counts, .element = 9}};
        rr::pass_io const bad_element = {.name = "cluster", .bindings = {}, .targets = {}, .barrier_buffers = out_of_range, .push = std::nullopt};
        CHECK(!rr::validate(bad_element).has_value());
    }

    // ---- the TENTH declaration: the deferred lighting stage, a fullscreen pass over the same shared resources the
    //      traced compute passes reach, whose only resource of its own is the target it renders into - and whose target is a
    //      recorded deviation (it names scene_color; the host hands over hdr on the frames TAA is off) ----
    {
        CHECK(rr::validate(rr::deferred_io).has_value());
        CHECK(rr::deferred_io.bindings.empty());    // every binding it uses is one of the frame's shared resources
        CHECK(rr::deferred_io.targets.size() == 1); // the only resource of its own: what it renders into
        CHECK(rr::deferred_io.targets[0].resource == rr::resource_id::scene_color);
        CHECK(rr::deferred_io.targets[0].kind == rr::target_kind::color);
        CHECK(rr::deferred_io.barrier_images.empty()); // it moves no image of its own
        CHECK(rr::deferred_io.push.has_value());
        CHECK(rr::deferred_io.push->size == 88);                         // mat4 + vec4 + two floats
        CHECK(rr::deferred_io.push->stages == rr::stage_flag::fragment); // the vertex stage pushes nothing
    }

    // ---- the post chain's FIVE declarations: the bloom chain's four levels and the composite, each with the level
    //      IS the pass boundary and the chain's edges declared where they are not the frame loop's ----
    {
        CHECK(rr::post_bloom_io.size() == 4);
        for (std::size_t level = 0; level < rr::post_bloom_io.size(); ++level) {
            rr::pass_io const& io = rr::post_bloom_io[level];
            auto const valid = rr::validate(io);
            CHECK_MSG(valid.has_value(), valid.has_value() ? "" : valid.error().c_str());
            CHECK(io.bindings.empty()); // everything it reads is a shared post-chain resource
            CHECK(io.targets.size() == 1);
            CHECK(io.targets[0].resource == rr::resource_id::bloom); // the level it writes...
            CHECK(io.targets[0].element == level);                   // ... which IS the pass boundary
            CHECK(io.targets[0].kind == rr::target_kind::color);
            // the level it READS is declared exactly when it has to move it: level 0's input is the HDR target,
            // whose transition is the frame loop's (the composite reads it too, and on a bloom-off frame nobody
            // else does), and levels 1..3 own the transition of the level before them
            CHECK((level == 0 ? io.barrier_images.empty() : io.barrier_images.size() == 1));
            if (level > 0) {
                CHECK(io.barrier_images[0].resource == rr::resource_id::bloom);
                CHECK(io.barrier_images[0].element == level - 1);
            }
            CHECK(io.push.has_value());
            CHECK(io.push->size == rr::post_push_bytes); // the chain's ONE block, all 52 bytes of it
            CHECK(io.push->stages == rr::stage_flag::fragment);
        }

        auto const composite = rr::validate(rr::post_composite_io);
        CHECK_MSG(composite.has_value(), composite.has_value() ? "" : composite.error().c_str());
        CHECK(rr::post_composite_io.targets.size() == 1);
        // the RECORDED DEVIATION: the declaration names the swapchain, and the host hands over the LDR image (and
        // the R16F pipeline that goes with it) on the frames FXAA runs
        CHECK(rr::post_composite_io.targets[0].resource == rr::resource_id::swapchain_image);
        CHECK(rr::post_composite_io.barrier_images.empty()); // the HDR transition is the frame loop's, on every frame
        CHECK(rr::post_composite_io.push.has_value());
        CHECK(rr::post_composite_io.push->size == rr::post_push_bytes);
    }

    // ---- the FOURTEENTH declaration: the FXAA pass, the frame's LAST writer whenever it runs ----
    {
        auto const fxaa = rr::validate(rr::fxaa_io);
        CHECK_MSG(fxaa.has_value(), fxaa.has_value() ? "" : fxaa.error().c_str());
        CHECK(rr::fxaa_io.bindings.empty()); // everything it reads is a shared post-chain resource
        CHECK(rr::fxaa_io.targets.size() == 1);
        CHECK(rr::fxaa_io.targets[0].resource == rr::resource_id::swapchain_image); // it finishes the frame
        CHECK(rr::fxaa_io.targets[0].kind == rr::target_kind::color);
        // ITS INPUT IS DECLARED, unlike the composite's, and the difference is the frame: the LDR image is written
        // by the composite and read here, and on a frame without FXAA nobody touches it at all - so this pass is
        // the one that moves it, and there is no "nobody ran" case to hand over to the frame loop
        CHECK(rr::fxaa_io.barrier_images.size() == 1);
        CHECK(rr::fxaa_io.barrier_images[0].resource == rr::resource_id::ldr);
        CHECK(rr::fxaa_io.push.has_value());
        CHECK(rr::fxaa_io.push->size == rr::post_push_bytes); // FXAA is mode 3 of the chain's one shader block
        CHECK(rr::fxaa_io.push->stages == rr::stage_flag::fragment);
    }

    // ---- the FIFTEENTH declaration: the G-buffer debug view - the stored surface, one channel at a time ----
    {
        auto const debug = rr::validate(rr::gbuffer_debug_io);
        CHECK_MSG(debug.has_value(), debug.has_value() ? "" : debug.error().c_str());
        CHECK(rr::gbuffer_debug_io.bindings.empty()); // everything it reads is a shared G-buffer resource
        // THE HDR TARGET, declared as itself: this is the one graphics declaration in the chain with no deviation -
        // the image it writes is the image it names
        CHECK(rr::gbuffer_debug_io.targets.size() == 1);
        CHECK(rr::gbuffer_debug_io.targets[0].resource == rr::resource_id::hdr);
        CHECK(rr::gbuffer_debug_io.targets[0].kind == rr::target_kind::color);
        // the four images it moves to a sampled layout: the three stored targets and the motion vectors. The DEPTH
        // is deliberately NOT here - its old layout depends on whether the G-buffer instance rendered this frame,
        // which is the host's per-image bookkeeping (the accessor the two other sampling stages share)
        CHECK(rr::gbuffer_debug_io.barrier_images.size() == 4);
        CHECK(rr::gbuffer_debug_io.barrier_images[0].resource == rr::resource_id::gbuffer_targets);
        CHECK(rr::gbuffer_debug_io.barrier_images[0].element == 0);
        CHECK(rr::gbuffer_debug_io.barrier_images[1].element == 1);
        CHECK(rr::gbuffer_debug_io.barrier_images[2].element == 2);
        CHECK(rr::gbuffer_debug_io.barrier_images[3].resource == rr::resource_id::velocity);
        CHECK(rr::gbuffer_debug_io.push.has_value());
        CHECK(rr::gbuffer_debug_io.push->size == 16); // the channel, two projection terms, the motion gain
        CHECK(rr::gbuffer_debug_io.push->stages == rr::stage_flag::fragment);
    }

    // ---- the SIXTEENTH declaration: the shadow pass, one cascade at a time into a layer of the map array ----
    {
        auto const shadow = rr::validate(rr::shadow_io);
        auto const shadow_error = shadow.has_value() ? deren::utility::vessel::string{} : shadow.error(); // a temporary`s c_str() would dangle
        CHECK_MSG(shadow.has_value(), shadow_error.c_str());
        CHECK(rr::shadow_io.bindings.empty()); // the shared scene resources carry everything the depth-only draw reads
        // ONE TARGET ENTRY, N ELEMENTS - the RUN of cascade layers, and this is what replaced "one target by
        // declaration, N by frame": the validator refuses a second DEPTH target (a rendering instance has exactly
        // one depth attachment), so the host used to hand the extra layers over itself. `render_target::count` is
        // the vocabulary that says what that was trying to say - ONE family, rendered one INSTANCE per element -
        // and the frame still caps it, because the map has exactly the layers the cascade knob asked for.
        CHECK(rr::shadow_io.targets.size() == 1);
        CHECK(rr::shadow_io.targets[0].resource == rr::resource_id::shadow_map);
        CHECK(rr::shadow_io.targets[0].element == 0); // the first cascade; the run is consecutive from there
        CHECK(rr::shadow_io.targets[0].kind == rr::target_kind::depth);
        CHECK(rr::shadow_io.targets[0].count == 4); // every layer max_shadow_cascades allows
        // ... and the map family holds exactly that many layers: FOUR, like the image create_shadow_resources
        // creates (the count was 1 until this declaration, the same correction the bloom family needed)
        CHECK(rr::find(rr::resource_id::shadow_map)->count == 4);
        CHECK(rr::shadow_io.barrier_images.empty()); // each layer is moved through its own target
        CHECK(rr::shadow_io.push.has_value());
        CHECK(rr::shadow_io.push->size == 4); // the cascade index
        // AT 108, NOT 96: the scene's own push block ends at 96 (scene_push_constant_size), and the host appends
        // THE HEAP INDEX LANES - three of them - at exactly that offset for every stage. The cascade used to be
        // declared at 96, which put it UNDER the first lane: the shader read frame_slot as its cascade, the pass
        // rendered every caster into the wrong cascade layer, and the lanes themselves were shifted. It rides
        // after the third lane now, which is why this offset moved with it (see source/shaders/lighting/shadow.slang's block).
        CHECK(rr::shadow_io.push->offset == 108); // scene_push_constant_size + three 4-byte heap index lanes
        CHECK(rr::shadow_io.push->stages == (rr::stage_flag::vertex | rr::stage_flag::fragment));

        // THE RUN'S OWN CHECKS: a run that reaches past its family is a declaration error ...
        deren::utility::vessel::array<rr::render_target, 1> const run_past_the_family = {rr::render_target{.resource = rr::resource_id::shadow_map, .element = 2, .kind = rr::target_kind::depth, .count = 4}};
        rr::pass_io const bad_run = {.name = "shadow", .bindings = {}, .targets = run_past_the_family, .push = std::nullopt};
        CHECK(!rr::validate(bad_run).has_value());
        // ... and so is claiming NO element, which would be a target that renders nothing ...
        deren::utility::vessel::array<rr::render_target, 1> const empty_run = {rr::render_target{.resource = rr::resource_id::shadow_map, .element = 0, .kind = rr::target_kind::depth, .count = 0}};
        rr::pass_io const no_run = {.name = "shadow", .bindings = {}, .targets = empty_run, .push = std::nullopt};
        CHECK(!rr::validate(no_run).has_value());
        // ... and two runs of ONE resource may not overlap, which is "rendering into one image twice" one layer
        // out. (Two depth runs would be refused by the one-depth rule first, so this case is a colour family's.)
        deren::utility::vessel::array<rr::render_target, 2> const overlapping = {rr::render_target{.resource = rr::resource_id::bloom, .element = 0, .count = 3},
                                                                                 rr::render_target{.resource = rr::resource_id::bloom, .element = 2, .count = 2}};
        rr::pass_io const overlap = {.name = "post_hdr", .bindings = {}, .targets = overlapping, .push = std::nullopt};
        CHECK(!rr::validate(overlap).has_value());
        // ... while two runs that TOUCH but do not overlap are legal, which keeps the rule about the IMAGES a
        // declaration claims rather than about adjacency
        deren::utility::vessel::array<rr::render_target, 2> const adjacent = {rr::render_target{.resource = rr::resource_id::bloom, .element = 0, .count = 2},
                                                                              rr::render_target{.resource = rr::resource_id::bloom, .element = 2, .count = 2}};
        rr::pass_io const touching = {.name = "post_hdr", .bindings = {}, .targets = adjacent, .push = std::nullopt};
        CHECK(rr::validate(touching).has_value());
    }
    // ---- the SEVENTEENTH declaration: the UPSCALE resolve, the frame's last writer below render_scale 1.0 ----
    {
        auto const upscale = rr::validate(rr::upscale_io);
        CHECK_MSG(upscale.has_value(), upscale.has_value() ? "" : upscale.error().c_str());
        CHECK(rr::upscale_io.bindings.empty()); // the LDR image reaches the shader through the frame's heap
        CHECK(rr::upscale_io.targets.size() == 1);
        CHECK(rr::upscale_io.targets[0].resource == rr::resource_id::swapchain_image); // it finishes the frame
        CHECK(rr::upscale_io.targets[0].element == 0);
        CHECK(rr::upscale_io.targets[0].kind == rr::target_kind::color);
        // ITS INPUT IS DECLARED, exactly as FXAA's is and for the same reason: the composite writes the LDR
        // image and this pass reads it, so this pass is the one that moves it - and on a frame it does not run
        // (render_scale 1.0, or FXAA resolving instead) nothing moves it at all
        CHECK(rr::upscale_io.barrier_images.size() == 1);
        CHECK(rr::upscale_io.barrier_images[0].resource == rr::resource_id::ldr);
        CHECK(rr::upscale_io.barrier_images[0].element == 0);
        CHECK(rr::upscale_io.push.has_value());
        // THE PASS'S OWN BLOCK, NOT THE POST CHAIN'S: EASU's four constants plus the two scalar lanes (which
        // filter, and the display transfer). The shader's block is this size PLUS the heap index lanes the
        // framework appends (the LDR image is a per-swapchain-image heap slot the shader names itself), which is
        // why the declaration stops at the bytes the pass composes - see upscale_push_bytes.
        CHECK(rr::upscale_io.push->size == 72);
        CHECK(rr::upscale_io.push->size == rr::upscale_push_bytes);
        CHECK(rr::upscale_io.push->offset == 0);
        CHECK(rr::upscale_io.push->stages == rr::stage_flag::fragment);
    }

    // ---- EASU's constants: `vulkan.pass.upscale::make_easu_constants` IS `FsrEasuCon` (AMD's ffx_fsr1.h,
    //      v1.20210629) with the extents split into the two this renderer has. Pinned as arithmetic rather than
    //      only observed through a rendered frame, because the term that is easiest to get wrong - the -0.5
    //      half-pixel centering - produces a plausible image shifted by half an input pixel, and the ratio that
    //      would hide a tap-offset mistake is exactly the power-of-two one.
    {
        auto const near = [](float const a, float const b) { return std::abs(a - b) < 1e-6f; };

        // HALF SCALE, an integer ratio: 540x480 of a 1080x960 output.
        auto const half = up::make_easu_constants(540, 480, 1080, 960);
        CHECK(near(half.con0[0], 0.5f) && near(half.con0[1], 0.5f));     // input -> output scale
        CHECK(near(half.con0[2], -0.25f) && near(half.con0[3], -0.25f)); // the half-pixel centering
        CHECK(near(half.con1[0], 1.0f / 540.0f) && near(half.con1[1], 1.0f / 480.0f));
        CHECK(near(half.con1[2], 1.0f / 540.0f) && near(half.con1[3], -1.0f / 480.0f));
        CHECK(near(half.con2[0], -1.0f / 540.0f) && near(half.con2[1], 2.0f / 480.0f));
        CHECK(near(half.con2[2], 1.0f / 540.0f) && near(half.con2[3], 2.0f / 480.0f));
        CHECK(near(half.con3[0], 0.0f) && near(half.con3[1], 4.0f / 480.0f));
        CHECK(near(half.con3[2], 0.0f) && near(half.con3[3], 0.0f));

        // THREE QUARTERS: the same four formulas at a ratio that is NOT a power of two, which is where a port
        // that happens to be right at 0.5 (every tap offset lands on an integer there) stops passing.
        auto const three_quarters = up::make_easu_constants(810, 720, 1080, 960);
        CHECK(near(three_quarters.con0[0], 0.75f) && near(three_quarters.con0[1], 0.75f));
        CHECK(near(three_quarters.con0[2], -0.125f) && near(three_quarters.con0[3], -0.125f));
        CHECK(near(three_quarters.con1[0], 1.0f / 810.0f) && near(three_quarters.con2[1], 2.0f / 720.0f));

        // A ZERO EXTENT IS GUARDED, not undefined behaviour: these are divisions by an extent, and an infinity
        // in a tap position is a frame of garbage rather than a small image.
        auto const guarded = up::make_easu_constants(0, 0, 1080, 960);
        CHECK(std::isfinite(guarded.con0[0]) && std::isfinite(guarded.con0[2]));
        CHECK(std::isfinite(guarded.con1[0]) && std::isfinite(guarded.con1[3]));
        CHECK(std::isfinite(guarded.con3[1]));
    }

    // ---- the SLOT GRID's two sources of truth, compared: the host reserves it in core::heap_slots and the
    //      shaders BAKE the same numbers out of source/shaders/common/heap_slot_constants.slang. Both are text, neither is generated from
    //      the other, and a drift between them is invisible to validation - it shows up only as a wrong picture,
    //      because a heap-native shader indexes the heap by the number it was compiled with. The capture gate
    //      cannot run in CI at all (its references are tied to one machine's driver) and this can, so the two
    //      files are parsed and compared here: the scalars a shader bakes (base, stride, counts), the NAME SET of
    //      the arrays (a rename on either side is a drift), and the value of every array.
    {
        auto const read_lines = [](std::filesystem::path const& path) {
            deren::utility::vessel::vector<deren::utility::vessel::string> lines;
            std::ifstream file(path);
            CHECK_MSG(file.is_open(), path.string().c_str());
            deren::utility::vessel::string line;
            while (std::getline(file, line)) {
                lines.push_back(line);
            }
            return lines;
        };
        /// `name = <base> + <N>u` or `name = <N>u` or `name = <N>` -> the value, resolving `base` from `known`
        auto const value_of = [](deren::utility::vessel::string const& rhs, deren::utility::vessel::map<deren::utility::vessel::string, uint64_t> const& known) -> std::optional<uint64_t> {
            std::size_t const plus = rhs.find(" + ");
            if (plus == deren::utility::vessel::string::npos) {
                return std::strtoull(rhs.c_str(), nullptr, 10);
            }
            auto const base = known.find(rhs.substr(0, plus));
            if (base == known.end()) {
                return std::nullopt;
            }
            return base->second + std::strtoull(rhs.c_str() + plus + 3, nullptr, 10);
        };

        /// The `= <rhs>;` tail of a declaration line, or nullopt when the line is not shaped that way.
        /// NOT `line.substr(eq + 2u, line.find(';', eq) - eq - 2u)`: when the ';' is missing that length comes
        /// from npos, `substr` throws std::out_of_range, and under -fno-exceptions a throw is a fail-fast
        /// abort. This file died with 0xc0000409 exactly once - when a source file it parses had moved and the
        /// parse stopped matching - and a THROWN length was how. A malformed line must be a reported failure,
        /// not a crash.
        auto const rhs_after_eq = [](deren::utility::vessel::string const& line, std::size_t const eq) -> std::optional<deren::utility::vessel::string> {
            std::size_t const semi = line.find(';', eq);
            if (eq == deren::utility::vessel::string::npos || semi == deren::utility::vessel::string::npos || semi <= eq + 2u) {
                return std::nullopt;
            }
            return line.substr(eq + 2u, semi - eq - 2u);
        };

        deren::utility::vessel::map<deren::utility::vessel::string, uint64_t> shader_scalars;
        deren::utility::vessel::map<deren::utility::vessel::string, uint64_t> shader_slots; // the `heap_slots_<name>` arrays
        for (deren::utility::vessel::string const& line : read_lines(deren::utility::vessel::string(VR_TEST_SOURCE_DIR) + "/source/shaders/common/heap_slot_constants.slang")) {
            if (line.rfind("static const uint ", 0) != 0) {
                continue;
            }
            std::size_t const eq = line.find('=');
            if (eq == deren::utility::vessel::string::npos) {
                continue;
            }
            std::size_t const name_end = line.find(' ', 18);
            std::optional<deren::utility::vessel::string> const rhs = rhs_after_eq(line, eq);
            CHECK_MSG(name_end != deren::utility::vessel::string::npos && rhs.has_value(), line.c_str());
            if (name_end == deren::utility::vessel::string::npos || !rhs.has_value()) {
                continue;
            }
            deren::utility::vessel::string const name = line.substr(18, name_end - 18);
            std::optional<uint64_t> const value = value_of(*rhs, shader_scalars);
            CHECK_MSG(value.has_value(), name.c_str());
            if (!value.has_value()) {
                continue;
            }
            if (name.rfind("heap_slots_", 0) == 0) {
                shader_slots[name.substr(11)] = *value;
            } else {
                shader_scalars[name] = *value;
            }
        }

        deren::utility::vessel::map<deren::utility::vessel::string, uint64_t> host_scalars;
        deren::utility::vessel::map<deren::utility::vessel::string, uint64_t> host_slots; // the members of core::heap_slots
        std::size_t parsed_declarations = 0;                                              // the tooth on the braced-initializer skip (see below)
        bool in_slots = false;
        for (deren::utility::vessel::string const& line : read_lines(deren::utility::vessel::string(VR_TEST_SOURCE_DIR) + "/source/engine/render_layout/render_layout.cppm")) {
            if (line.find("struct heap_slots {") != deren::utility::vessel::string::npos) {
                in_slots = true;
                continue;
            }
            if (in_slots && line.find("};") != deren::utility::vessel::string::npos) {
                in_slots = false;
                continue;
            }
            // The grid's namespace-scope scalars are `inline constexpr` - a module cannot export an entity
            // with internal linkage, and namespace-scope `static constexpr` has exactly that linkage -
            // while the struct's members stay `static constexpr` (implicitly inline). Both spellings are
            // the grid's own declarations, so both are read.
            if (line.find("static constexpr") == deren::utility::vessel::string::npos && line.find("inline constexpr") == deren::utility::vessel::string::npos) {
                continue;
            }
            std::size_t const eq = line.find('=');
            if (eq == deren::utility::vessel::string::npos) {
                continue;
            }
            deren::utility::vessel::string lhs = line.substr(0, eq);
            while (!lhs.empty() && lhs.back() == ' ') {
                lhs.pop_back();
            }
            std::size_t const name_begin = lhs.rfind(' ');
            deren::utility::vessel::string const name = lhs.substr(name_begin == deren::utility::vessel::string::npos ? 0u : name_begin + 1u);
            std::optional<deren::utility::vessel::string> const rhs = rhs_after_eq(line, eq);
            // A BRACED INITIALIZER IS NOT A SCALAR, and since A1.0 this file carries more than the grid:
            // `deren.engine.render_layout` also declares the render chain's formats (`gbuffer_formats` is
            // an `deren::utility::vessel::array`).
            //
            // THE BOUNDARY OF THIS SKIP, stated so it cannot be widened by accident: it applies ONLY to an
            // AGGREGATE initializer (`= { ... }`), which neither comparison below can read. A SCALAR
            // `constexpr` (integer, enum, float) whose value this reader cannot parse is still a FAILURE -
            // `CHECK_MSG(rhs.has_value())` below is that tooth - because an unreadable scalar is the parser
            // losing its grip, not the file gaining a declaration.
            //
            // AND THE TOOTH IS COUNTED: `parsed_declarations` has a floor asserted after this loop, so a
            // skip branch that started eating real declarations would fail the test instead of quietly
            // comparing less. (The floor is deliberately below the grid's current size: it catches a
            // BRANCH that swallows the file, not a slot that legitimately moves out.)
            {
                deren::utility::vessel::string const after_eq = line.substr(eq + 1u);
                std::size_t const first = after_eq.find_first_not_of(' ');
                if (first != deren::utility::vessel::string::npos && after_eq[first] == '{') {
                    continue;
                }
            }
            CHECK_MSG(rhs.has_value(), line.c_str());
            if (!rhs.has_value()) {
                continue;
            }
            std::optional<uint64_t> const value = value_of(*rhs, host_scalars);
            if (value.has_value()) {
                ++parsed_declarations;
            }
            if (in_slots && value.has_value()) {
                host_slots[name] = *value;
            } else if (!in_slots && value.has_value() && name.rfind("heap_", 0) == 0) {
                host_scalars[name] = *value;
            }
        }
        // 40-odd slot members plus the seven `heap_*` scalars today; a floor of 30 cannot be reached by a
        // reader that skips real declarations, and it is low enough not to break when a slot legitimately
        // moves out of the grid.
        CHECK_MSG(parsed_declarations >= 30u, "the grid reader stopped reading the grid");
        CHECK_MSG(host_scalars.size() >= 6u, "the grid reader stopped reading the grid's scalar constants");

        // The six SAMPLER slots are named individually in the header, while the host keeps them as an ordered list
        // (core::shared_sampler_infos, written from core::create_samplers). So they are checked against the sampler
        // base rather than against a host constant - which is exactly the order contract both files state, and the
        // one thing a rename there could silently break.
        deren::utility::vessel::array<std::string_view, 6> const sampler_slots = {"heap_sampler_texture", "heap_sampler_post", "heap_sampler_gbuffer", "heap_sampler_post_nearest", "heap_sampler_taa", "heap_sampler_shadow"};
        for (std::size_t i = 0; i < sampler_slots.size(); ++i) {
            auto const found = shader_scalars.find(deren::utility::vessel::string(sampler_slots[i]));
            auto const base = shader_scalars.find("heap_sampler_base");
            CHECK_MSG(found != shader_scalars.end(), sampler_slots[i].data());
            CHECK_MSG(base != shader_scalars.end(), "heap_sampler_base is not declared in source/shaders/common/heap_slot_constants.slang");
            if (found == shader_scalars.end() || base == shader_scalars.end()) {
                continue; // a reported failure, not a thrown std::out_of_range from map::at
            }
            CHECK_MSG(found->second == base->second + i, "a sampler slot is out of order in source/shaders/common/heap_slot_constants.slang");
            shader_scalars.erase(found);
        }

        // the scalars a shader bakes: the grid's base, its stride, how many slots it holds, and the sampler grid's
        CHECK(!shader_scalars.empty());
        CHECK(!host_scalars.empty());
        CHECK_MSG(shader_scalars == host_scalars, "the grid's scalar constants differ between source/shaders/common/heap_slot_constants.slang and core.declarations.cppm");
        CHECK_MSG(shader_slots.size() == host_slots.size(), "the grid has a different number of arrays on the two sides");
        CHECK_MSG(shader_slots == host_slots, "a grid array's slot differs between source/shaders/common/heap_slot_constants.slang and core.declarations.cppm");

        // ---- THE TEXTURE RUN IS THE BINDLESS ARRAY'S CAPACITY ----
        //
        // The array occupies the grid slots between `textures` and the material table, one slot per scene
        // texture, and the RUNTIME refuses to upload past `scene_texture_capacity`. That capacity is DERIVED
        // in `deren.engine.render_layout` (`materials - textures`), so the two parsed tables above and the
        // C++ constant must agree - the shader has no capacity constant of its own, which is exactly why a
        // drift here would be a silent overrun rather than an error.
        {
            auto const textures = host_slots.find("textures");
            auto const materials = host_slots.find("materials");
            auto const shader_textures = shader_slots.find("textures");
            auto const shader_materials = shader_slots.find("materials");
            CHECK_MSG(textures != host_slots.end() && materials != host_slots.end() && shader_textures != shader_slots.end() && shader_materials != shader_slots.end(),
                      "the grid's texture or material slot is not declared");
            if (textures != host_slots.end() && materials != host_slots.end() && shader_textures != shader_slots.end() && shader_materials != shader_slots.end()) {
                CHECK_MSG(materials->second - textures->second == deren::engine::render_layout::scene_texture_capacity, "the host grid's texture run is not the bindless array's capacity");
                CHECK_MSG(shader_materials->second - shader_textures->second == deren::engine::render_layout::scene_texture_capacity, "the shader grid's texture run is not the bindless array's capacity");
            }
        }
        // ... AND THE CAPACITY IS AT LEAST WHAT THE ACCEPTANCE SCENE MEASURED: the textured Bistro asset
        // needs the white fallback plus 516 distinct runtime texture slots (597 image files collapsed by the
        // runtime's content+format dedup). A capacity below that silently paints the surplus textures white.
        static_assert(deren::engine::render_layout::scene_texture_capacity >= 517u, "the bindless texture array cannot hold the textured Bistro acceptance scene");
        // ---- ... and every slot the header names must be one the HOST actually WRITES ----
        //
        // The comparison above keeps the two tables equal; this keeps them MEANINGFUL. A slot no host code ever
        // writes is a name for memory nothing put a descriptor in, and a heap-native shader indexing it reads
        // whatever the heap happened to contain - silently, which is the failure mode this whole file exists to
        // catch. The host side is text too (runtime.cpp, core.cpp and the initialisation partition that now
        // holds core's render-target heap writes are where every heap write lives), so the check is the same
        // kind as the one above, with ONE documented exception: the bloom chain writes its levels with
        // arithmetic (`bloom_l0 + level * heap_image_capacity`), so l1..l3 are named in the header and never
        // spelled out in the host. Every other exception is a BUG, not a style choice.
        //
        // THIS SCANS THE RENDERER'S SOURCES INSTEAD OF NAMING THEM, and that is the fix for a real
        // history: the explicit list broke three times, once per partition that took heap writes
        // (core's constructor and runtime's), because a hand-kept list of "files where a heap write can
        // live" has to be updated by the same person who just moved the writes. A directory walk cannot
        // be forgotten, and it is STRICTER - it sees files the list never had.
        deren::utility::vessel::string host_text;
        // THE RENDERER'S SOURCES LIVE IN TWO DIRECTORIES SINCE S5, and both are walked for the reason this
        // loop replaced the hand-kept file list in the first place: `source/backends/vulkan/` holds the backend and the
        // engine modules around it, and `source/engine/runtime/` at the repository root holds the runtime - which moved
        // out of `source/backends/vulkan/` in S1, while the copy left behind inside `source/engine/runtime/` was still the file
        // this walk found. S5 deleted that copy, so the walk must name the runtime's real home or the
        // needles below would be looked for in a directory that no longer holds them.
        deren::utility::vessel::array<deren::utility::vessel::string, 2> const source_roots = {deren::utility::vessel::string(VR_TEST_SOURCE_DIR) + "/source/backends/vulkan",
                                                                                               deren::utility::vessel::string(VR_TEST_SOURCE_DIR) + "/source/engine"};
        for (deren::utility::vessel::string const& root : source_roots) {
            for (auto const& entry : std::filesystem::recursive_directory_iterator(root)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                auto const extension = entry.path().extension().string();
                if (extension != ".cpp" && extension != ".cppm") {
                    continue;
                }
                for (deren::utility::vessel::string const& line : read_lines(entry.path().string())) {
                    host_text += line;
                    host_text += '\n';
                }
            }
        }
        deren::utility::vessel::array<std::string_view, 3> const written_by_arithmetic = {"bloom_l1", "bloom_l2", "bloom_l3"};
        uint32_t covered = 0;
        for (auto const& [name, value] : shader_slots) {
            if (std::find(written_by_arithmetic.begin(), written_by_arithmetic.end(), name) != written_by_arithmetic.end()) {
                continue;
            }
            deren::utility::vessel::string const needle = "heap_slots::" + name;
            CHECK_MSG(host_text.find(needle) != deren::utility::vessel::string::npos, needle.c_str());
            ++covered;
        }
        CHECK(covered > 20u); // the floor again: a host file that failed to load would pass the loop above

        // ... and a floor under the parse itself: a file that stopped looking like this would otherwise pass by
        // coming out EMPTY on both sides, which is the way a contract test lies.
        CHECK(shader_slots.size() > 20u);
        CHECK(host_scalars.at("heap_slot_base") == 16384u);
    }
    software_resource_families();
    return deren::vk_test::finish("test_render_resources");
}
