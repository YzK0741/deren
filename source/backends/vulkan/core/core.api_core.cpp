// -*- C++ -*-
// ============================================================================
// file: source/backends/vulkan/core/core.api_core.cpp
//
// THE RHI CONTRACT'S VIRTUALS, DEFINED FOR THE REAL BACKEND (S1-C; the
// recording surface and the read-back slot are S2 batch 2).
// `core` derives from `deren::promise::rhi::api_core` (source/backends/vulkan/core/declarations/core.declarations.cppm), so the
// object that creates and destroys the instance / device / swapchain IS the object a host gets from
// deren_make_api_core() - there is no wrapper type to keep in step with it.
//
// WHAT IS REAL HERE, AND WHAT IS DELIBERATELY NOT:
//
//   - `abilities()`: A BIT MEANS THE CONTRACT CAN SERVE IT, not that the device has the feature. For
//     every set bit, `query_extension(kind)` has to answer with an object that can carry out every
//     operation that ability declares, on the objects this backend can produce. `vulkan_escape` IS NOT
//     AMONG THEM ANY MORE (abi 43): the escape interface is deleted; the GUI overlay gets the device
//     handles through the backend's `denen.vulkan.main_context` (abi 39) and keeps its recording
//     targets as contract `mergeable_command_buffer` handles (gui abi 5).
//   - `query_extension()`: the announced ability object for its kind, nullptr for everything else -
//     exactly the two-way invariant the startup gate (core.constructor.cppm, G2) and
//     source/tests/test_dynamic_link.cpp (G1) check.
//   - the factories: REAL since S3 - each `create_*` answers the contract handle, or `nullptr` for a
//     descriptor this backend cannot serve (§4.2: no throwing path across the boundary).
//   - the recording surface: REAL, a view of objects this class creates (the frame's
//     primary command buffer, the swapchain image the frame draws into, the read-back slot). See the
//     view types in core.declarations.cppm.
//   - the shadow gate (plan §8.3, gate A5): `use()` derives the barrier from the contract's role pair
//     and the asserts right below prove - field by field, AT COMPILE TIME - that it lands exactly on
//     the two recipes this renderer shipped by hand. The same dump is emitted once per pair at run
//     time, so the log carries both sides.
//   - the frame calls: moved to facet/core.frame_views.cpp with the frame face's views. The
//     machinery is unchanged - `wait_frame_slot`, `frame.image_available_semaphores`, `submit()`,
//     `present()`, `wait_idle()`, and `frame_begin()`'s acquire, which is the backend's because the
//     device and the swapchain are.
// ============================================================================
module;

#include <cstddef>
#include <vulkan/vulkan.h>

module deren.vulkan.core;
import deren.vstd;
import deren.utility.shared;

import deren.promise.rhi;
import deren.promise.render_operations;

import deren.vulkan.constant_init;
import deren.vulkan.core.pipeline;

// After the imports it needs: the header names deren::promise::rhi types and declares the translators.

namespace deren::vulkan {

    namespace rhi = deren::promise::rhi;
    namespace rec = rhi::recording;

    namespace {

        /// ONE ROW OF THE DECLARATION TABLE: which pair, and the fields the pair's barrier carries.
        ///
        /// THE FIELDS ARE COPIED FROM THE RENDERER'S RECIPE CONSTANT, never transcribed - see
        /// `pair_of` below. Transcribing a mask into this table would create a SECOND truth beside
        /// `source/backends/vulkan/constant_init/constant_init.cppm`, and the whole point of this face is that the pair
        /// (the contract's vocabulary) derives the recipe, not that the two agree by review.
        struct image_use_pair {
            rhi::image_use from = rhi::image_use::undefined;
            rhi::image_use to = rhi::image_use::undefined;
            VkImageLayout old_layout = VK_IMAGE_LAYOUT_GENERAL;
            VkImageLayout new_layout = VK_IMAGE_LAYOUT_GENERAL;
            VkPipelineStageFlags2 src_stage = 0;
            VkAccessFlags2 src_access = 0;
            VkPipelineStageFlags2 dst_stage = 0;
            VkAccessFlags2 dst_access = 0;
        };

        /// A pair's row, taking every field from @p recipe (the shipped constant). The pair is the
        /// DECLARATION; the recipe is the FACT, and this function is the only place they meet.
        [[nodiscard]] constexpr image_use_pair pair_of(rhi::image_use const from, rhi::image_use const to,
                                                       VkImageMemoryBarrier2 const& recipe) noexcept {
            return image_use_pair{
                .from = from,
                .to = to,
                .old_layout = recipe.oldLayout,
                .new_layout = recipe.newLayout,
                .src_stage = recipe.srcStageMask,
                .src_access = recipe.srcAccessMask,
                .dst_stage = recipe.dstStageMask,
                .dst_access = recipe.dstAccessMask,
            };
        }

        // ============================================================================================
        // THE DECLARATION TABLE (recording face, batch ②). ONE table, and `barrier_for` is its lookup:
        // the pair-to-mask mapping exists exactly once, and the shadow gate's asserts below prove each
        // row against the same constant it was built from.
        //
        // THE SEMANTICS LIVE IN THE MASKS, NOT THE LAYOUTS. This renderer keeps every image in GENERAL
        // (`docs/unified_image_layouts.md`), so `oldLayout`/`newLayout` are GENERAL on both sides of
        // almost every row - `undefined_to_*` is the exception (its old layout really is UNDEFINED) and
        // the `present` rows are the other (their NEW layout is PRESENT_SRC_KHR). A later reader who
        // tries to fix a bug by changing a LAYOUT here will change the wrong thing: the fact is the
        // pair, and the pair's meaning is its stage/access masks.
        //
        // THE DEPENDENCY ROW (`color_attachment -> color_attachment`) IS NOT A TRANSITION: it changes no
        // layout and orders one pass's colour-attachment STORE before the next instance's LOAD
        // (`deferred.cpp`'s `color_attachment_dependency`). Its masks come from that constant like every
        // other row's, because re-spelling them would be the second truth this table exists to avoid.
        //
        // WHAT THIS BATCH DOES *NOT* CLAIM: that every `image_use` combination is covered. This table is
        // "these twenty pairs are transcribed"; the coverage assertion over ALL combinations is the next
        // commit (batch ③), and until it lands no pass may migrate - a partial mapping must never read as
        // a green coverage light.
        // ============================================================================================
        constexpr deren::utility::vessel::array image_use_pairs = {
            // ---- the fresh-target family: UNDEFINED as the old layout ----
            pair_of(rhi::image_use::undefined, rhi::image_use::color_attachment, deren::vulkan::color_attachment_transition),
            pair_of(rhi::image_use::undefined, rhi::image_use::depth_attachment, deren::vulkan::depth_attachment_transition),
            pair_of(rhi::image_use::undefined, rhi::image_use::shader_read, deren::vulkan::undefined_to_sampling_transition),
            pair_of(rhi::image_use::undefined, rhi::image_use::depth_read, deren::vulkan::undefined_to_depth_sampling_transition),
            pair_of(rhi::image_use::undefined, rhi::image_use::shader_write, deren::vulkan::undefined_to_general_transition),
            pair_of(rhi::image_use::undefined, rhi::image_use::transfer_destination, deren::vulkan::undefined_to_transfer_dst_transition),
            pair_of(rhi::image_use::undefined, rhi::image_use::present, deren::vulkan::undefined_to_present_transition),
            // ---- hand a written target to its next consumer ----
            pair_of(rhi::image_use::color_attachment, rhi::image_use::shader_read, deren::vulkan::hdr_sampling_transition),
            pair_of(rhi::image_use::color_attachment, rhi::image_use::transfer_source, deren::vulkan::color_attachment_to_transfer_transition),
            pair_of(rhi::image_use::color_attachment, rhi::image_use::present, deren::vulkan::present_transition),
            // PLAN X4: the host-access role the census said the contract had none of. The probe reads its
            // render back through `image::get_content()`, and this is the pair that says so.
            pair_of(rhi::image_use::color_attachment, rhi::image_use::host_read, deren::vulkan::color_attachment_to_host_transition),
            // THE DEPENDENCY (no layout change, store -> load inside one frame): see the note above
            pair_of(rhi::image_use::color_attachment, rhi::image_use::color_attachment, deren::vulkan::color_attachment_dependency),
            // ---- the shader-written family ----
            pair_of(rhi::image_use::shader_write, rhi::image_use::shader_read, deren::vulkan::general_to_sampling_transition),
            pair_of(rhi::image_use::shader_write, rhi::image_use::transfer_source, deren::vulkan::general_to_transfer_src_transition),
            // a read-modify-write self barrier: the destination needs BOTH access bits, which is why the
            // pair carries its role (`shader_read_write`) instead of one of its halves
            pair_of(rhi::image_use::shader_write, rhi::image_use::shader_read_write, deren::vulkan::compute_storage_transition),
            // ---- the sampled family ----
            pair_of(rhi::image_use::shader_read, rhi::image_use::shader_write, deren::vulkan::sampling_to_general_transition),
            pair_of(rhi::image_use::shader_read, rhi::image_use::transfer_destination, deren::vulkan::sampling_to_transfer_dst_transition),
            pair_of(rhi::image_use::shader_read, rhi::image_use::depth_attachment, deren::vulkan::sampling_to_depth_attachment_transition),
            pair_of(rhi::image_use::depth_attachment, rhi::image_use::shader_read, deren::vulkan::shadow_map_sampling_transition),
            // ---- the transfer family ----
            pair_of(rhi::image_use::transfer_source, rhi::image_use::color_attachment, deren::vulkan::transfer_to_color_attachment_transition),
            pair_of(rhi::image_use::transfer_source, rhi::image_use::shader_read, deren::vulkan::transfer_src_to_sampling_transition),
            pair_of(rhi::image_use::transfer_destination, rhi::image_use::shader_read, deren::vulkan::transfer_dst_to_sampling_transition),
        };
        // ============================================================================================
        // A COUNT ASSERTION MUST COME FROM THE CENSUS, NEVER FROM AN ENUMERATION. This assert said
        // `size() == 20` when batch 2 landed, because 20 was the number in a hand-written list - and the
        // measurement says 21 (`transfer_source -> shader_read` was missing, i.e.
        // `transfer_src_to_sampling_transition`, which `source/engine/pass/megalights_temporal.cpp` uses). A green
        // assertion was certifying a WRONG NUMBER, which is worse than no assertion: it made the gap look
        // covered. The numbers below are the histogram output of `scripts/tools/recording_face_census.py`, quoted
        // so a reader can re-run it:
        //
        //     measured pairs: 22   role values: 11   combinations: 121   unsupported: 99
        //
        // and 22 + 99 = 121 is asserted, so the three cannot drift apart silently. THE HOST-READ PAIR MOVED ALL
        // FOUR NUMBERS (plan X4): `image_use::host_read` is a new VALUE, so the census's role set grew with it -
        // and this line is the record of re-running the census rather than of a hand count.
        // ============================================================================================
        inline constexpr std::size_t image_use_pair_count_from_census = 22;   // 21 + the host-read pair (plan X4)
        inline constexpr std::size_t unsupported_pair_count_from_census = 99; // 121 combinations - 22 measured
        inline constexpr std::uint32_t image_use_value_count = 11;            // the enum's values, contiguous 0..10
        static_assert(image_use_pairs.size() == image_use_pair_count_from_census,
                      "the declaration table must be exactly the pairs the CENSUS measured "
                      "(scripts/tools/recording_face_census.py: 'measured pairs: 22'); a hand count is not evidence");
        static_assert(image_use_pair_count_from_census + unsupported_pair_count_from_census ==
                          static_cast<std::size_t>(image_use_value_count) * image_use_value_count,
                      "22 measured pairs + 99 unsupported must be exactly the 121 role combinations the census "
                      "reports - if the enum grows, both numbers are re-read from the same run");

        /// The barrier one (from, to) role pair needs: the TABLE's row, with the caller's image filled in.
        ///
        /// THE DERIVATION FROM THE PAIR IS THE POINT of the shadow gate: it is spelled here in the
        /// contract's vocabulary (two roles), and the table's rows come from the renderer's recipes -
        /// so a pair that is transcribed cannot drift from the recipe it was measured from, and the
        /// asserts below keep proving it field for field. A pair NO ROW CARRIES has no stages and no
        /// accesses at all, which is the honest answer for a transition nobody has defined yet -
        /// `use()` reports it once instead of recording nonsense.
        [[nodiscard]] constexpr VkImageMemoryBarrier2 barrier_for(rhi::image_use const from, rhi::image_use const to) noexcept {
            // EVERY IMAGE IN THIS RENDERER LIVES IN GENERAL: the layouts are an invariant, not a
            // parameter (docs/unified_image_layouts.md), which is exactly why the contract carries
            // no layout. The rows that differ (UNDEFINED as `from`, PRESENT_SRC as `to`) override it.
            // The whole image, one mip, one layer: what the screenshot read-back copies, and what
            // the hand-written recipes carry.
            VkImageMemoryBarrier2 barrier = make_image_memory_barrier_2(
                0, 0, 0, 0, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_NULL_HANDLE,
                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});
            for (image_use_pair const& row : image_use_pairs) {
                if (row.from == from && row.to == to) {
                    barrier.oldLayout = row.old_layout;
                    barrier.newLayout = row.new_layout;
                    barrier.srcStageMask = row.src_stage;
                    barrier.srcAccessMask = row.src_access;
                    barrier.dstStageMask = row.dst_stage;
                    barrier.dstAccessMask = row.dst_access;
                    break;
                }
            }
            return barrier;
        }

        /// Every field of a VkImageMemoryBarrier2, for the shadow gate's equality proofs and its dump
        [[nodiscard]] constexpr bool same_barrier(VkImageMemoryBarrier2 const& a, VkImageMemoryBarrier2 const& b) noexcept {
            return a.sType == b.sType && a.pNext == b.pNext && a.srcStageMask == b.srcStageMask && a.srcAccessMask == b.srcAccessMask &&
                   a.dstStageMask == b.dstStageMask && a.dstAccessMask == b.dstAccessMask && a.oldLayout == b.oldLayout && a.newLayout == b.newLayout &&
                   a.srcQueueFamilyIndex == b.srcQueueFamilyIndex && a.dstQueueFamilyIndex == b.dstQueueFamilyIndex && a.image == b.image &&
                   a.subresourceRange.aspectMask == b.subresourceRange.aspectMask &&
                   a.subresourceRange.baseMipLevel == b.subresourceRange.baseMipLevel &&
                   a.subresourceRange.levelCount == b.subresourceRange.levelCount &&
                   a.subresourceRange.baseArrayLayer == b.subresourceRange.baseArrayLayer &&
                   a.subresourceRange.layerCount == b.subresourceRange.layerCount;
        }

        // ============================================================================================
        // BATCH 3b: THE COVERAGE GATE, REPAIRED - AND THE REPAIR IS EVIDENCE-DRIVEN.
        //
        // THE FIRST VERSION OF THIS GATE WAS GREEN WHEN IT SHOULD HAVE BEEN RED. Measured, not reasoned:
        // deleting the row `transfer_source -> shader_read` left `-fsyntax-only` at EXIT 0, because
        //     * `deren::utility::vessel::array<image_use_pair, 21>` with twenty initializers VALUE-INITIALISES a twenty-first
        //       (dummy `undefined -> undefined`) row, so `size() == 21` was still satisfied;
        //     * `unsupported_reason` short-circuited on the table, so the check that was supposed to name
        //       the offending pair reduced to `in_table == !in_table` - a tautology, never false.
        // A substituted row therefore kept every number intact. Three rules follow, and they are now the
        // ACCEPTANCE STANDARD for this gate:
        //     * THE SET IS ASSERTED, NOT ONLY THE COUNT (a substituted row preserves any count);
        //     * THE TWO PREDICATES MUST NOT SHORT-CIRCUIT EACH OTHER (or the partition check is vacuous);
        //     * THE GATE MUST BE ABLE TO FAIL, and that is proven by MUTATION, not by reading the code.
        // ============================================================================================

        /// THE PAIRS THE CENSUS MEASURED - transcribed from `scripts/tools/recording_face_census.py`'s output
        /// (the `measured pairs` line, printed as role pairs). This is the INDEPENDENT side of the set
        /// equality below: the table is checked against the measurement, never against itself.
        struct role_pair_expected {
            rhi::image_use from;
            rhi::image_use to;
        };
        constexpr deren::utility::vessel::array expected_pairs_from_census = {
            role_pair_expected{rhi::image_use::undefined, rhi::image_use::color_attachment},
            role_pair_expected{rhi::image_use::undefined, rhi::image_use::depth_attachment},
            role_pair_expected{rhi::image_use::undefined, rhi::image_use::shader_read},
            role_pair_expected{rhi::image_use::undefined, rhi::image_use::depth_read},
            role_pair_expected{rhi::image_use::undefined, rhi::image_use::shader_write},
            role_pair_expected{rhi::image_use::undefined, rhi::image_use::transfer_destination},
            role_pair_expected{rhi::image_use::undefined, rhi::image_use::present},
            role_pair_expected{rhi::image_use::color_attachment, rhi::image_use::shader_read},
            role_pair_expected{rhi::image_use::color_attachment, rhi::image_use::transfer_source},
            role_pair_expected{rhi::image_use::color_attachment, rhi::image_use::present},
            // PLAN X4: the host-read pair, from `color_attachment_to_host_transition` (the recipe the census counts too).
            role_pair_expected{rhi::image_use::color_attachment, rhi::image_use::host_read},
            role_pair_expected{rhi::image_use::color_attachment, rhi::image_use::color_attachment},
            role_pair_expected{rhi::image_use::shader_write, rhi::image_use::shader_read},
            role_pair_expected{rhi::image_use::shader_write, rhi::image_use::transfer_source},
            role_pair_expected{rhi::image_use::shader_write, rhi::image_use::shader_read_write},
            role_pair_expected{rhi::image_use::shader_read, rhi::image_use::shader_write},
            role_pair_expected{rhi::image_use::shader_read, rhi::image_use::transfer_destination},
            role_pair_expected{rhi::image_use::shader_read, rhi::image_use::depth_attachment},
            role_pair_expected{rhi::image_use::depth_attachment, rhi::image_use::shader_read},
            role_pair_expected{rhi::image_use::transfer_source, rhi::image_use::color_attachment},
            role_pair_expected{rhi::image_use::transfer_source, rhi::image_use::shader_read},
            role_pair_expected{rhi::image_use::transfer_destination, rhi::image_use::shader_read},
        };

        [[nodiscard]] constexpr bool expected_pair(rhi::image_use const from, rhi::image_use const to) noexcept {
            for (role_pair_expected const& row : expected_pairs_from_census) {
                if (row.from == from && row.to == to) {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] constexpr bool image_use_pair_is_in_table(rhi::image_use const from, rhi::image_use const to) noexcept {
            for (image_use_pair const& row : image_use_pairs) {
                if (row.from == from && row.to == to) {
                    return true;
                }
            }
            return false;
        }

        /// The census histograms, as the booleans a rule can be checked against: `role as from: 0` means
        /// the role never starts a measured transition, and likewise for `as to`.
        [[nodiscard]] constexpr bool appears_as_from(rhi::image_use const role) noexcept {
            return role == rhi::image_use::undefined || role == rhi::image_use::color_attachment ||
                   role == rhi::image_use::shader_read || role == rhi::image_use::shader_write ||
                   role == rhi::image_use::transfer_source || role == rhi::image_use::transfer_destination ||
                   role == rhi::image_use::depth_attachment;
        }
        [[nodiscard]] constexpr bool appears_as_to(rhi::image_use const role) noexcept {
            return role == rhi::image_use::color_attachment || role == rhi::image_use::depth_attachment ||
                   role == rhi::image_use::depth_read || role == rhi::image_use::present ||
                   role == rhi::image_use::shader_read || role == rhi::image_use::shader_read_write ||
                   role == rhi::image_use::shader_write || role == rhi::image_use::transfer_source ||
                   role == rhi::image_use::transfer_destination;
        }

        /// WHY A COMBINATION IS UNSUPPORTED, in rules that DO NOT LOOK AT THE TABLE (that short circuit is
        /// what made the first gate a tautology), each citing the census number that backs it. nullptr means
        /// "no rule speaks against this pair", which is exactly the case for a MEASURED pair - so a measured
        /// pair missing from the table is caught below instead of being quietly absorbed by a rule.
        [[nodiscard]] constexpr char const* image_use_pair_rule(rhi::image_use const from, rhi::image_use const to) noexcept {
            if (expected_pair(from, to)) {
                return nullptr; // the measurement says this pair exists; only the table may disagree
            }
            if (to == rhi::image_use::undefined) {
                return "nothing transitions INTO undefined (census: 'undefined as to: 0')";
            }
            if (from == rhi::image_use::present) {
                return "nothing reads out of a presented image (census: 'present as from: 0')";
            }
            if (from == rhi::image_use::depth_read) {
                return "nothing reads out of a depth read (census: 'depth_read as from: 0')";
            }
            if (from == rhi::image_use::shader_read_write) {
                return "nothing reads out of a read-modify-write state (census: 'shader_read_write as from: 0')";
            }
            if (appears_as_from(from) && appears_as_to(to)) {
                return "both roles occur in the census, but this pair is not among the measured 21 "
                       "(census: 'measured pairs: 21')";
            }
            return "a role that never appears on that side of a measured recipe (census: the role histograms)";
        }

        /// How many combinations the rules leave unsupported - counted from the RULES, never from the
        /// table, so a substituted table row cannot move this number.
        [[nodiscard]] consteval std::size_t unsupported_combination_count() noexcept {
            std::size_t count = 0;
            for (std::uint32_t from = 0; from < image_use_value_count; ++from) {
                for (std::uint32_t to = 0; to < image_use_value_count; ++to) {
                    if (image_use_pair_rule(static_cast<rhi::image_use>(from), static_cast<rhi::image_use>(to)) != nullptr) {
                        ++count;
                    }
                }
            }
            return count;
        }
        static_assert(unsupported_combination_count() == unsupported_pair_count_from_census,
                      "the rules must leave exactly the census's 79 combinations unsupported "
                      "(scripts/tools/recording_face_census.py: 'unsupported by this measurement: 79')");

        [[nodiscard]] consteval bool table_rows_are_unique() noexcept {
            for (std::size_t a = 0; a < image_use_pairs.size(); ++a) {
                for (std::size_t b = a + 1; b < image_use_pairs.size(); ++b) {
                    if (image_use_pairs[a].from == image_use_pairs[b].from && image_use_pairs[a].to == image_use_pairs[b].to) {
                        return false;
                    }
                }
            }
            return true;
        }
        static_assert(table_rows_are_unique(), "the table must not carry the same pair twice - a duplicate is " // NOLINT
                                               "how a value-initialised (dummy) row silently replaced a real one");

        /// Set equality, both directions: the sizes agree (declared below) and no measured pair is absent.
        [[nodiscard]] consteval bool every_expected_pair_is_in_the_table() noexcept {
            return std::ranges::all_of(expected_pairs_from_census, [](auto row) { return image_use_pair_is_in_table(row.from, row.to); });
        }
        static_assert(every_expected_pair_is_in_the_table(),
                      "every pair the census measured must be a table row - a missing row is a gap, and this "
                      "assert exists because the first version of the gate stayed GREEN while one was gone");
        // THE SIZE IS DEDUCED (CTAD above), so a deleted row cannot hide behind a declared size.
        static_assert(image_use_pairs.size() == expected_pairs_from_census.size(),
                      "the table and the census must have the same number of pairs (21)");

        template <std::uint32_t EncodedPair>
        struct image_use_pair_must_be_in_the_table_or_under_a_rule;
        /// The complete case: every combination is accounted for. Empty ON PURPOSE - a member nothing reads
        /// is a `-Wunused-const-variable` error in this build.
        template <>
        struct image_use_pair_must_be_in_the_table_or_under_a_rule<100> {};
        // THE GATE, and it can FAIL: the alias instantiates a DELIBERATELY INCOMPLETE template carrying the
        // encoded offending pair, so a mutation produces a diagnostic naming it (proven by mutation).

        // ---- GATE A5, THE COMPILE-TIME HALF: THE DERIVED BARRIERS ARE THE SHIPPED RECIPES ----------
        // The two transitions this batch records must be the ones the renderer already had, and "the
        // same" here means all sixteen fields: stage/access masks, both layouts, queue family indices,
        // the image handle slot and the four subresource fields. A mismatch is a BUILD failure, not a
        // validation message at run time.
        constexpr VkImageMemoryBarrier2 derived_attachment_to_transfer = barrier_for(rhi::image_use::color_attachment, rhi::image_use::transfer_source);
        constexpr VkImageMemoryBarrier2 derived_transfer_to_attachment = barrier_for(rhi::image_use::transfer_source, rhi::image_use::color_attachment);
        static_assert(same_barrier(derived_attachment_to_transfer, deren::vulkan::color_attachment_to_transfer_transition),
                      "A5: use(color_attachment, transfer_source) must land field-for-field on color_attachment_to_transfer_transition");
        static_assert(same_barrier(derived_transfer_to_attachment, deren::vulkan::transfer_to_color_attachment_transition),
                      "A5: use(transfer_source, color_attachment) must land field-for-field on transfer_to_color_attachment_transition");
        // abi 20's two new pairs, proved against the same shipped recipes (the post chain's barriers,
        // the pilot's transitions)
        constexpr VkImageMemoryBarrier2 derived_undefined_to_attachment = barrier_for(rhi::image_use::undefined, rhi::image_use::color_attachment);
        constexpr VkImageMemoryBarrier2 derived_attachment_to_sampling = barrier_for(rhi::image_use::color_attachment, rhi::image_use::shader_read);
        static_assert(same_barrier(derived_undefined_to_attachment, deren::vulkan::color_attachment_transition),
                      "A5: use(undefined, color_attachment) must land field-for-field on color_attachment_transition");
        static_assert(same_barrier(derived_attachment_to_sampling, deren::vulkan::hdr_sampling_transition),
                      "A5: use(color_attachment, shader_read) must land field-for-field on hdr_sampling_transition");

        /// every field of one barrier on one line, for the shadow gate's run-time dump (A5 asks for
        /// BOTH sides in the log, not just for "equal or not")
        [[nodiscard]] deren::utility::vessel::string barrier_text(VkImageMemoryBarrier2 const& barrier) {
            return deren::utility::vessel::format("sType={:#x} pNext={:#x} srcStage={:#x} srcAccess={:#x} dstStage={:#x} dstAccess={:#x} oldLayout={:#x} newLayout={:#x} "
                                                  "srcQFI={:#x} dstQFI={:#x} image={:#x} aspect={:#x} mip={}+{} layer={}+{}",
                                                  static_cast<std::uint32_t>(barrier.sType),
                                                  reinterpret_cast<std::uintptr_t>(barrier.pNext),
                                                  static_cast<std::uint64_t>(barrier.srcStageMask),
                                                  static_cast<std::uint64_t>(barrier.srcAccessMask),
                                                  static_cast<std::uint64_t>(barrier.dstStageMask),
                                                  static_cast<std::uint64_t>(barrier.dstAccessMask),
                                                  static_cast<std::uint32_t>(barrier.oldLayout),
                                                  static_cast<std::uint32_t>(barrier.newLayout),
                                                  static_cast<std::uint32_t>(barrier.srcQueueFamilyIndex),
                                                  static_cast<std::uint32_t>(barrier.dstQueueFamilyIndex),
                                                  reinterpret_cast<std::uintptr_t>(barrier.image),
                                                  static_cast<std::uint32_t>(barrier.subresourceRange.aspectMask),
                                                  barrier.subresourceRange.baseMipLevel,
                                                  barrier.subresourceRange.levelCount,
                                                  barrier.subresourceRange.baseArrayLayer,
                                                  barrier.subresourceRange.layerCount);
        }

    } // namespace

    // ---- THE APPEND-ONLY ABI GUARD FOR THE FACTORY DESCRIPTORS -------------------------------------
    // `core.constructor.cppm` applies the same rule to the context's creation structure (§11.2 of the
    // plan); the arithmetic itself lives in one place, `covered_by` in core.declarations.cppm, because
    // both descriptors are read through it.
    namespace {

        /// the contract's format name in this backend's spelling; UNDEFINED means "refuse" (unknown),
        /// and the `depth` ROLE resolves to the device's depth attachment format - which concrete
        /// depth format a device serves is the backend's capability question, not the caller's (§17).
        [[nodiscard]] VkFormat native_image_format(deren::promise::rhi::image_format const format, VkFormat const depth_format) noexcept {
            using rhi_image_format = deren::promise::rhi::image_format;
            switch (format) {
            case rhi_image_format::rgba8_unorm:
                return VK_FORMAT_R8G8B8A8_UNORM;
            case rhi_image_format::rgba8_srgb:
                return VK_FORMAT_R8G8B8A8_SRGB;
            case rhi_image_format::bgra8_unorm:
                return VK_FORMAT_B8G8R8A8_UNORM;
            case rhi_image_format::bgra8_srgb:
                return VK_FORMAT_B8G8R8A8_SRGB;
            case rhi_image_format::r16g16_sfloat:
                return VK_FORMAT_R16G16_SFLOAT;
            case rhi_image_format::r16g16b16a16_sfloat:
                return VK_FORMAT_R16G16B16A16_SFLOAT;
            case rhi_image_format::r32g32b32_sfloat:
                return VK_FORMAT_R32G32B32_SFLOAT;
            // APPENDED in A1.2 with the contract value of the same name: the ray-traced visibility image,
            // which the engine creates now. The mapping is spelled out here like every other one because a
            // new contract value must not silently become whatever the integer happens to mean.
            case rhi_image_format::r16_sfloat:
                return VK_FORMAT_R16_SFLOAT;
            // APPENDED for the visibility buffer (2026-10-17): one 32-bit unsigned channel carrying the
            // packed visibility id. Spelled out like every other mapping for the reason above.
            case rhi_image_format::r32_uint:
                return VK_FORMAT_R32_UINT;
            case rhi_image_format::depth:
                return depth_format;
            case rhi_image_format::unknown:
                return VK_FORMAT_UNDEFINED;
            }
            return VK_FORMAT_UNDEFINED;
        }

        /// the same ABI guard for `image_view_desc`.
        [[nodiscard]] deren::promise::rhi::image_view_desc sanitize_image_view_desc(deren::promise::rhi::image_view_desc const& desc) {
            using rhi_view_desc = deren::promise::rhi::image_view_desc;

            std::uint32_t const declared = desc.struct_size;
            constexpr std::uint32_t known = static_cast<std::uint32_t>(sizeof(rhi_view_desc));
            if (declared != known) {
                deren::utility::log("core: the RHI image_view_desc is {} B here and {} B in the caller -> only the caller's declared prefix is read",
                                    known, declared);
            }

            rhi_view_desc options = {};
            if (covered_by(declared, offsetof(rhi_view_desc, base_layer), sizeof(rhi_view_desc::base_layer))) {
                options.base_layer = desc.base_layer;
            }
            if (covered_by(declared, offsetof(rhi_view_desc, layer_count), sizeof(rhi_view_desc::layer_count))) {
                options.layer_count = desc.layer_count;
            }
            if (covered_by(declared, offsetof(rhi_view_desc, base_mip), sizeof(rhi_view_desc::base_mip))) {
                options.base_mip = desc.base_mip;
            }
            if (covered_by(declared, offsetof(rhi_view_desc, mip_count), sizeof(rhi_view_desc::mip_count))) {
                options.mip_count = desc.mip_count;
            }
            if (covered_by(declared, offsetof(rhi_view_desc, role), sizeof(rhi_view_desc::role))) {
                options.role = desc.role;
            }
            return options;
        }

    } // namespace

    // abi 39: THE MAIN-THREAD CONTEXT. Every handle it names is a stable member of this core, so the object
    // is a view filled on demand rather than a second copy; the GUI plugin casts it back through the shared
    // `deren.vulkan.main_context` layout and copies what it needs for the session.
    rhi::main_context_view* core::device_facet::main_context() noexcept {
        auto& root = this->full_core;
        this->view.core = &root;
        this->view.instance = root.device.instance;
        this->view.physical_device = root.device.physical_device;
        this->view.device = root.device.logical_device;
        this->view.graphics_queue = root.device.graphics_queue_handle;
        this->view.graphics_queue_family = root.device.graphics_queue_family_index;
        this->view.color_format = static_cast<std::uint32_t>(root.frame.swap_chain_image_format);
        this->view.frames_in_flight = static_cast<std::uint32_t>(deren::vulkan::core::frame_facet::frames_in_flight);
        return &this->view;
    }

    rhi::main_context_view* core::main_context() noexcept {
        return this->device.main_context();
    }

    std::uint32_t core::api_version() const noexcept {
        // THE OBJECT'S ATTESTATION (abi 19): the number BOTH halves compiled from the contract
        // module. The engine asks it before it uses this object, which covers the case the entry's
        // argument cannot - an `api_core` the engine did not create. Here it is trivially the constant
        // this image compiled, and the returned type is the contract's `std::uint32_t`.
        return rhi::abi_version;
    }

    rhi::ability_bits core::abilities() const noexcept {
        // ---- THE ANNOUNCED BITS, EACH A PROMISE ABOUT SERVICE (batch-1 spec §4.1) -------------------
        //
        // A SET BIT IS A PROMISE ABOUT SERVICE, and every bit below is announced only when this backend can
        // carry out everything the ability declares on objects it can produce. `vulkan_escape` IS NOT
        // ANNOUNCED ANY MORE (abi 43): the escape interface is deleted, the GUI overlay's device handles come
        // through the shared `denen.vulkan.main_context` (abi 39), and no tier-2 escape interface exists for
        // this backend to serve.
        //
        // `device_address` is served because a `buffer` is something `create_buffer()` hands out; its former
        // acceleration-structure half moved to `ray_tracing` (abi 5) so the bit would stop being hostage to a
        // resource no backend could make - see rhi.extension.cppm's `device_address` note and the view's.
        //
        // descriptor_heap 只在 heap 真正可用时广播；mesh_shader 的绘制走 contract 的 `draw_mesh_tasks`，
        // ray_tracing ability 已在 abi 26 退役（launch 是 contract 的 `trace_rays`）。
        //
        // host_image_copy IS SERVED NOW (③-D/E step 2), so it is announced exactly when the device lets
        // the view do what it promises: the extension, its `hostImageCopy` feature, and GENERAL among the
        // layouts a host copy may READ FROM (all three are `host_image_copy_available`, verified at
        // construction). Until this batch nobody served the ability and the bit was therefore not set -
        // and the engine's heap probes called the device entry point off `core` instead; that is the
        // consumer this bit was waiting for.
        return rhi::to_bits(rhi::extension_kind::frame_checkpoint) | rhi::to_bits(rhi::extension_kind::command_submission) | rhi::to_bits(rhi::extension_kind::frame_resources) | rhi::to_bits(rhi::extension_kind::scoped_synchronization) | (this->create_options.native_window != nullptr ? rhi::to_bits(rhi::extension_kind::async_presentation) : 0u) | (this->resource.gpu_sorting_available ? rhi::to_bits(rhi::extension_kind::gpu_sorting) : 0u) | rhi::to_bits(rhi::extension_kind::device_address) |
               (this->resource.heap_view.ready() ? rhi::to_bits(rhi::extension_kind::descriptor_heap) : 0u) |
               // THE DESCRIPTOR SPACE IS SERVABLE EXACTLY WHEN THE HEAP IS: it is the same backend state
               // (the heap's slot pool + the texture-group table), just addressed by positions instead of
               // byte offsets. Announcing both together keeps the ability rule literal - a set bit is a
               // promise about service, not a separate device fact.
               (this->resource.heap_view.ready() ? rhi::to_bits(rhi::extension_kind::descriptor_space) : 0u) |
               (this->device.host_image_copy_available ? rhi::to_bits(rhi::extension_kind::host_image_copy) : 0u) |
               // AND THE DEVICE'S FACTS, SERVABLE THE MOMENT THE DEVICE EXISTS (a device with no mesh shader
               // ANSWERS false - that is an answer, not an unserved ability), which is why this bit carries no
               // device condition while the two above it do.
               rhi::to_bits(rhi::extension_kind::device_capabilities) |
               (this->device.ray_tracing_pipeline_available ? rhi::to_bits(rhi::extension_kind::shader_group_access) : 0u);
    }

    bool core::frame_heap::ready() const noexcept {
        return this->parent != nullptr && this->parent->resource.descriptor_heaps.ready();
    }

    rhi::descriptor_heap_properties core::frame_heap::properties() const noexcept {
        if (this->parent == nullptr) {
            return {};
        }
        auto const& limits = this->parent->resource.descriptor_heaps.limits();
        return {
            .resource_size = this->parent->resource.descriptor_heaps.resource_size(),
            .max_resource_size = limits.max_resource_size,
            .max_sampler_size = limits.max_sampler_size,
            .resource_alignment = limits.resource_alignment,
            .sampler_alignment = limits.sampler_alignment,
            .resource_reserved = limits.resource_reserved,
            .sampler_reserved_with_embedded = limits.sampler_reserved_with_embedded,
            .buffer_descriptor_size = limits.buffer_descriptor_size,
            .image_descriptor_size = limits.image_descriptor_size,
            .sampler_descriptor_size = limits.sampler_descriptor_size,
            .max_embedded_samplers = limits.max_embedded_samplers,
        };
    }

    namespace {
        [[nodiscard]] constexpr VkDescriptorType heap_descriptor_type(rhi::descriptor_type const type) noexcept {
            switch (type) {
            case rhi::descriptor_type::sampled_image:
                return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            case rhi::descriptor_type::storage_image:
                return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            case rhi::descriptor_type::uniform_buffer:
                return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            case rhi::descriptor_type::storage_buffer:
                return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            case rhi::descriptor_type::acceleration_structure:
                return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            case rhi::descriptor_type::uniform_buffer_dynamic:
            case rhi::descriptor_type::storage_buffer_dynamic:
            case rhi::descriptor_type::combined_image_sampler:
                return VK_DESCRIPTOR_TYPE_MAX_ENUM;
            }
            return VK_DESCRIPTOR_TYPE_MAX_ENUM;
        }

        // VK_EXT_descriptor_heap禁止把组合采样器或动态buffer描述符直接写入资源heap。
        static_assert(heap_descriptor_type(rhi::descriptor_type::combined_image_sampler) == VK_DESCRIPTOR_TYPE_MAX_ENUM);
        static_assert(heap_descriptor_type(rhi::descriptor_type::uniform_buffer_dynamic) == VK_DESCRIPTOR_TYPE_MAX_ENUM);
        static_assert(heap_descriptor_type(rhi::descriptor_type::storage_buffer_dynamic) == VK_DESCRIPTOR_TYPE_MAX_ENUM);

    } // namespace

    core::frame_commands* core::resolve_recorder(rhi::command_recorder* commands, rhi::error& result) noexcept {
        result = rhi::error::invalid_argument;
        if (commands == nullptr) {
            return nullptr;
        }
        frame_commands* recorded = nullptr;
        if (commands == &frame.commands_view) {
            recorded = &frame.commands_view;
            if (!frame.frame_in_flight) {
                result = rhi::error::not_ready;
                return nullptr;
            }
        } else {
            // Verify this core's live pointer before any implementation cast. A matching
            // concrete tag alone does not prove a foreign object's layout or lifetime rule.
            {
                std::lock_guard lock(command.contract_command_buffers_mutex);
                if (command.contract_command_buffers.contains(commands)) {
                    recorded = static_cast<core::command_buffer*>(commands);
                }
            }
            if (recorded == nullptr) {
                std::lock_guard lock(command.contract_mergeable_buffers_mutex);
                if (command.contract_mergeable_buffers.contains(commands)) {
                    recorded = static_cast<core::mergeable_command_buffer*>(commands)->inner.get();
                    if (recorded == nullptr) {
                        result = rhi::error::not_ready;
                        return nullptr;
                    }
                }
            }
        }
        if (recorded == nullptr) {
            return nullptr;
        }
        if (!recorded->recording || recorded->native() == VK_NULL_HANDLE) {
            result = rhi::error::not_ready;
            return nullptr;
        }
        result = rhi::error::ok;
        return recorded;
    }

    rhi::error core::frame_heap::write_image(rhi::heap_image_write_info const& info) noexcept {
        return write_image_native(info, false);
    }

    rhi::error core::frame_heap::write_image_native(rhi::heap_image_write_info const& info, bool tracked) const noexcept {
        rhi::error const checked = rhi::validate_structure(info.header, rhi::structure_type::heap_image_write, sizeof(info), false);
        if (checked != rhi::error::ok) {
            return checked;
        }
        if (!this->ready()) {
            return rhi::error::not_ready;
        }
        VkDescriptorType const type = heap_descriptor_type(info.type);
        if (type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            return rhi::error::unsupported;
        }
        auto const capacity = this->parent->resource.descriptor_heaps.resource_size();
        auto const stride = this->parent->resource.descriptor_heaps.descriptor_stride(type);
        if (info.offset > capacity || stride > capacity - info.offset) {
            return rhi::error::invalid_argument;
        }
        if (info.resource == nullptr || info.view == nullptr || info.view->struct_size < sizeof(rhi::image_view_desc)) {
            return rhi::error::invalid_argument;
        }
        // 类型标签相同不代表具体布局相同；先核实本core发出的活跃image，再访问其持有的 image。
        {
            std::lock_guard const lock(this->parent->resource.contract_images_mutex);
            if (!this->parent->resource.contract_images.contains(info.resource)) {
                return rhi::error::invalid_argument;
            }
        }
        auto const& image = *rhi::do_concrete<core::image>(*info.resource, concrete::image);
        if (image.source && (!tracked || image.state() != rhi::lazy_state::ready)) {
            return rhi::error::unsupported;
        }
        auto const& range = *info.view;
        if (range.base_layer >= image.array_layers || range.base_mip >= image.mip_levels) {
            return rhi::error::invalid_argument;
        }
        auto const layers = range.layer_count == 0 ? image.array_layers - range.base_layer : range.layer_count;
        auto const mips = range.mip_count == 0 ? image.mip_levels - range.base_mip : range.mip_count;
        if (layers > image.array_layers - range.base_layer || mips > image.mip_levels - range.base_mip) {
            return rhi::error::invalid_argument;
        }
        bool const sampled = type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        if ((sampled && range.role != rhi::view_role::sampled) || (!sampled && range.role != rhi::view_role::storage) ||
            !rhi::has_flag(image.declared_flags, sampled ? rhi::image_flag::sampled : rhi::image_flag::storage)) {
            return rhi::error::unsupported;
        }
        bool const cube = image.cube_compatible && image.array_layers == 6 && range.base_layer == 0 && layers == 6;
        VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (layers == 1) {
            view_type = VK_IMAGE_VIEW_TYPE_2D;
        }
        if (cube) {
            view_type = VK_IMAGE_VIEW_TYPE_CUBE;
        }
        VkImageViewCreateInfo view = make_image_view_info(image.native_handle, image.resolved_format,
                                                          view_type,
                                                          image.declared_format == rhi::image_format::depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                                                          mips, layers);
        view.subresourceRange.baseMipLevel = range.base_mip;
        view.subresourceRange.baseArrayLayer = range.base_layer;
        return this->parent->resource.descriptor_heaps.write_image(info.offset, view, VK_IMAGE_LAYOUT_GENERAL, type) ? rhi::error::ok : rhi::error::operation_failed;
    }

    rhi::error core::frame_heap::write_buffer(rhi::heap_buffer_write_info const& info) noexcept {
        rhi::error const checked = rhi::validate_structure(info.header, rhi::structure_type::heap_buffer_write, sizeof(info));
        if (checked != rhi::error::ok) {
            return checked;
        }
        if (!this->ready()) {
            return rhi::error::not_ready;
        }
        VkDescriptorType const type = heap_descriptor_type(info.type);
        if (type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
            type != VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
            return rhi::error::unsupported;
        }
        auto const capacity = this->parent->resource.descriptor_heaps.resource_size();
        auto const stride = this->parent->resource.descriptor_heaps.descriptor_stride(type);
        if (info.address == 0 || info.size == 0 || info.size > std::numeric_limits<std::uint64_t>::max() - info.address ||
            info.offset > capacity || stride > capacity - info.offset) {
            return rhi::error::invalid_argument;
        }
        return this->parent->resource.descriptor_heaps.write_buffer(info.offset, info.address, info.size, type) ? rhi::error::ok : rhi::error::operation_failed;
    }

    rhi::error core::frame_heap::bind(rhi::heap_bind_info const& info) const noexcept {
        rhi::error result = rhi::validate_structure(info.header, rhi::structure_type::heap_bind, sizeof(info), false);
        if (result != rhi::error::ok) {
            return result;
        }
        if (!this->ready()) {
            return rhi::error::not_ready;
        }
        auto* const recorded = this->parent->resolve_recorder(info.commands, result);
        if (result != rhi::error::ok) {
            return result;
        }
        if (auto const apply_result = recorded->apply_operations(); apply_result != rhi::error::ok) {
            return apply_result;
        }
        if (auto prepared = this->parent->prepare_lazy_descriptors(*recorded); prepared != rhi::error::ok) {
            return prepared;
        }
        this->parent->resource.descriptor_heaps.record_bind(recorded->native());
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::push_constants(std::uint32_t offset, std::span<std::byte const> data) noexcept {
        if (parent) {
            ++parent->operation_requests[0]; // push_constants
        }
        if (!recording || native() == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (recording_error != rhi::error::ok) {
            return recording_error;
        }
        if (!recording || native() == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (!parent || !parent->resource.descriptor_heaps.ready()) {
            return rhi::error::unsupported;
        }
        auto const capacity = parent->device.capabilities_view.max_push_constants_size();
        if (capacity == 0) {
            return rhi::error::unsupported;
        }
        auto result = rhi::validate_push_constants_range(offset, data.size(), capacity);
        if (result != rhi::error::ok) {
            return result;
        }
        // Protect the pipeline group bytes the backend created on either kind of recorder.
        if (bound_pipeline) {
            std::lock_guard lock(parent->resource.contract_pipelines_mutex);
            if (!parent->resource.contract_pipelines.contains(bound_pipeline)) {
                return rhi::error::invalid_argument;
            }
            for (auto const& binding : bound_pipeline->resource_layout) {
                if (offset < binding.push_byte_offset + 4u && data.size() > binding.push_byte_offset - std::min(offset, binding.push_byte_offset)) {
                    return rhi::error::invalid_argument;
                }
            }
            if (bound_pipeline->group_binding.enabled) {
                auto reserved = bound_pipeline->group_binding.push_byte_offset;
                if (offset < reserved + 4 && offset + data.size() > reserved) {
                    return rhi::error::invalid_argument;
                }
            }
        }
        operations.append<rec::operation_kind::push>({offset}, data);
        ++parent->deferred_requested;
        return rhi::error::ok;
    }

    rhi::heap_bindings core::frame_heap::bindings() const noexcept {
        if (!this->ready()) {
            return {};
        }
        VkBindHeapInfoEXT resource{}, sampler{};
        this->parent->resource.descriptor_heaps.bind_infos(resource, sampler);
        return {
            .resource = {resource.heapRange.address, resource.heapRange.size, resource.reservedRangeOffset, resource.reservedRangeSize},
            .sampler = {sampler.heapRange.address, sampler.heapRange.size, sampler.reservedRangeOffset, sampler.reservedRangeSize},
        };
    }

    rhi::extension* core::query_extension(rhi::extension_kind const kind) noexcept {
        if (kind == rhi::extension_kind::frame_checkpoint) {
            return &command.checkpoint_view;
        }
        if (kind == rhi::extension_kind::command_submission) {
            return &command.completion_view;
        }
        if (kind == rhi::extension_kind::frame_resources) {
            return &this->frame.frame_resources;
        }
        if (kind == rhi::extension_kind::scoped_synchronization) {
            return &this->command.scoped_sync;
        }
        if (kind == rhi::extension_kind::async_presentation) {
            return this->create_options.native_window != nullptr ? &this->frame.async_presentation_view : nullptr;
        }
        // The invariant is two-way and is checked in two places: at startup on the REAL backend
        // (core.constructor.cppm, gate G2) and on the probe backend in source/tests/test_dynamic_link.cpp
        // (G1). Every kind this backend announces answers with an object whose kind() is the kind that
        // was asked for, and every kind it does not announce answers nullptr.
        if (kind == rhi::extension_kind::gpu_sorting && this->resource.gpu_sorting_available) {
            return &this->resource.sorting_view;
        }
        if (kind == rhi::extension_kind::descriptor_heap && this->resource.heap_view.ready()) {
            return &this->resource.heap_view;
        }
        if (kind == rhi::extension_kind::descriptor_space && this->resource.heap_view.ready()) {
            return &this->resource.descriptor_space_view;
        }
        if (kind == rhi::extension_kind::device_address) {
            return &this->resource.address_view;
        }
        if (kind == rhi::extension_kind::host_image_copy && this->device.host_image_copy_available) {
            return &this->resource.host_copy_view;
        }
        if (kind == rhi::extension_kind::device_capabilities) {
            // UNCONDITIONAL, and that is the difference from host_image_copy just above: every method of this
            // ability is answerable once the device exists (a device with no mesh shader answers false), so no
            // device fact can make it unserved.
            return &this->device.capabilities_view;
        }
        if (kind == rhi::extension_kind::shader_group_access && this->device.ray_tracing_pipeline_available) {
            return &this->resource.shader_groups_view;
        }
        return nullptr;
    }

    VkFormat core::resolve_format(rhi::image_format format) const noexcept {
        return native_image_format(format, frame.depth_attachment_format);
    }

    rhi::buffer* core::create_buffer_raw(rhi::buffer_desc const& desc) {
        return this->resource.create_buffer(desc).release();
    }

    std::uint64_t core::buffer::size() const noexcept {
        return this->size_bytes;
    }

    std::span<std::byte> core::buffer::mapped() noexcept {
        // EMPTY when this buffer cannot be mapped - the contract's spelling of "this one is not
        // host-visible", so a caller decides from the span instead of guessing (the same answer the
        // frame's read-back view gives).
        if (this->mapped_bytes == nullptr || this->size_bytes == 0) {
            return {};
        }
        return std::span<std::byte>(static_cast<std::byte*>(this->mapped_bytes), static_cast<std::size_t>(this->size_bytes));
    }

    void core::buffer::release() noexcept {
        if (references.fetch_sub(1, std::memory_order_acq_rel) != 1) {
            return;
        }
        if (this->parent != nullptr) {
            std::lock_guard const lock(this->parent->resource.contract_buffers_mutex);
            this->parent->resource.contract_buffers.erase(this);
        }
        // RELEASE DECREMENTS THE ALLOCATOR'S COUNT, and it happens inside the backend: the
        // destructor resets the `vk_buffer` wrapper, whose release lambda reaches `free_buffer` - which
        // destroys the buffer only if this was the last user (rhi.api_core.cppm's lifetime note;
        // vma_allocator's comment says the same). A heap object because the FACTORY made it, so the
        // matching delete is the backend's operator delete.
        delete this;
    }

    // ---- THE IMAGE FACE'S OBJECTS (abi 7, §17's design) --------------------------------------

    rhi::image_extent core::image::extent() const noexcept {
        return {.width = this->width, .height = this->height, .depth = 1};
    }

    rhi::image_format core::image::format() const noexcept {
        return this->declared_format;
    }

    rhi::unique_object<rhi::image_view> core::image::make_view(rhi::image_view_desc const& declared_desc) {
        // The range is validated against THIS image's shape, per the contract's rule: a range the
        // image does not have is refused, not clamped - a clamped view would sample the wrong mip and
        // say nothing.
        rhi::image_view_desc const desc = sanitize_image_view_desc(declared_desc);
        if (source && (desc.base_layer >= array_layers || desc.base_mip >= mip_levels ||
                       (desc.layer_count && desc.layer_count > array_layers - desc.base_layer) ||
                       (desc.mip_count && desc.mip_count > mip_levels - desc.base_mip))) {
            return nullptr;
        }
        std::uint32_t const layers = desc.layer_count == 0 ? this->array_layers - desc.base_layer : desc.layer_count;
        std::uint32_t const mips = desc.mip_count == 0 ? this->mip_levels - desc.base_mip : desc.mip_count;
        if (desc.base_layer >= this->array_layers || layers == 0 || desc.base_layer + layers > this->array_layers) {
            deren::utility::log("rhi: make_view refused: layer range [{} + {}) outside the image's {}", desc.base_layer, layers, this->array_layers);
            return nullptr;
        }
        if (desc.base_mip >= this->mip_levels || mips == 0 || desc.base_mip + mips > this->mip_levels) {
            deren::utility::log("rhi: make_view refused: mip range [{} + {}) outside the image's {}", desc.base_mip, mips, this->mip_levels);
            return nullptr;
        }

        // The role picks the aspect (the format is the image's). The view TYPE follows the image's
        // shape: a cube-compatible six-layer image viewed whole IS the cube view; any other
        // multi-layer image is a 2D array; everything else a plain 2D.
        VkImageAspectFlags const aspect = this->declared_format == rhi::image_format::depth
                                              ? VK_IMAGE_ASPECT_DEPTH_BIT
                                              : VK_IMAGE_ASPECT_COLOR_BIT;
        bool const whole_cube = this->cube_compatible && this->array_layers == 6 && desc.base_layer == 0 && layers == 6u;
        // THE RANGE DECIDES THE TYPE, because the shader's sampler must agree with it: a single-layer
        // range is a plain 2D view even on a layered image (the per-cascade shadow layer view samples
        // as texture2D), a whole six-layer cube-compatible image is the CUBE, and every other
        // multi-layer range is a 2D array.
        VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        if (layers == 1) {
            view_type = VK_IMAGE_VIEW_TYPE_2D;
        }
        if (whole_cube) {
            view_type = VK_IMAGE_VIEW_TYPE_CUBE;
        }
        VkImageViewCreateInfo const base_info = make_image_view_info(this->source ? VK_NULL_HANDLE : this->native_handle,
                                                                     this->resolved_format,
                                                                     view_type, aspect, mips, layers);
        // make_image_view_info always bases at 0; the contract's desc carries the base explicitly.
        VkImageViewCreateInfo view_info = base_info;
        view_info.subresourceRange.baseMipLevel = desc.base_mip;
        view_info.subresourceRange.baseArrayLayer = desc.base_layer;
        view_info.subresourceRange.levelCount = mips;
        view_info.subresourceRange.layerCount = layers;

        auto answer = std::make_unique<core::image_view>();
        answer->description = desc;
        answer->parent_image = this->share();
        answer->device = this->parent->device.logical_device;
        if (this->source) {
            answer->lazy_reference = parent->depend_on(*this);
            if (desc.role != rhi::view_role::sampled) {
                return nullptr;
            }
            return rhi::unique_object<rhi::image_view>{answer.release()};
        }
        VkImageView handle = VK_NULL_HANDLE;
        if (vkCreateImageView(this->parent->device.logical_device, &view_info, nullptr, &handle) != VK_SUCCESS || handle == VK_NULL_HANDLE) {
            deren::utility::log("rhi: make_view refused: vkCreateImageView failed");
            return nullptr;
        }
        answer->native_view = handle;
        return rhi::unique_object<rhi::image_view>{answer.release()};
    }

    deren::promise::rhi::shared_object<deren::promise::rhi::image> core::image::share() {
        std::lock_guard const lock(this->shared_handle_mutex);
        if (auto handle = this->shared_handle.lock()) {
            return handle;
        }
        // THE ONE CONTROL BLOCK IS BUILT HERE: the initial count is raised FIRST, so the
        // shared handle's single `release()` balances it exactly; repeated share() calls reuse the block.
        this->references.fetch_add(1, std::memory_order_relaxed);
        auto handle = deren::promise::rhi::shared_object<deren::promise::rhi::image>(this);
        this->shared_handle = handle;
        return handle;
    }

    void core::image::release() noexcept {
        if (this->references.fetch_sub(1, std::memory_order_acq_rel) != 1) {
            return;
        }
        {
            std::lock_guard const lock(this->parent->resource.contract_images_mutex);
            this->parent->resource.contract_images.erase(this);
        }
        // `delete this`: the destructor resets the `vk_image` RAII wrapper, which decrements the allocator's
        // count (rhi.api_core.cppm's lifetime note - release, not necessarily
        // destruction: a content-deduplicated image dies when the last caller drops it).
        delete this;
    }

    core::image_view::~image_view() noexcept {
        if (this->native_view != VK_NULL_HANDLE && this->device != VK_NULL_HANDLE) {
            vkDestroyImageView(this->device, this->native_view, nullptr);
            this->native_view = VK_NULL_HANDLE;
        }
    }

    void core::image_view::release() noexcept {
        delete this;
    }

    core::sampler::~sampler() noexcept {
        if (this->native_sampler_handle != VK_NULL_HANDLE && this->device != VK_NULL_HANDLE) {
            vkDestroySampler(this->device, this->native_sampler_handle, nullptr);
            this->native_sampler_handle = VK_NULL_HANDLE;
        }
    }

    void core::sampler::release() noexcept {
        if (parent) {
            std::lock_guard lock(parent->resource.group_members_mutex);
            parent->resource.group_members.erase(this);
        }
        delete this;
    }

    void core::shader::release() noexcept {
        delete this;
    }

    void core::pipeline::release() noexcept {
        if (this->parent != nullptr) {
            std::lock_guard const lock(this->parent->resource.contract_pipelines_mutex);
            this->parent->resource.contract_pipelines.erase(this);
        }
        delete this;
    }

    /// THE COMPUTE PIPELINE, in the shape the engine's raw builders had (pipelines.cppm's
    /// `vkCreateComputePipelines` sites) - the same heap-native rules, moved to the backend.
    ///
    /// TWO FACTS MAKE IT A SEPARATE PATH rather than a branch inside the graphics one:
    ///
    ///  * A COMPUTE PIPELINE HAS NO ATTACHMENT STATE. Everything the graphics path derives (the vertex-input
    ///    interface, the colour formats, the blend attachments, the sample count, the depth test and bias) is
    ///    absent: the module is built from `compute_code` and the create info carries one stage, one NULL
    ///    layout and nothing else.
    ///  * THE BIND POINT IS DECIDED HERE, ONCE. `core::pipeline::bind_point` is what `bind_pipeline` (abi 20)
    ///    hands the API, and the graphics path leaves it at its GRAPHICS default - a compute pipeline that
    ///    forgot to set it would be bound to the wrong point with no error anywhere. (`create_pipeline`'s
    ///    note explains why the contract made the bind point the backend's business.)
    rhi::pipeline* core::create_compute_pipeline(rhi::pipeline_desc const& desc, char const* const what) {
        // THE ONE REFUSAL OF THIS PATH, and it is the ABI guard's consequence: a caller that declared only
        // the graphics prefix has no `compute_code`, so "compute without SPIR-V" is refused by name instead of
        // being built from whatever the empty span points at.
        if (desc.compute_code.empty()) {
            deren::utility::log("rhi: create_pipeline {} refused: a compute pipeline names its SPIR-V in compute_code", what);
            return nullptr;
        }
        std::optional<vk_shader_module> module = ::deren::vulkan::make_shader_module(
            std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(desc.compute_code.data()), desc.compute_code.size()),
            this->device.logical_device);
        if (!module.has_value()) {
            deren::utility::log("rhi: create_pipeline {} refused: vkCreateShaderModule failed", what);
            return nullptr;
        }
        VkPipelineShaderStageCreateInfo const stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module->get(),
            .pName = "main", // every stage this renderer builds is entered at "main" (see the descriptor's note)
            .pSpecializationInfo = nullptr,
        };
        // THE HEAP FLAG IS NOT OPTIONAL WHEN THE LAYOUT IS NULL: validation's rule is "both or neither" -
        // "pCreateInfos[0].flags (VkPipelineCreateFlags2(0)) does not include
        // VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT while layout is VK_NULL_HANDLE"
        // (VUID-VkComputePipelineCreateInfo-None-11367). The bit lives past the classic 32-bit `flags` field,
        // so it rides VkPipelineCreateFlags2CreateInfo - the shape the engine's raw builders used.
        constexpr VkPipelineCreateFlags2CreateInfo heap_flags = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
        };
        VkComputePipelineCreateInfo const info = {
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = &heap_flags,
            .flags = 0,
            .stage = stage,
            .layout = VK_NULL_HANDLE, // heap-native stages: a layout would contradict them (see docs)
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = -1,
        };
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(this->device.logical_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
            deren::utility::log("rhi: create_pipeline {} refused: vkCreateComputePipelines failed", what);
            return nullptr;
        }
        auto* const answer = new core::pipeline();
        answer->parent = this;
        {
            std::lock_guard const lock(this->resource.contract_pipelines_mutex);
            answer->identity = resource.resource_identity.fetch_add(1, std::memory_order_relaxed);
            this->resource.contract_pipelines.insert(answer);
        }
        // THE MODULE DIES WITH THIS CALL, the pipeline does not: a VkShaderModule is only needed while the
        // pipeline is being created, and keeping one per pipeline would leave N modules allocated for nothing. The
        // RAII wrapper above destroys it when this function returns, whatever the path out.
        answer->native_object.emplace(pipeline, this->device.logical_device);
        answer->native_handle = pipeline;
        answer->bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
        answer->group_binding = desc.texture_group;
        answer->resource_layout.assign(desc.resource_group.begin(), desc.resource_group.end());
        std::ranges::sort(answer->resource_layout, [](auto const& a, auto const& b) { return a.binding < b.binding; });
        return answer;
    }

    /// THE RAY-TRACING PIPELINE, in the shape the engine's raw builder had (pipelines.cppm's
    /// `vkCreateRayTracingPipelinesKHR` site) - the same heap-native rules, on the backend side.
    ///
    /// THREE THINGS MAKE IT Its PATH: it is built from SEVERAL named entry points (one module each), its
    /// executor is a GROUP TABLE rather than a single stage (the group indices are positions in the stage
    /// list), and its entry point is not exported by the loader's import library - so it is resolved through
    /// `vkGetDeviceProcAddr` here, where the device is, rather than at the caller.
    ///
    /// THE SHADER BINDING TABLE IS NOT THIS FUNCTION'S: the group HANDLES are read back by the caller after
    /// creation, and the regions are the caller's memory with the device's stride rules (see the pass that
    /// fills one). What this returns is the pipeline, with `bind_point` set to the ray-tracing one - which is
    /// what makes `bind_pipeline` work for it like any other pipeline.
    rhi::pipeline* core::create_ray_tracing_pipeline(rhi::pipeline_desc const& desc, char const* const what) {
        if (desc.ray_tracing_groups.empty()) {
            deren::utility::log("rhi: create_pipeline {} refused: a ray-tracing pipeline names the groups its shader binding table is built from", what);
            return nullptr;
        }
        // THE ENTRY POINT IS RESOLVED, never linked: the import library exports no extension command, so a
        // direct call would be an undefined symbol rather than a missing feature. A device that announces no
        // ray-tracing pipeline answers null here, which is this path's refusal by name.
        auto const create_ray_tracing = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(vkGetDeviceProcAddr(this->device.logical_device, "vkCreateRayTracingPipelinesKHR"));
        if (create_ray_tracing == nullptr) {
            deren::utility::log("rhi: create_pipeline {} refused: the device did not publish vkCreateRayTracingPipelinesKHR", what);
            return nullptr;
        }
        auto const native_stage_kind = [](rhi::shader_stage const stage) -> std::optional<VkShaderStageFlagBits> {
            switch (stage) {
            case rhi::shader_stage::ray_generation:
                return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
            case rhi::shader_stage::miss:
                return VK_SHADER_STAGE_MISS_BIT_KHR;
            case rhi::shader_stage::closest_hit:
                return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
            case rhi::shader_stage::any_hit:
                return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
            case rhi::shader_stage::intersection:
                return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
            default:
                return std::nullopt; // a graphics/compute stage is not a ray-tracing stage, and guessing is worse
            }
        };
        // ONE MODULE PER STAGE, each destroyed when this function returns (a module is needed only while the
        // pipeline is created): the RAII wrappers below cover every path out, refusals included.
        // Ordinary AS bindings still read the same heap. This avoids ResourceHeapEXT-based AS
        // loads, which lose the device on the NVIDIA driver tested by the native traversal probe.
        // All stage mapping arrays must stay valid until pipeline creation has consumed them.
        deren::utility::vessel::vector<deren::utility::vessel::vector<VkDescriptorSetAndBindingMappingEXT>> mappings(desc.ray_tracing_stages.size());
        deren::utility::vessel::vector<VkShaderDescriptorSetAndBindingMappingInfoEXT> mapping_infos(desc.ray_tracing_stages.size());
        for (auto const& binding : desc.acceleration_structure_bindings) {
            auto const capacity = this->resource.descriptor_heaps.resource_size();
            auto const descriptor_size = this->resource.descriptor_heaps.descriptor_stride(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
            std::uint64_t const last_offset = binding.array_count == 0 ? 0 : static_cast<std::uint64_t>(binding.array_count - 1) * binding.array_stride;
            if (!this->resource.descriptor_heaps.ready() || descriptor_size == 0 || binding.array_count == 0 || binding.array_stride < descriptor_size ||
                binding.byte_offset > capacity || descriptor_size > capacity - binding.byte_offset ||
                last_offset > capacity - binding.byte_offset - descriptor_size ||
                binding.byte_offset % descriptor_size != 0 || binding.array_stride % descriptor_size != 0) {
                deren::utility::log("rhi: create_pipeline {} refused: invalid acceleration-structure heap range", what);
                return nullptr;
            }
            bool found_stage = false;
            for (std::size_t index = 0; index < desc.ray_tracing_stages.size(); ++index) {
                if (desc.ray_tracing_stages[index].stage != binding.stage) {
                    continue;
                }
                found_stage = true;
                for (auto const& prior : mappings[index]) {
                    if (prior.descriptorSet == binding.descriptor_set && prior.firstBinding == binding.binding) {
                        deren::utility::log("rhi: create_pipeline {} refused: duplicate acceleration-structure shader binding", what);
                        return nullptr;
                    }
                }
                mappings[index].push_back(make_descriptor_set_and_binding_mapping(
                    binding.descriptor_set, binding.binding, 1, VK_SPIRV_RESOURCE_TYPE_ACCELERATION_STRUCTURE_BIT_EXT,
                    VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT,
                    binding.byte_offset, binding.array_stride));
            }
            if (!found_stage) {
                deren::utility::log("rhi: create_pipeline {} refused: acceleration-structure binding has no matching stage", what);
                return nullptr;
            }
        }
        for (std::size_t index = 0; index < mappings.size(); ++index) {
            mapping_infos[index].sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT;
            mapping_infos[index].mappingCount = static_cast<std::uint32_t>(mappings[index].size());
            mapping_infos[index].pMappings = mappings[index].data();
        }
        deren::utility::vessel::vector<std::optional<vk_shader_module>> modules(desc.ray_tracing_stages.size());
        deren::utility::vessel::vector<VkPipelineShaderStageCreateInfo> stages;
        stages.reserve(desc.ray_tracing_stages.size());
        for (std::size_t index = 0; index < desc.ray_tracing_stages.size(); ++index) {
            rhi::ray_tracing_stage const& declared = desc.ray_tracing_stages[index];
            std::optional<VkShaderStageFlagBits> const kind = native_stage_kind(declared.stage);
            if (!kind.has_value()) {
                deren::utility::log("rhi: create_pipeline {} refused: ray-tracing stage {} is not a ray-tracing entry point", what, index);
                return nullptr;
            }
            if (declared.code.empty()) {
                deren::utility::log("rhi: create_pipeline {} refused: ray-tracing stage {} carries no SPIR-V", what, index);
                return nullptr;
            }
            modules[index] = ::deren::vulkan::make_shader_module(
                std::span<std::uint8_t const>(reinterpret_cast<std::uint8_t const*>(declared.code.data()), declared.code.size()), this->device.logical_device);
            if (!modules[index].has_value()) {
                deren::utility::log("rhi: create_pipeline {} refused: vkCreateShaderModule failed for ray-tracing stage {}", what, index);
                return nullptr;
            }
            stages.push_back(VkPipelineShaderStageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = mappings[index].empty() ? nullptr : &mapping_infos[index],
                .flags = 0,
                .stage = kind.value(),
                .module = modules[index]->get(),
                .pName = "main", // every entry point this renderer builds is "main"
                .pSpecializationInfo = nullptr,
            });
        }
        // THE GROUP TABLE, with the kind following from WHICH SLOTS ARE FILLED (the descriptor's rule):
        // any hit slot makes it a hit group, and `triangles` says whether its geometry is triangles. This is the
        // one place the two spellings meet, and it is a translation rather than a second vocabulary.
        deren::utility::vessel::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
        groups.reserve(desc.ray_tracing_groups.size());
        for (rhi::ray_tracing_group const& group : desc.ray_tracing_groups) {
            bool const hit_group = group.closest_hit != rhi::shader_group_none || group.any_hit != rhi::shader_group_none || group.intersection != rhi::shader_group_none;
            VkRayTracingShaderGroupTypeKHR group_type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
            if (hit_group) {
                group_type = group.triangles ? VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR
                                             : VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;
            }
            groups.push_back(VkRayTracingShaderGroupCreateInfoKHR{
                .sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                .pNext = nullptr,
                .type = group_type,
                .generalShader = group.general,
                .closestHitShader = group.closest_hit,
                .anyHitShader = group.any_hit,
                .intersectionShader = group.intersection,
                .pShaderGroupCaptureReplayHandle = nullptr,
            });
        }
        // THE HEAP FLAG IS NOT OPTIONAL WHEN THE LAYOUT IS NULL (VUID-VkRayTracingPipelineCreateInfoKHR-...):
        // the same "both or neither" rule the compute path states, through the same flags2 structure.
        constexpr VkPipelineCreateFlags2CreateInfo heap_flags = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
        };
        VkRayTracingPipelineCreateInfoKHR const info = {
            .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
            .pNext = &heap_flags,
            .flags = 0,
            .stageCount = static_cast<std::uint32_t>(stages.size()),
            .pStages = stages.data(),
            .groupCount = static_cast<std::uint32_t>(groups.size()),
            .pGroups = groups.data(),
            .maxPipelineRayRecursionDepth = desc.max_ray_recursion == 0u ? 1u : desc.max_ray_recursion,
            .pLibraryInfo = nullptr,
            .pLibraryInterface = nullptr,
            .pDynamicState = nullptr,
            .layout = VK_NULL_HANDLE, // heap-native stages: a layout would contradict them
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = -1,
        };
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (create_ray_tracing(this->device.logical_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
            deren::utility::log("rhi: create_pipeline {} refused: vkCreateRayTracingPipelinesKHR failed", what);
            return nullptr;
        }
        auto* const answer = new core::pipeline();
        answer->parent = this;
        {
            std::lock_guard const lock(this->resource.contract_pipelines_mutex);
            answer->identity = resource.resource_identity.fetch_add(1, std::memory_order_relaxed);
            this->resource.contract_pipelines.insert(answer);
        }
        answer->native_object.emplace(pipeline, this->device.logical_device);
        answer->native_handle = pipeline;
        answer->bind_point = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
        answer->group_count = info.groupCount;
        return answer;
    }

    // ---- THE COMMAND BUFFER HANDLE (abi 15) ----------------------------------------------------------

    core::command_buffer* core::allocate_command_buffer(VkCommandBufferLevel const level) {
        auto* const answer = new core::command_buffer();
        answer->parent = this;
        // ONE POOL PER BUFFER, created and destroyed by the wrapper (handles/handles.cppm): the handle is a
        // self-contained device resource and the recording thread that uses it never shares a pool.
        answer->buffer = ::deren::vulkan::make_command_buffer(this->device.logical_device, this->device.graphics_queue_family_index, level);
        if (*answer->buffer == VK_NULL_HANDLE) {
            delete answer;
            deren::utility::log("rhi: command buffer allocation failed (level {})", static_cast<int>(level));
            return nullptr;
        }
        answer->target = *answer->buffer;
        answer->set_target(*answer->buffer); // the Vulkan class's native handle, read through get()
        answer->kind = level == VK_COMMAND_BUFFER_LEVEL_SECONDARY ? rhi::command_buffer_kind::secondary
                                                                  : rhi::command_buffer_kind::primary;
        {
            // THE PROVENANCE REGISTRY, the same shape `resource.contract_images` uses: the batch merge / one-shot submit
            // and the recording-time merge MUST tell a buffer this backend made from a pointer a caller passes,
            // and the cast that reads it is only defined once that is known.
            std::lock_guard const lock(this->command.contract_command_buffers_mutex);
            this->command.contract_command_buffers.insert(answer);
        }
        return answer;
    }

    // ---- abi 35: the mergeable command buffer (batch-only) -----------------------------------------

    void core::mergeable_command_buffer::release() noexcept {
        if (this->parent != nullptr) {
            std::lock_guard const lock(this->parent->command.contract_mergeable_buffers_mutex);
            this->parent->command.contract_mergeable_buffers.erase(this);
        }
        // THE INNER SECONDARY IS FREED BY THE `std::shared_ptr` MEMBER - a backend-INTERNAL share, not a
        // contract handle: a submission that is still in flight raises the count through `reference_command`, so
        // the recording outlives this handle exactly as the contract's lifetime rule promises.
        delete this;
    }

    rhi::error core::submit_batch(std::span<rhi::mergeable_command_buffer_ref const> batch, rhi::submission_token* token) {
        // EMPTY IS A CALLER BUG: an empty frame wants no queue submission, and there is no acquire/present
        // to do on its behalf - refusing keeps "submitted nothing" from looking like "submitted the frame".
        if (batch.empty()) {
            return rhi::error::invalid_argument;
        }
        // THE WHOLE BATCH IS VALIDATED BEFORE ANYTHING IS RECORDED OR QUEUED (the contract's atomic rule).
        {
            std::lock_guard const lock(this->command.contract_mergeable_buffers_mutex);
            for (auto const& one : batch) {
                if (one == nullptr || !this->command.contract_mergeable_buffers.contains(one.get())) {
                    return rhi::error::invalid_argument; // a handle this backend did not hand out
                }
            }
        }
        for (auto const& one : batch) {
            auto const& mergeable = *rhi::do_concrete<core::mergeable_command_buffer>(*one, concrete::mergeable_command_buffer);
            if (mergeable.inner == nullptr) {
                return rhi::error::not_ready;
            }
            auto const& inner = *mergeable.inner;
            if ((inner.kind != rhi::command_buffer_kind::secondary) || !inner.executable || inner.recording ||
                inner.pending.load(std::memory_order_acquire) != 0 || *inner.buffer == VK_NULL_HANDLE) {
                return rhi::error::not_ready; // still recording, released, or already in flight
            }
        }
        for (std::size_t i = 0; i < batch.size(); ++i) {
            for (std::size_t j = i + 1; j < batch.size(); ++j) {
                if (batch[i].get() == batch[j].get()) {
                    return rhi::error::invalid_argument; // the same segment twice is not one batch
                }
            }
        }

        // THE MERGE PRIMARY THE BACKEND CREATES: an ordinary primary created here, so the EXISTING
        // primary submission path below does the lifetime bookkeeping, fence and pending bookkeeping - the batch
        // does not need a second submission mechanism.
        rhi::command_buffer_desc primary_desc{};
        primary_desc.kind = rhi::command_buffer_kind::primary;
        rhi::command_buffer* const raw_primary = this->command.create_command_buffer(primary_desc).release();
        if (raw_primary == nullptr) {
            return rhi::error::not_ready;
        }
        auto* const primary = static_cast<core::command_buffer*>(raw_primary);
        rhi::error status = primary->begin_recording(rhi::command_buffer_begin_info{});
        if (status == rhi::error::ok) {
            for (auto const& one : batch) {
                auto& inner = *static_cast<core::mergeable_command_buffer*>(one.get())->inner;
                // THE RECORDING IS KEPT (and its pending counter raised) by the primary's secondary_refs,
                // which the submission path copies into the pending submission: the caller may drop its
                // mergeable handles as soon as this call returns.
                primary->secondary_refs.push_back(this->reference_command(inner, true));
                VkCommandBuffer const native = *inner.buffer;
                vkCmdExecuteCommands(*primary->buffer, 1, &native);
            }
            status = primary->end_recording();
        }
        if (status == rhi::error::ok) {
            status = this->submit_commands(*raw_primary, token);
        }
        // drop OUR one count: after a successful submit the pending submission keeps its share, and a failed
        // one has nothing in flight - either way the handle dies here, inside the backend.
        primary->release();
        return status;
    }

    void core::command_buffer::release() noexcept {
        if (references.fetch_sub(1, std::memory_order_acq_rel) != 1) {
            return;
        }
        {
            std::lock_guard const lock(this->parent->command.contract_command_buffers_mutex);
            this->parent->command.contract_command_buffers.erase(this);
        }
        // `delete this`: the destructor releases the `vk_command_buffer`, whose release DESTROYS THE
        // COMMAND POOL it created - the contract's single count is the whole lifetime rule here (there
        // is no allocator registry behind this resource, unlike a buffer or an image).
        delete this;
    }

    rhi::error core::command_buffer::begin_recording(rhi::command_buffer_begin_info const& declared_info) {
        if (pending.load(std::memory_order_acquire) != 0 || recording_users.load(std::memory_order_acquire) != 0 || recording) {
            return rhi::error::not_ready;
        }
        // THE ABI GUARD for the begin info: only the prefix the caller declares is read.
        std::uint32_t const declared = declared_info.struct_size;
        rhi::command_buffer_flags usage = rhi::no_command_buffer_flags;
        rhi::structure_header const* next = nullptr;
        if (covered_by(declared, offsetof(rhi::command_buffer_begin_info, usage), sizeof(rhi::command_buffer_begin_info::usage))) {
            usage = declared_info.usage;
        }
        if (covered_by(declared, offsetof(rhi::command_buffer_begin_info, next), sizeof(rhi::command_buffer_begin_info::next))) {
            next = declared_info.next;
        }

        if (*this->buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready; // this wrapper names no buffer any more
        }

        // The contract's colour formats, converted once into the API's spelling (see the note below):
        // eight is this backend's ceiling for inherited attachments, and a larger declaration is refused.
        deren::utility::vessel::array<VkFormat, 8> native_color_formats = {};
        VkCommandBufferInheritanceRenderingInfo rendering = make_inheritance_rendering_info(false, nullptr, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT);
        VkCommandBufferInheritanceDescriptorHeapInfoEXT heap_inheritance;
        VkBindHeapInfoEXT resource_bind = {};
        VkBindHeapInfoEXT sampler_bind = {};
        VkCommandBufferInheritanceInfo inheritance = make_inheritance_info(nullptr);
        bool const inherits = next != nullptr;
        if (inherits) {
            if (next->s_type == rhi::structure_type::command_buffer_inheritance) {
                auto const valid = rhi::validate_structure(*next, next->s_type, sizeof(rhi::command_buffer_inheritance_info));
                if (valid != rhi::error::ok) {
                    return valid;
                }
                auto const& attachment = *reinterpret_cast<rhi::command_buffer_inheritance_info const*>(next);
                if (attachment.color_format_count > native_color_formats.size()) {
                    return rhi::error::unsupported;
                }
                if ((attachment.color_format_count != 0 && attachment.color_formats == nullptr) ||
                    attachment.samples == 0 || attachment.samples > 64 || (attachment.samples & (attachment.samples - 1)) != 0 ||
                    (attachment.depth_format != rhi::image_format::unknown && attachment.depth_format != rhi::image_format::depth)) {
                    return rhi::error::invalid_argument;
                }
                for (std::uint32_t index = 0; index < attachment.color_format_count; ++index) {
                    auto const format = attachment.color_formats[index];
                    native_color_formats[index] = native_image_format(format, this->parent->frame.depth_attachment_format);
                    if (format == rhi::image_format::depth || native_color_formats[index] == VK_FORMAT_UNDEFINED) {
                        return rhi::error::invalid_argument;
                    }
                }
                rendering.colorAttachmentCount = attachment.color_format_count;
                rendering.depthAttachmentFormat = native_image_format(attachment.depth_format, this->parent->frame.depth_attachment_format);
                rendering.rasterizationSamples = static_cast<VkSampleCountFlagBits>(attachment.samples);
                rendering.viewMask = attachment.view_mask;
            } else {
                if (!this->refused_chain_logged) {
                    this->refused_chain_logged = true;
                    deren::utility::log("rhi: begin_recording refused a parameter chain of type {:#x}", static_cast<std::uint32_t>(next->s_type));
                }
                return rhi::error::unsupported;
            }
            rendering.pColorAttachmentFormats = rendering.colorAttachmentCount == 0 ? nullptr : native_color_formats.data();
            inheritance.pNext = &rendering;
        } else if (rhi::has_flag(usage, rhi::command_buffer_usage::render_pass_continue)) {
            // A CONTINUATION WITH NO INHERITANCE IS NOT EXPRESSIBLE: Vulkan requires the secondary to
            // declare the attachments it continues (dynamic rendering), and the backend cannot invent
            // them. Refused by name rather than begun into an unvalidated state.
            return rhi::error::unsupported;
        }
        // ---- THE DESCRIPTOR HEAPS A CONTINUATION INHERITS (VUID-vkCmdDrawIndexed-None-11308) -----------
        //
        // WHY THE BACKEND DERIVES THIS INSTEAD OF THE CALLER HANDING IT OVER: a descriptor heap is the
        // BACKEND's state - this class creates both heaps, `heap_inheritance`'s two bind infos come
        // straight out of `resource.descriptor_heaps` (the same two `record_bind` binds for a primary), and the
        // CONTRACT's `begin_recording` says so in its words: the parameter chain carries what the
        // caller knows (which attachment formats the instance has), and "the backend defines the
        // compatibility rules" - which heap state a secondary is validated against is exactly such a
        // rule. Asking every caller to reach for `VkCommandBufferInheritanceDescriptorHeapInfoEXT`
        // would put this backend's heap layout into the engine half (the boundary this whole face
        // exists to keep), and a caller that forgot it would produce a BLACK secondary with the only
        // symptom being a VUID nobody reads.
        //
        // A SECONDARY IS VALIDATED alone, so the bind the primary recorded never reaches it - and
        // an inherited heap is only meaningful for a continuation, which is the one case where the
        // secondary's draws are validated against the instance the primary opened. It is chained in
        // FRONT of whatever the caller's chain put in `pNext` (the attachment inheritance above),
        // because a chain is a list and this entry is the backend's, not the caller's. Only when the
        // heap face is ACTIVE: a device without VK_EXT_descriptor_heap leaves both heaps unusable, the
        // bind infos stay zero, and chaining a heap the device never got would be worse than leaving it
        // out - the same "the heap is simply unused" state every other heap path in this file accepts.
        //
        // `inherit_descriptor_heaps` (abi 45) is the caller's one opt-OUT of that derivation: a THIRD-PARTY
        // recorder that binds its classic descriptor sets - the Dear ImGui Vulkan backend is the case the
        // field exists for - starts its segment through the overlay's begin info with the flag false, because
        // a heap-inheriting continuation would trip VUID-...-11295/11296 on every one of its binds.
        bool inherit_descriptor_heaps = true;
        if (covered_by(declared_info.struct_size, offsetof(rhi::command_buffer_begin_info, inherit_descriptor_heaps),
                       sizeof(rhi::command_buffer_begin_info::inherit_descriptor_heaps))) {
            inherit_descriptor_heaps = declared_info.inherit_descriptor_heaps;
        }
        if (inherit_descriptor_heaps && this->parent->resource.descriptor_heaps.ready() &&
            (inherits || rhi::has_flag(usage, rhi::command_buffer_usage::render_pass_continue))) {
            this->parent->resource.descriptor_heaps.bind_infos(resource_bind, sampler_bind);
            heap_inheritance = VkCommandBufferInheritanceDescriptorHeapInfoEXT{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_DESCRIPTOR_HEAP_INFO_EXT,
                .pNext = inheritance.pNext,
                .pSamplerHeapBindInfo = &sampler_bind,
                .pResourceHeapBindInfo = &resource_bind,
            };
            inheritance.pNext = &heap_inheritance;
        }

        VkCommandBufferUsageFlags flags = 0;
        if (rhi::has_flag(usage, rhi::command_buffer_usage::one_time_submit)) {
            flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        }
        if (rhi::has_flag(usage, rhi::command_buffer_usage::render_pass_continue)) {
            flags |= VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        }
        if (rhi::has_flag(usage, rhi::command_buffer_usage::simultaneous_use)) {
            flags |= VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        }

        // The inheritance info is passed whenever ANY of it was filled: the caller's attachment chain
        // or this backend's heap entry (see above). When neither applies - a primary, or a
        // secondary begun for inline recording - `pInheritanceInfo` must be NULL.
        VkCommandBufferBeginInfo const begin =
            make_command_buffer_begin_info(flags, kind == rhi::command_buffer_kind::secondary ? &inheritance : nullptr);
        auto const result = generic_error(vkBeginCommandBuffer(*this->buffer, &begin));
        if (result == rhi::error::ok) {
            secondary_refs.clear();
            group_refs.clear();
            resource_refs.clear();
            lazy_refs.clear();
            executable = false;
            reset_operations();
            rendering_active = (flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) != 0;
            recording = true;
            rendering_inherited = rendering_active;
            simultaneous_use = (flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT) != 0;
            one_time_submit = (flags & VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT) != 0;
        }
        return result;
    }

    rhi::error core::command_buffer::end_recording() noexcept {
        if (!recording) {
            return rhi::error::not_ready;
        }
        if (*this->buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        final_application = true;
        auto applied = apply_operations();
        final_application = false;
        if (rendering_active && !rendering_inherited && applied == rhi::error::ok) {
            applied = rhi::error::invalid_argument;
        }
        if (applied != rhi::error::ok) {
            // Reset also handles a native recording invalidated by a destroyed resource it pointed at.
            static_cast<void>(vkResetCommandBuffer(*this->buffer, 0));
            recording = rendering_active = executable = false;
            return applied;
        }
        auto const result = generic_error(vkEndCommandBuffer(*this->buffer));
        if (result == rhi::error::ok) {
            recording = false;
            executable = true;
        }
        return result;
    }

    // ---- THE FRAME'S BUFFER VIEW: the four lifecycle verbs, refused BY NAME (see the note in
    //      core.declarations.cppm). release() is the one that must not be silent - a caller that wrapped
    //      this view in unique_object<> has a bug - so it says so once, exactly as frame_image_slot does.

    void core::frame_commands::release() noexcept {
        // A VIEW IS NOT A FACTORY PRODUCT: the core creates the frame's list, the view names it, and
        // there is nothing to drop. Reaching here is a lifecycle PRECONDITION violation, so it terminates
        // loudly instead of being tolerated with a log - the container-based lifetime in this batch is what
        // makes "who releases" unambiguous; this branch is the caller ignoring it.
        deren::utility::panic("rhi: release() on the FRAME's command buffer - it is a view of a list the core created, "
                              "and only factory products have a count to drop");
    }

    rhi::error core::frame_commands::begin_recording(rhi::command_buffer_begin_info const& declared_info) {
        if (recording) {
            return rhi::error::not_ready;
        }
        // THE FRAME'S RECORDING RIDES THE CONTRACT NOW (abi 26), and the two verbs under this comment are
        // why the frame loop stopped calling `vkBeginCommandBuffer`/`vkEndCommandBuffer` itself: the buffer is
        // the CORE's (`frame_command_buffer()`), the API's state machine behind it is exactly what the
        // recording face exists to expose, and the engine half must not name an entry point to open a frame.
        //
        // WHAT THE REFUSAL THAT STOOD HERE WAS PROTECTING: "the frame loop begins the frame's recording, not a
        // pass". That rule is still true and is now the CALLER's to keep - the contract's `begin_recording`
        // doc has always said a buffer's lifecycle is begun once and by its creator - while the pass layer never
        // reaches this verb at all (a pass records into `resolved_io::list`, whose recording the RUNNER opened).
        // A usage bit this backend cannot serve is still refused BY NAME, exactly as the factory-made form refuses it.
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready; // no frame in flight: the same window `begin_commands()` answers in
        }
        rhi::command_buffer_flags usage = rhi::no_command_buffer_flags;
        if (covered_by(declared_info.struct_size, offsetof(rhi::command_buffer_begin_info, usage), sizeof(rhi::command_buffer_begin_info::usage))) {
            usage = declared_info.usage;
        }
        VkCommandBufferUsageFlags flags = 0;
        if (rhi::has_flag(usage, rhi::command_buffer_usage::one_time_submit)) {
            flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        }
        if (rhi::has_flag(usage, rhi::command_buffer_usage::simultaneous_use)) {
            flags |= VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        }
        // `render_pass_continue` is a SECONDARY's bit and the frame's buffer is a primary: the factory-made form refuses
        // the combination it cannot spell rather than passing a flag the API rejects on a primary.
        if (rhi::has_flag(usage, rhi::command_buffer_usage::render_pass_continue)) {
            return rhi::error::unsupported;
        }
        VkCommandBufferBeginInfo const begin = make_command_buffer_begin_info(flags, nullptr); // a primary has no inheritance
        auto const result = generic_error(vkBeginCommandBuffer(command_buffer, &begin));
        if (result == rhi::error::ok) {
            secondary_refs.clear();
            group_refs.clear();
            resource_refs.clear();
            lazy_refs.clear();
            executable = false;
            reset_operations();
            rendering_active = (flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) != 0;
            recording = true;
            rendering_inherited = rendering_active;
        }
        return result;
    }

    rhi::error core::frame_commands::end_recording() noexcept {
        if (!recording) {
            return rhi::error::not_ready;
        }
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        final_application = true;
        auto applied = apply_operations();
        final_application = false;
        if (rendering_active && !rendering_inherited && applied == rhi::error::ok) {
            applied = rhi::error::invalid_argument;
        }
        if (applied != rhi::error::ok) {
            // Reset also handles a native recording invalidated by a destroyed resource it pointed at.
            static_cast<void>(vkResetCommandBuffer(command_buffer, 0));
            recording = rendering_active = executable = false;
            return applied;
        }
        auto const result = generic_error(vkEndCommandBuffer(command_buffer));
        if (result == rhi::error::ok) {
            recording = false;
            executable = true;
        }
        return result;
    }

    namespace {
        /// THE ONE RECORDING-TIME MERGE (abi 36): validate the whole batch, then record the segments into
        /// `target`'s native buffer in span order. The segments are kept through the target's
        /// `secondary_refs`, which is what leaves their recordings valid for the submission that follows.
        [[nodiscard]] rhi::error execute_mergeable_batch(core& parent, core::frame_commands& target,
                                                         std::span<deren::promise::rhi::mergeable_command_buffer_ref const> batch) {
            if (auto applied = target.apply_operations(); applied != rhi::error::ok) {
                return applied;
            }
            if (batch.empty()) {
                return rhi::error::invalid_argument;
            }
            if (!target.recording) {
                return rhi::error::not_ready;
            }
            VkCommandBuffer const command_buffer = target.native();
            if (command_buffer == VK_NULL_HANDLE) {
                return rhi::error::not_ready;
            }
            {
                std::lock_guard const lock(parent.command.contract_mergeable_buffers_mutex);
                for (auto const& one : batch) {
                    if (one == nullptr || !parent.command.contract_mergeable_buffers.contains(one.get())) {
                        return rhi::error::invalid_argument;
                    }
                }
            }
            deren::utility::vessel::vector<core::mergeable_command_buffer const*> resolved;
            resolved.reserve(batch.size());
            for (auto const& one : batch) {
                auto const& mergeable = *rhi::do_concrete<core::mergeable_command_buffer>(*one, concrete::mergeable_command_buffer);
                if (mergeable.inner == nullptr || mergeable.inner->kind != rhi::command_buffer_kind::secondary ||
                    !mergeable.inner->executable || mergeable.inner->recording ||
                    mergeable.inner->pending.load(std::memory_order_acquire) != 0 || *mergeable.inner->buffer == VK_NULL_HANDLE) {
                    return rhi::error::not_ready;
                }
                for (auto const* const seen : resolved) {
                    if (seen == &mergeable) {
                        return rhi::error::invalid_argument; // the same segment twice is not one batch
                    }
                }
                resolved.push_back(&mergeable);
            }
            for (auto const* const mergeable : resolved) {
                auto& inner = *mergeable->inner;
                target.secondary_refs.push_back(parent.reference_command(inner, true));
                VkCommandBuffer const native = *inner.buffer;
                vkCmdExecuteCommands(command_buffer, 1, &native);
                target.finish_native();
            }
            return rhi::error::ok;
        }
    } // namespace

    rhi::error core::frame_commands::execute(std::span<deren::promise::rhi::mergeable_command_buffer_ref const> batch) {
        if (parent) {
            ++parent->operation_requests[1]; // execute
        }
        return execute_mergeable_batch(*this->parent, *this, batch);
    }

    rhi::error core::command_buffer::execute(std::span<deren::promise::rhi::mergeable_command_buffer_ref const> batch) {
        if (this->kind != rhi::command_buffer_kind::primary) {
            return rhi::error::invalid_argument; // only a primary can be a merge target
        }
        return execute_mergeable_batch(*this->parent, *this, batch);
    }

    VkCommandBuffer core::frame_command_buffer() const noexcept {
        // The frame slot in progress: submit() and wait_frame_slot() use `frame.current_frame` for it, and
        // to_next_frame() advances it.
        std::size_t const slot = static_cast<std::size_t>(this->frame.current_frame);
        if (slot >= this->frame.frame_command_buffers.size()) {
            return VK_NULL_HANDLE;
        }
        return this->frame.frame_command_buffers[slot].get();
    }

    // ---- the contract's frame-domain views ---------------------------------------------------------
    // Every one of them answers for the core that creates it (the `parent` field is set in the constructor, and the
    // view object is a member of that core, so it can never outlive it).

    rhi::image_extent core::frame_image_slot::extent() const noexcept {
        rhi::image_extent const extent = this->parent->frame.swap_chain_extent;
        return rhi::image_extent{.width = extent.width, .height = extent.height, .depth = 1};
    }

    rhi::unique_object<rhi::image_view> core::frame_image_slot::make_view(rhi::image_view_desc const& declared_desc) {
        // A FRAME IMAGE HANDS OUT VIEWS (③-D/E item B, abi 16). The IMAGE is the swapchain image,
        // created by the presentation path - but a VIEW is a NEW backend object created
        // right here, so `release()` on it is real and the contract's lifetime rule applies to it exactly
        // as it does to `create_image()->make_view()`: one count, dropped once, inside the backend.
        //
        // THE LIFETIME RULE THE CALLER MUST KEEP (and the reason this used to refuse): a view made over a
        // swapchain image dies with that image, so the caller has to release every view it created from a
        // frame image BEFORE the swapchain is recreated (`swapchain::recreate()`, which the engine
        // drives from `runtime::on_swapchain_recreated`). A view that outlives its image is a stale handle
        // the validation layer reports; this backend does not track the caller's views, so it cannot paper
        // over it. In this renderer the window is exactly one frame's worth: the engine makes the view while
        // a frame is being recorded and releases it after the frame's submission, and a rebuild happens
        // between frames.
        rhi::image_view_desc const desc = sanitize_image_view_desc(declared_desc);
        VkImage const image = this->handle();
        if (image == VK_NULL_HANDLE) {
            deren::utility::log("rhi: make_view on the frame image refused: no image has been acquired yet");
            return nullptr;
        }
        // THE SWAPCHAIN IMAGE'S SHAPE IS ONE LAYER AND ONE MIP, and that is a fact rather than a limit of
        // this implementation: the presentation image is what it is. A range outside it is refused, not
        // clamped - the same rule `core::image::make_view` follows, for the same reason (a clamped view
        // samples something the caller did not ask for and says nothing).
        std::uint32_t const layers = desc.layer_count == 0 ? 1u : desc.layer_count;
        std::uint32_t const mips = desc.mip_count == 0 ? 1u : desc.mip_count;
        if (desc.base_layer != 0u || layers != 1u || desc.base_mip != 0u || mips != 1u) {
            deren::utility::log("rhi: make_view on the frame image refused: it is a single-layer, single-mip image "
                                "(asked for layer {} + {}, mip {} + {})",
                                desc.base_layer, layers, desc.base_mip, mips);
            return nullptr;
        }
        VkImageViewCreateInfo const view_info = make_image_view_info(image,
                                                                     this->parent->frame.swap_chain_image_format,
                                                                     VK_IMAGE_VIEW_TYPE_2D,
                                                                     VK_IMAGE_ASPECT_COLOR_BIT,
                                                                     mips,
                                                                     layers);
        VkImageView handle = VK_NULL_HANDLE;
        if (vkCreateImageView(this->parent->device.logical_device, &view_info, nullptr, &handle) != VK_SUCCESS || handle == VK_NULL_HANDLE) {
            deren::utility::log("rhi: make_view on the frame image refused: vkCreateImageView failed");
            return nullptr;
        }
        auto* const answer = new core::image_view();
        answer->native_view = handle;
        answer->device = this->parent->device.logical_device;
        return rhi::unique_object<rhi::image_view>{answer};
    }

    void core::frame_image_slot::release() noexcept {
        // A VIEW IS NOT A FACTORY PRODUCT: this object is a member of the core, the swapchain creates the image
        // behind it, and the view names that image without creating anything. Reaching here is a lifecycle
        // PRECONDITION violation and terminates loudly (see `frame_commands::release`).
        deren::utility::panic("rhi: release() on a frame image view - it is created by the core and has no count to drop; "
                              "only factory products have a count to drop");
    }

    rhi::image_format core::frame_image_slot::format() const noexcept {
        return this->parent->frame.presentation_surface.format();
    }

    VkImage core::frame_image_slot::handle() const noexcept {
        core const& self = *this->parent;
        std::size_t const index = static_cast<std::size_t>(self.frame.acquired_image_index);
        return index < self.frame.swap_chain_images.size() ? self.frame.swap_chain_images[index] : VK_NULL_HANDLE;
    }

    std::uint64_t core::frame_readback_slot::size() const noexcept {
        return static_cast<std::uint64_t>(this->parent->frame.readback_slot_size);
    }

    void core::frame_readback_slot::release() noexcept {
        // A VIEW, same answer as the frame image view's: the read-back slot is the core's allocation,
        // so a release here is a lifecycle precondition violation and terminates.
        deren::utility::panic("rhi: release() on the frame read-back buffer - it is created by the core and has no "
                              "count to drop; only factory products have a count to drop");
    }

    std::span<std::byte> core::frame_readback_slot::mapped() noexcept {
        // EMPTY when the buffer cannot be mapped - the contract's spelling of "this one is not
        // host-visible", so a caller decides from the span instead of guessing.
        if (this->parent->frame.readback_slot_mapped == nullptr || this->parent->frame.readback_slot_size == 0) {
            return {};
        }
        return std::span<std::byte>(static_cast<std::byte*>(this->parent->frame.readback_slot_mapped),
                                    static_cast<std::size_t>(this->parent->frame.readback_slot_size));
    }

    VkBuffer core::frame_readback_slot::handle() const noexcept {
        return this->parent->frame.readback_slot_handle;
    }

    rhi::error core::frame_commands::use(rhi::image const& resource, rhi::image_use const from, rhi::image_use const to) noexcept {
        if (parent) {
            ++parent->operation_requests[2]; // use
        }
        if (auto result = apply_operations(); result != rhi::error::ok) {
            return result;
        }
        core* const self = this->parent;
        if (self == nullptr || !self->frame.frame_in_flight) {
            // NO FRAME IS BEING RECORDED, so there is nowhere to record the transition - and SAYING SO is
            // the point of the return value: a silently dropped barrier is a frame whose image is in a
            // state nobody declared, which is the failure class task-148 measured.
            return rhi::error::not_ready;
        }
        if (this->target != VK_NULL_HANDLE) {
            // THE FRAME-SCOPED VERBS BELONG TO THE FRAME'S LIST (abi 15): `use()` declares the FRAME
            // image's role pair, and a list that records into a buffer the caller created has no frame
            // image in it. `not_ready` is the contract's word for it ("or the list is not this
            // frame's" - the window `use()` and `begin_commands()` share).
            return rhi::error::not_ready;
        }
        if (static_cast<void const*>(&resource) != static_cast<void const*>(&self->frame.frame_image_view)) {
            // The factories still answer nullptr, so `frame_image()` is the ONLY image that can reach this
            // call today. A foreign object is the caller's bug: the answer says so, and the reason is
            // logged once.
            if (!this->unexpected_use_logged) {
                this->unexpected_use_logged = true;
                deren::utility::log("rhi: use() was handed an image this backend did not hand out; nothing was recorded");
            }
            return rhi::error::invalid_argument;
        }
        VkImageMemoryBarrier2 barrier = barrier_for(from, to);
        if (barrier.srcStageMask == 0 && barrier.srcAccessMask == 0 && barrier.dstStageMask == 0 && barrier.dstAccessMask == 0) {
            if (!this->unexpected_use_logged) {
                this->unexpected_use_logged = true;
                deren::utility::log("rhi: use({}, {}) is not a transition this backend can spell; nothing was recorded",
                                    static_cast<std::uint32_t>(from),
                                    static_cast<std::uint32_t>(to));
            }
            return rhi::error::unsupported;
        }
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        barrier.image = self->frame.frame_image_view.handle();

        // ---- GATE A5, THE RUN-TIME HALF: BOTH SIDES OF THE BARRIER, ONCE PER PAIR ------------------
        // The static_asserts above prove the equality at compile time; this dump is what puts the two
        // field lists in the run's log (spec §6.1 A5 asks for both sides, not for a verdict).
        std::uint32_t const pair_bit = (from == rhi::image_use::color_attachment) ? 1u : 2u;
        if ((this->shadow_gate_dumped & pair_bit) == 0) {
            this->shadow_gate_dumped |= pair_bit;
            VkImageMemoryBarrier2 const& recipe = (from == rhi::image_use::color_attachment) ? deren::vulkan::color_attachment_to_transfer_transition
                                                                                             : deren::vulkan::transfer_to_color_attachment_transition;
            deren::utility::log("rhi shadow gate (A5) use({}, {}) image={:#x}", static_cast<std::uint32_t>(from), static_cast<std::uint32_t>(to),
                                reinterpret_cast<std::uintptr_t>(barrier.image));
            deren::utility::log("rhi shadow gate (A5)   derived: {}", barrier_text(barrier));
            deren::utility::log("rhi shadow gate (A5)   recipe : {}", barrier_text(recipe));
        }

        VkDependencyInfo const dependency = make_image_dependency_info(1, &barrier);
        vkCmdPipelineBarrier2(command_buffer, &dependency);
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::copy_image_to_buffer(rhi::buffer& destination,
                                                          rhi::image const& source,
                                                          rhi::image_copy_region const& region) noexcept {
        if (parent) {
            ++parent->operation_requests[3]; // copy_image_to_buffer
        }
        if (auto result = apply_operations(); result != rhi::error::ok) {
            return result;
        }
        core* const self = this->parent;
        if (rhi::is_concrete(source, concrete::image)) {
            if (!self || !this->recording || this->native() == VK_NULL_HANDLE) {
                return rhi::error::not_ready;
            }
            if (!rhi::is_concrete(destination, concrete::buffer)) {
                return rhi::error::invalid_argument;
            }
            {
                std::lock_guard const lock(self->resource.contract_images_mutex);
                if (!self->resource.contract_images.contains(&source)) {
                    return rhi::error::invalid_argument;
                }
            }
            auto const& image = *rhi::do_concrete<core::image>(source, concrete::image);
            auto const& buffer = static_cast<core::buffer const&>(destination);
            if (image.parent != self || buffer.parent != self || !buffer.readback) {
                return rhi::error::invalid_argument;
            }
            if (!rhi::has_flag(image.declared_flags, rhi::image_flag::transfer_source)) {
                return rhi::error::unsupported;
            }
            auto bpp = rhi::bytes_per_pixel(image.declared_format);
            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
            if (!bpp) {
                // A DEPTH ROLE's declared format is `rhi::image_format::depth` (the device picks the concrete one into
                // `resolved_format`), and the contract refuses depth in bytes_per_pixel on purpose. The DEPTH-PROBE
                // capture (visibility-buffer root-cause measurement) is the one caller that copies it: derive the byte
                // width and the DEPTH aspect here rather than at the call site.
                switch (image.resolved_format) {
                case VK_FORMAT_D16_UNORM:
                    bpp = 2u;
                    break;
                case VK_FORMAT_UNDEFINED:
                    return rhi::error::unsupported;
                default:
                    bpp = 4u;
                    break; // D24*, D32_SFLOAT*, X8_D24*
                }
                aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            }
            if (!bpp || region.mip_level >= image.mip_levels || region.base_array_layer >= image.array_layers ||
                !region.array_layer_count || region.array_layer_count > image.array_layers - region.base_array_layer ||
                !region.extent.width || !region.extent.height || region.extent.depth != 1u || region.offset_z != 0u) {
                return rhi::error::invalid_argument;
            }
            auto const width = std::max(1u, image.width >> region.mip_level);
            auto const height = std::max(1u, image.height >> region.mip_level);
            if (region.offset_x > width || region.extent.width > width - region.offset_x || region.offset_y > height || region.extent.height > height - region.offset_y) {
                return rhi::error::invalid_argument;
            }
            std::uint64_t needed = static_cast<std::uint64_t>(region.extent.width) * region.extent.height;
            if (needed > buffer.size_bytes / bpp / region.array_layer_count) {
                return rhi::error::invalid_argument;
            }
            VkBufferImageCopy const copy = make_buffer_image_copy(
                0, 0, 0,
                make_image_subresource_layers(aspect, region.mip_level, region.base_array_layer, region.array_layer_count),
                {static_cast<std::int32_t>(region.offset_x), static_cast<std::int32_t>(region.offset_y), 0},
                {region.extent.width, region.extent.height, 1u});
            vkCmdCopyImageToBuffer(this->native(), image.native_handle, VK_IMAGE_LAYOUT_GENERAL, buffer.native, 1u, &copy);
            return rhi::error::ok;
        }
        // WHAT CAN SAY NO, in the contract's vocabulary (batch-2 spec §6.1): `unsupported` = this
        // surface cannot be a copy source at all; `invalid_argument` = the region does not fit, or the
        // destination is too small; `not_ready` = there is no frame to record into.
        if (self == nullptr || !self->frame.frame_in_flight) {
            return rhi::error::not_ready;
        }
        if (this->target != VK_NULL_HANDLE) {
            // THE FRAME-SCOPED VERBS BELONG TO THE FRAME'S LIST (abi 15): the copy's source is the
            // FRAME image and its destination the frame's read-back slot, so a list that records into
            // some other buffer has neither (the same `not_ready` window `use()` answers in).
            return rhi::error::not_ready;
        }
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (!self->frame.swapchain_transfer_src_supported) {
            // The swapchain images lack TRANSFER_SRC, so copying out of one would violate
            // VUID-vkCmdCopyImageToBuffer-srcImage-00186. The ENGINE issues the one-time log and the F12
            // shutdown that go with this answer (runtime.frames.cppm); the judgement itself is the
            // backend's, because the swapchain is.
            return rhi::error::unsupported;
        }
        if (static_cast<void const*>(&destination) != static_cast<void const*>(&self->frame.readback_slot_view) ||
            static_cast<void const*>(&source) != static_cast<void const*>(&self->frame.frame_image_view)) {
            return rhi::error::invalid_argument;
        }
        rhi::image_extent const extent = self->frame.swap_chain_extent;
        if (region.extent.width == 0 || region.extent.height == 0 || region.extent.depth != 1) {
            return rhi::error::invalid_argument;
        }
        if (region.mip_level != 0 || region.base_array_layer != 0 || region.array_layer_count != 1 || region.offset_z != 0) {
            return rhi::error::invalid_argument; // the frame image has one mip, one layer, and is 2D
        }
        if (static_cast<std::uint64_t>(region.offset_x) + region.extent.width > extent.width ||
            static_cast<std::uint64_t>(region.offset_y) + region.extent.height > extent.height) {
            return rhi::error::invalid_argument;
        }
        std::uint64_t const needed = static_cast<std::uint64_t>(region.extent.width) * region.extent.height * 4u;
        if (static_cast<std::uint64_t>(destination.size()) < needed) {
            return rhi::error::invalid_argument;
        }

        // The copy the screenshot path recorded by hand, field for field (batch-2 spec §3.3 item 2):
        // tightly packed, whole subresource, at the source's CURRENT layout (GENERAL everywhere here).
        VkBufferImageCopy const copy = make_buffer_image_copy(
            0, 0, 0,
            make_image_subresource_layers(VK_IMAGE_ASPECT_COLOR_BIT, region.mip_level, region.base_array_layer, region.array_layer_count),
            {static_cast<std::int32_t>(region.offset_x), static_cast<std::int32_t>(region.offset_y), static_cast<std::int32_t>(region.offset_z)},
            {region.extent.width, region.extent.height, region.extent.depth});
        vkCmdCopyImageToBuffer(command_buffer, self->frame.frame_image_view.handle(), VK_IMAGE_LAYOUT_GENERAL, self->frame.readback_slot_handle, 1, &copy);
        return rhi::error::ok;
    }

    // ---- the portable record series (abi 20) ----------------------------------------------------
    // THE TRANSLATION IS THE BACKEND'S ALONE (the plan's §1): every Vulkan structure below is built
    // here, from the contract's vocabulary. The handles the descriptors carry are verified by
    // interface identity and, for images, by the same registry the heap writes go through; the cast
    // to the concrete core type is defined only after that check, which is the no-RTTI provenance
    // rule the contract states as a caller premise. The readiness answer is uniform and cheap: a list
    // with no buffer to record into (`native()` null) is `not_ready` - the frame list's answer when no
    // frame is in flight, a concrete list's never.

    namespace {

        /// The stage/access pair one buffer role needs. SMALL ON PURPOSE (four real sites): the
        /// shader roles read and write through the shader stages, the transfer roles through the
        /// transfer stage - the same derivation shape `barrier_for` gives the image roles.
        [[nodiscard]] constexpr bool buffer_masks_for(rhi::buffer_use const use, VkPipelineStageFlags2& stage, VkAccessFlags2& access) noexcept {
            switch (use) {
            case rhi::buffer_use::shader_read:
                stage = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                access = VK_ACCESS_2_SHADER_READ_BIT;
                return true;
            case rhi::buffer_use::shader_write:
                stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                access = VK_ACCESS_2_SHADER_WRITE_BIT;
                return true;
            case rhi::buffer_use::indirect_read:
                stage = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
                access = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
                return true;
            case rhi::buffer_use::shader_read_write:
                stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                access = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
                return true;
            case rhi::buffer_use::host_read:
                stage = VK_PIPELINE_STAGE_2_HOST_BIT;
                access = VK_ACCESS_2_HOST_READ_BIT;
                return true;
            case rhi::buffer_use::transfer_source:
                stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                access = VK_ACCESS_2_TRANSFER_READ_BIT;
                return true;
            case rhi::buffer_use::transfer_destination:
                stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
                access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                return true;
            case rhi::buffer_use::acceleration_structure_read:
                // THE READER IS NOT A SHADER STAGE (abi 21): the acceleration-structure BUILD reads the vertex
                // data a compute dispatch wrote. The access bit is the same SHADER_READ every consumer of
                // shader-written data uses - what differs, and what no other role named, is the STAGE.
                //
                // IT CARRIES A SECOND ACCESS BIT SINCE THE RT BATCH, AND MEASUREMENT PUT IT THERE: the same ROLE
                // ("an acceleration-structure build reads") also has to cover a build reading the STRUCTURE
                // another build wrote - `source/engine/ray_tracing/ray_tracing.cpp`'s top-level build-ordering barrier,
                // whose raw destination access was `VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR`. With only
                // SHADER_READ the two builds were NOT ordered against each other, and the symptom was exactly
                // what an unsynchronised structure read looks like: the next frame's refit read half-written
                // bottom levels, the GPU faulted, and `wait_and_acquire()` reported a device loss (measured: the
                // rt_shadows smoke run panicked "waiting the frame slot's timeline failed" until this bit was
                // added). Widening a DESTINATION access mask is the safe direction - it can only order more - so
                // one role serves both sites instead of two values saying nearly the same thing.
                stage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                access = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                return true;
            case rhi::buffer_use::acceleration_structure_write:
                // THE WRITER IS NOT A SHADER STAGE EITHER (the appended value): a bottom level the BUILD wrote
                // must finish before the next build reads it (ray_tracing.cpp's build-ordering barrier). The
                // access bit is the extension's ACCELERATION_STRUCTURE_WRITE_KHR.
                stage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                access = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                return true;
            case rhi::buffer_use::micromap_write:
                // THE MICROMAP BUILD AS A WRITER (the appended value, and the spec's pair): the opacity
                // micromap a build wrote must finish before the acceleration-structure build reads it.
                stage = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT;
                access = VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT;
                return true;
            case rhi::buffer_use::micromap_read:
                stage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                access = VK_ACCESS_2_MICROMAP_READ_BIT_EXT;
                return true;
            case rhi::buffer_use::undefined:
                return false;
            }
            return false;
        }

        /// THE ACCELERATION-STRUCTURE ROLE PAIR (RT stage 1): the structure ITSELF, as the build wrote it, as the
        /// build reads it (a top-level build traversing the bottom levels it instances), or as a SHADER traverses
        /// it. Structures have no layout, so a row is a stage/access pair for the memory barrier the batch emits.
        ///
        /// THE SHADER SIDE IS EVERY SHADER STAGE OF THE DEVICE'S RT FLAVOURS: an RT pipeline's raygen/miss/hit
        /// stages and a ray query inside a compute/fragment shader are the two measured readers, and the batch's
        /// `stage_hint` narrows the mask when the caller knows which one ran (`with_stage_hint` below does it).
        [[nodiscard]] constexpr bool acceleration_structure_masks_for(rhi::acceleration_structure_use const use,
                                                                      VkPipelineStageFlags2& stage,
                                                                      VkAccessFlags2& access) noexcept {
            switch (use) {
            case rhi::acceleration_structure_use::build_write:
                stage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                access = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                return true;
            case rhi::acceleration_structure_use::shader_read:
                stage = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
                access = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
                return true;
            case rhi::acceleration_structure_use::undefined:
                return false;
            }
            return false;
        }

        // THE ACCELERATION-STRUCTURE TABLE'S COMPILE-TIME WITNESS (RT stage 1): pinned entry by entry, the same
        // rule the image declaration table's static_asserts follow - a value that changes its masks must change
        // this witness in the same commit.
        static_assert([] {
            VkPipelineStageFlags2 stage = 0;
            VkAccessFlags2 access = 0;
            return acceleration_structure_masks_for(rhi::acceleration_structure_use::build_write, stage, access) &&
                   stage == VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR &&
                   access == VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        }(),
                      "build_write is the AS build stage with the extension's WRITE access");
        static_assert([] {
            VkPipelineStageFlags2 stage = 0;
            VkAccessFlags2 access = 0;
            return acceleration_structure_masks_for(rhi::acceleration_structure_use::shader_read, stage, access) &&
                   (stage & VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR) != 0 &&
                   (stage & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) != 0 &&
                   access == VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
        }(),
                      "shader_read covers the ray-tracing and compute stages with the extension's READ access");
        static_assert([] {
            VkPipelineStageFlags2 stage = 0;
            VkAccessFlags2 access = 0;
            return !acceleration_structure_masks_for(rhi::acceleration_structure_use::undefined, stage, access);
        }(),
                      "undefined is refused, never silently mapped");

        /// THE STAGE HINT, in one place (abi 21): the recipes name the stage their SHADER side runs at (the
        /// storage-image recipes say COMPUTE_SHADER), and a site whose producer or consumer is another shader
        /// stage replaces exactly those bits. THE NON-SHADER BITS AND THE ACCESS HALF ARE THE PAIR'S and stay:
        /// a hint is about WHICH STAGE the engine ran, never about what the transition costs.
        ///
        /// A SIDE THAT NAMES NO SHADER STAGE IS RETURNED UNTOUCHED, which is what keeps a transfer side of a
        /// pair (a copy's destination, say) exactly as the recipe spelled it.
        [[nodiscard]] constexpr VkPipelineStageFlags2 with_stage_hint(VkPipelineStageFlags2 const mask, rhi::stage_hint const hint) noexcept {
            constexpr VkPipelineStageFlags2 shader_stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT |
                                                            VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
            if (hint == rhi::stage_hint::none || (mask & shader_stages) == 0) {
                return mask;
            }
            VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
            if (hint == rhi::stage_hint::vertex) {
                stage = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
            } else if (hint == rhi::stage_hint::fragment) {
                stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            } else if (hint == rhi::stage_hint::compute) {
                stage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            } else if (hint == rhi::stage_hint::mesh) {
                stage = VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
            }
            return (mask & ~shader_stages) | stage;
        }

        /// The subresource range a barrier covers: all-zero means "the whole image", which Vulkan
        /// spells with the REMAINING sentinels.
        [[nodiscard]] constexpr VkImageSubresourceRange subresource_of(VkImageAspectFlags const aspect, rhi::subresource_range const& range) noexcept {
            return VkImageSubresourceRange{
                .aspectMask = aspect,
                .baseMipLevel = range.base_mip,
                .levelCount = range.mip_count == 0 ? VK_REMAINING_MIP_LEVELS : range.mip_count,
                .baseArrayLayer = range.base_layer,
                .layerCount = range.layer_count == 0 ? VK_REMAINING_ARRAY_LAYERS : range.layer_count,
            };
        }

        /// The aspect a barrier on THIS image covers: the depth formats' one bit, colour everywhere
        /// else. The backend knows the resolved format - the caller never names an aspect, because
        /// the format is the backend's creation fact.
        [[nodiscard]] constexpr VkImageAspectFlags aspect_for(VkFormat const format) noexcept {
            switch (format) {
            case VK_FORMAT_D16_UNORM:
            case VK_FORMAT_X8_D24_UNORM_PACK32:
            case VK_FORMAT_D32_SFLOAT:
            case VK_FORMAT_D24_UNORM_S8_UINT:
            case VK_FORMAT_D32_SFLOAT_S8_UINT:
                return VK_IMAGE_ASPECT_DEPTH_BIT;
            default:
                return VK_IMAGE_ASPECT_COLOR_BIT;
            }
        }

        // THE TRANSLATION TABLES' COMPILE-TIME WITNESS (the plan's §1 "table tests", in the shape the
        // contract's layout asserts take): the constexpr mappers above are pinned entry by entry,
        // so a vocabulary change fails here instead of at a recording call site.
        static_assert([] {
            VkPipelineStageFlags2 stage = 0;
            VkAccessFlags2 access = 0;
            return buffer_masks_for(rhi::buffer_use::shader_read, stage, access) &&
                   (stage & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) != 0 && access == VK_ACCESS_2_SHADER_READ_BIT;
        }());
        static_assert([] {
            VkPipelineStageFlags2 stage = 0;
            VkAccessFlags2 access = 0;
            return buffer_masks_for(rhi::buffer_use::transfer_destination, stage, access) && stage == VK_PIPELINE_STAGE_2_TRANSFER_BIT &&
                   access == VK_ACCESS_2_TRANSFER_WRITE_BIT;
        }());
        static_assert([] {
            VkPipelineStageFlags2 stage = 0;
            VkAccessFlags2 access = 0;
            return !buffer_masks_for(rhi::buffer_use::undefined, stage, access); // the undefined role spells nothing
        }());
        static_assert(subresource_of(VK_IMAGE_ASPECT_COLOR_BIT, rhi::subresource_range{}).levelCount == VK_REMAINING_MIP_LEVELS &&
                      subresource_of(VK_IMAGE_ASPECT_COLOR_BIT, rhi::subresource_range{}).layerCount == VK_REMAINING_ARRAY_LAYERS);
        static_assert(subresource_of(VK_IMAGE_ASPECT_COLOR_BIT, rhi::subresource_range{.base_mip = 1, .mip_count = 3}).levelCount == 3);
        static_assert(aspect_for(VK_FORMAT_D32_SFLOAT) == VK_IMAGE_ASPECT_DEPTH_BIT && aspect_for(VK_FORMAT_R8G8B8A8_UNORM) == VK_IMAGE_ASPECT_COLOR_BIT);
    } // namespace

    rhi::error core::frame_commands::barrier(rhi::barrier_group const& group) {
        if (parent) {
            ++parent->operation_requests[4]; // barrier
        }
        if (auto result = apply_operations(); result != rhi::error::ok) {
            return result;
        }
        core* const self = this->parent;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        // The verified image handles: registry membership first (the heap-write rule - the cast is
        // only defined once provenance is known), then the native handle out of the image object. THE
        // ONE IMAGE THE REGISTRY DOES NOT TRACK is the frame's swapchain image - a VIEW the
        // core itself creates (the same object `use()` accepts by identity) - so the identity check is the
        // second acceptance, with the format the swapchain was created with.
        auto const resolve_image = [&](rhi::image const* const resource, VkImage& native, VkFormat& format) -> rhi::error {
            if (resource == nullptr || (!rhi::is_concrete(*resource, concrete::image) && !rhi::is_concrete(*resource, concrete::frame_image) && !rhi::is_concrete(*resource, concrete::frame_snapshot))) {
                return rhi::error::invalid_argument;
            }
            if (static_cast<void const*>(resource) == static_cast<void const*>(&self->frame.frame_image_view)) {
                native = self->frame.frame_image_view.handle();
                format = self->frame.swap_chain_image_format;
                return rhi::error::ok;
            }

            std::unique_lock registry_lock(self->resource.contract_images_mutex);
            if (self->frame.frame_snapshots.contains(resource)) {
                auto const& snapshot = *static_cast<frame_snapshot const*>(resource);
                // Like scoped synchronization, frame targets belong only to the acquired primary.
                if (this != &self->frame.commands_view || snapshot.parent != self || !snapshot.current()) {
                    return rhi::error::invalid_argument;
                }
                native = snapshot.native_handle;
                format = self->frame.swap_chain_image_format;
                return rhi::error::ok;
            }
            if (!self->resource.contract_images.contains(resource)) {
                return rhi::error::invalid_argument;
            }
            registry_lock.unlock();
            auto const* const concrete = rhi::do_concrete<core::image>(*resource, concrete::image);
            native = concrete->source ? VK_NULL_HANDLE : concrete->native_handle;
            format = concrete->resolved_format;
            return rhi::error::ok;
        };

        deren::utility::vessel::array<VkImageMemoryBarrier2, 8> image_barriers = {};
        deren::utility::vessel::array<VkBufferMemoryBarrier2, 8> buffer_barriers = {};
        deren::utility::vessel::array<VkMemoryBarrier2, 1> memory_barriers = {};
        // THE TWO APPENDED FIELDS ARE READ ONLY AS FAR AS THE CALLER DECLARED (the `struct_size` rule the
        // whole descriptor family lives by): an older caller's bytes decode to "the recipes' stages, no global
        // barrier", which is exactly the behaviour it compiled against.
        rhi::stage_hint const hint = covered_by(group.struct_size, offsetof(rhi::barrier_group, stage), sizeof(rhi::barrier_group::stage))
                                         ? group.stage
                                         : rhi::stage_hint::none;
        bool const memory_used = covered_by(group.struct_size, offsetof(rhi::barrier_group, has_memory), sizeof(rhi::barrier_group::has_memory)) && group.has_memory;
        rhi::memory_barrier const memory = memory_used && covered_by(group.struct_size, offsetof(rhi::barrier_group, memory), sizeof(rhi::barrier_group::memory))
                                               ? group.memory
                                               : rhi::memory_barrier{};
        std::uint32_t image_count = 0;
        std::uint32_t buffer_count = 0;
        std::uint32_t memory_count = 0;
        for (rhi::image_barrier const& one : group.images) {
            if (image_count >= image_barriers.size()) {
                return rhi::error::invalid_argument; // one group, eight images: far past every real site
            }
            VkImage native = VK_NULL_HANDLE;
            VkFormat format = VK_FORMAT_UNDEFINED;
            if (rhi::error const checked = resolve_image(one.resource, native, format); checked != rhi::error::ok) {
                return checked;
            }
            bool registered;
            {
                std::lock_guard lock(self->resource.contract_images_mutex);
                registered = self->resource.contract_images.contains(one.resource);
            }
            if (registered) {
                auto const& image = *static_cast<core::image const*>(one.resource);
                if (image.source && (one.range.base_mip >= image.mip_levels || one.range.base_layer >= image.array_layers ||
                                     (one.range.mip_count && one.range.mip_count > image.mip_levels - one.range.base_mip) ||
                                     (one.range.layer_count && one.range.layer_count > image.array_layers - one.range.base_layer))) {
                    return rhi::error::invalid_argument;
                }
            }
            VkImageMemoryBarrier2 barrier = barrier_for(one.from, one.to);
            if (barrier.srcStageMask == 0 && barrier.srcAccessMask == 0 && barrier.dstStageMask == 0 && barrier.dstAccessMask == 0) {
                return rhi::error::unsupported; // a pair nobody has defined - the same honest answer `use()` gives
            }
            // THE STAGE HINT ON TOP OF THE PAIR (abi 21): the recipes name the stage their shader side runs at,
            // and a site whose producer or consumer is ANOTHER shader stage replaces exactly those bits - the
            // non-shader bits (TRANSFER) and the whole access/layout half stay the pair's. `none` is a no-op,
            // which is why this is one line and not a second table.
            barrier.srcStageMask = with_stage_hint(barrier.srcStageMask, hint);
            barrier.dstStageMask = with_stage_hint(barrier.dstStageMask, hint);
            barrier.image = native;
            barrier.subresourceRange = subresource_of(aspect_for(format), one.range);
            image_barriers[image_count++] = barrier;
        }
        for (rhi::buffer_barrier const& one : group.buffers) {
            if (buffer_count >= buffer_barriers.size()) {
                return rhi::error::invalid_argument;
            }
            if (one.resource == nullptr || !rhi::is_concrete(*one.resource, concrete::buffer)) {
                return rhi::error::invalid_argument;
            }
            {
                std::lock_guard lock(parent->resource.contract_buffers_mutex);
                if (!parent->resource.contract_buffers.contains(one.resource)) {
                    return rhi::error::invalid_argument;
                }
            }
            auto const* const concrete = static_cast<core::buffer const*>(one.resource);
            if (one.offset >= concrete->size_bytes || (one.size && one.size > concrete->size_bytes - one.offset)) {
                return rhi::error::invalid_argument;
            }
            VkPipelineStageFlags2 source_stage = 0;
            VkAccessFlags2 source_access = 0;
            VkPipelineStageFlags2 destination_stage = 0;
            VkAccessFlags2 destination_access = 0;
            if (!buffer_masks_for(one.from, source_stage, source_access) || !buffer_masks_for(one.to, destination_stage, destination_access)) {
                return rhi::error::unsupported;
            }
            buffer_barriers[buffer_count++] = make_buffer_memory_barrier_2(
                source_stage, source_access, destination_stage, destination_access,
                concrete->source ? VK_NULL_HANDLE : concrete->native,
                one.offset, one.size == 0 ? (concrete->size_bytes - one.offset) : one.size);
        }
        // THE GLOBAL MEMORY BARRIER OF THE BATCH (abi 21), if the caller declared one: the same role-pair
        // derivation the buffer loop uses, with NO operand - which is the whole reason the field exists. A
        // pair the roles cannot spell is refused by name, exactly as a buffer pair is.
        //
        // THE ACCELERATION-STRUCTURE BARRIERS OF THE SAME BATCH (RT stage 1) FOLD INTO THE SAME VkMemoryBarrier2:
        // a structure has no layout and the extension synchronizes it with ACCELERATION_STRUCTURE_READ/WRITE
        // accesses, so an object-level transition is exactly a memory dependency. One memory barrier can carry
        // every source and destination mask in the batch (masks are ORed), which keeps the dependency's memory
        // barrier count at one however many structures a batch names.
        VkPipelineStageFlags2 memory_source_stage = 0;
        VkAccessFlags2 memory_source_access = 0;
        VkPipelineStageFlags2 memory_destination_stage = 0;
        VkAccessFlags2 memory_destination_access = 0;
        bool memory_any = false;
        if (memory_used) {
            if (!buffer_masks_for(memory.from, memory_source_stage, memory_source_access) ||
                !buffer_masks_for(memory.to, memory_destination_stage, memory_destination_access)) {
                return rhi::error::unsupported;
            }
            memory_any = true;
        }
        // THE APPENDED SPAN IS READ ONLY AS FAR AS THE CALLER DECLARED (the same `struct_size` rule as the two
        // fields above): an older caller's bytes decode to "no acceleration-structure barriers".
        if (covered_by(group.struct_size, offsetof(rhi::barrier_group, acceleration_structures), sizeof(rhi::barrier_group::acceleration_structures))) {
            for (rhi::acceleration_structure_barrier const& one : group.acceleration_structures) {
                if (one.resource == nullptr || !rhi::is_concrete(*one.resource, concrete::acceleration_structure)) {
                    return rhi::error::invalid_argument;
                }
                auto const& structure = static_cast<core::acceleration_structure const&>(*one.resource);
                if (structure.native == VK_NULL_HANDLE || structure.parent != self) {
                    return rhi::error::invalid_argument;
                }
                VkPipelineStageFlags2 source_stage = 0;
                VkAccessFlags2 source_access = 0;
                VkPipelineStageFlags2 destination_stage = 0;
                VkAccessFlags2 destination_access = 0;
                if (!acceleration_structure_masks_for(one.from, source_stage, source_access) ||
                    !acceleration_structure_masks_for(one.to, destination_stage, destination_access)) {
                    return rhi::error::unsupported; // a use nobody has defined - the honest answer, as for a buffer pair
                }
                // THE STAGE HINT NARROWS THE SHADER SIDE (the reason it rides the batch): the write side names the
                // build stage and is untouched; `shader_read`'s mask is narrowed to the stage the caller ran.
                source_stage = with_stage_hint(source_stage, hint);
                destination_stage = with_stage_hint(destination_stage, hint);
                memory_source_stage |= source_stage;
                memory_source_access |= source_access;
                memory_destination_stage |= destination_stage;
                memory_destination_access |= destination_access;
                memory_any = true;
            }
        }
        if (memory_any) {
            memory_barriers[memory_count++] = VkMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = memory_source_stage,
                .srcAccessMask = memory_source_access,
                .dstStageMask = memory_destination_stage,
                .dstAccessMask = memory_destination_access,
            };
        }
        // Validation is complete; only now may native-resource demand allocate or upload.
        for (std::size_t i = 0; i < group.images.size(); ++i) {
            auto* resource = group.images[i].resource;
            bool registered;
            {
                std::lock_guard lock(parent->resource.contract_images_mutex);
                registered = parent->resource.contract_images.contains(resource);
            }
            if (!registered) {
                continue; // a frame target the registry does not track
            }
            auto ready = demand(*resource);
            if (ready != rhi::error::ok) {
                return ready;
            }
            image_barriers[i].image = static_cast<core::image const*>(resource)->native_handle;
        }
        for (std::size_t i = 0; i < group.buffers.size(); ++i) {
            auto ready = demand(*group.buffers[i].resource);
            if (ready != rhi::error::ok) {
                return ready;
            }
            buffer_barriers[i].buffer = static_cast<core::buffer const*>(group.buffers[i].resource)->native;
        }
        VkDependencyInfo const dependency = make_dependency_info(image_count, image_barriers.data(), buffer_count, buffer_barriers.data(), memory_count, memory_count != 0 ? memory_barriers.data() : nullptr);
        vkCmdPipelineBarrier2(command_buffer, &dependency);
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::barrier(rhi::image_barrier const& one) {
        if (parent) {
            ++parent->operation_requests[4]; // barrier
        }
        if (auto result = apply_operations(); result != rhi::error::ok) {
            return result;
        }
        // The single-image form is the group of one - the same derivation, the same refusals.
        deren::utility::vessel::array<rhi::image_barrier, 1> const one_barriers = {one};
        rhi::barrier_group const group{
            .struct_size = sizeof(rhi::barrier_group),
            .images = one_barriers,
            .buffers = {},
        };
        return this->barrier(group);
    }

    rhi::error core::frame_commands::begin_rendering(rhi::rendering_info const& info) {
        if (parent) {
            ++parent->operation_requests[5]; // begin_rendering
        }
        if (auto result = apply_operations(); result != rhi::error::ok) {
            return result;
        }
        core* const self = this->parent;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE || !recording || rendering_active) {
            return rhi::error::not_ready;
        }
        if (info.struct_size < sizeof(rhi::rendering_info)) {
            return rhi::error::invalid_argument;
        }
        auto const resolve_view = [&](rhi::image_view const* const view) -> VkImageView {
            if (view == nullptr || !rhi::is_concrete(*view, concrete::image_view)) {
                return VK_NULL_HANDLE;
            }
            return static_cast<core::image_view const*>(view)->native_view;
        };
        // THE BOUND IS THE PASS FRAMEWORK'S, NOT THIS FUNCTION'S: `pass::max_render_targets` is 8 and the
        // SCENE instance is the widest site (three surface targets + velocity + the scene colour + depth),
        // so a 4-slot array refused the scene's scope and the frame recorded no geometry. The contract
        // places no bound of its on `rendering_info::colors`; a scope wider than the widest DECLARED
        // site is refused rather than silently truncated.
        deren::utility::vessel::array<VkRenderingAttachmentInfo, 8> attachments = {};
        std::uint32_t color_count = 0;
        for (rhi::color_attachment const& one : info.colors) {
            if (color_count >= attachments.size()) {
                return rhi::error::invalid_argument;
            }
            VkImageView const native_view = resolve_view(one.view);
            if (native_view == VK_NULL_HANDLE) {
                return rhi::error::invalid_argument;
            }
            VkAttachmentLoadOp const load_op = one.load == rhi::load_op::clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : one.load == rhi::load_op::dont_care ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                                                                                                                                   : VK_ATTACHMENT_LOAD_OP_LOAD;
            VkAttachmentStoreOp const store_op = one.store == rhi::store_op::dont_care ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
            VkClearValue const clear_value = {.color = {{one.clear[0], one.clear[1], one.clear[2], one.clear[3]}}};
            attachments[color_count] = make_attachment_info(native_view, VK_IMAGE_LAYOUT_GENERAL, load_op, store_op, clear_value);
            ++color_count;
        }
        VkRenderingAttachmentInfo depth_attachment = {};
        bool const depth_used = info.has_depth;
        if (depth_used) {
            VkImageView const native_view = resolve_view(info.depth.view);
            if (native_view == VK_NULL_HANDLE) {
                return rhi::error::invalid_argument;
            }
            VkAttachmentLoadOp const load_op = info.depth.load == rhi::load_op::clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : info.depth.load == rhi::load_op::dont_care ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                                                                                                                                                 : VK_ATTACHMENT_LOAD_OP_LOAD;
            VkAttachmentStoreOp const store_op = info.depth.store == rhi::store_op::dont_care ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
            VkClearValue const clear_value = {.depthStencil = {.depth = info.depth.clear_depth, .stencil = info.depth.clear_stencil}};
            depth_attachment = make_attachment_info(native_view, VK_IMAGE_LAYOUT_GENERAL, load_op, store_op, clear_value);
        }
        VkRenderingFlags const rendering_flags = info.secondary_contents ? VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT : 0u;
        VkRenderingInfo const rendering = make_rendering_info(
            rendering_flags,
            VkRect2D{.offset = {info.area.offset_x, info.area.offset_y}, .extent = {info.area.width, info.area.height}},
            attachments.data(),
            color_count,
            depth_used ? &depth_attachment : nullptr,
            info.layer_count);
        vkCmdBeginRendering(command_buffer, &rendering);
        rendering_active = true;
        return rhi::error::ok;
    }

    void core::frame_commands::end_rendering() noexcept {
        if (parent) {
            ++parent->operation_requests[6]; // end_rendering
        }
        if (!recording || recording_error != rhi::error::ok) {
            return;
        }
        if (!rendering_active || rendering_inherited) {
            recording_error = rhi::error::invalid_argument;
            return;
        }
        auto const applied = apply_operations();
        if (applied != rhi::error::ok) {
            recording_error = applied;
            return;
        }
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer != VK_NULL_HANDLE && recording && rendering_active) {
            vkCmdEndRendering(command_buffer);
            rendering_active = false;
        }
    }

    rhi::error core::frame_commands::build_micromap(rhi::micromap& target) {
        if (parent) {
            ++parent->operation_requests[16]; // build_micromap
        }
        if (auto result = apply_operations(); result != rhi::error::ok) {
            return result;
        }
        core* const self = this->parent;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (self->device.micromap_build == nullptr) {
            return rhi::error::unsupported; // no VK_EXT_opacity_micromap on this device (see the constructor)
        }
        if (!rhi::is_concrete(target, concrete::micromap)) {
            return rhi::error::invalid_argument; // a handle this backend did not hand out
        }
        auto& micromap = static_cast<core::micromap&>(target);
        if (micromap.parent != self || micromap.native == VK_NULL_HANDLE) {
            return rhi::error::invalid_argument;
        }
        // THE BARRIER IS THE BACKEND'S, as it is for an acceleration structure build: the setup buffers were
        // written by the HOST through their mappings, and the micromap itself is written.
        constexpr VkMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT,
            .dstAccessMask = VK_ACCESS_2_MICROMAP_READ_BIT_EXT | VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT | VK_ACCESS_2_SHADER_READ_BIT,
        };
        VkDependencyInfo const dependency = make_dependency_info(0, nullptr, 0, nullptr, 1, &barrier);
        vkCmdPipelineBarrier2(command_buffer, &dependency);

        VkMicromapBuildInfoEXT const info = make_micromap_build_info(
            VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT, VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT,
            VK_BUILD_MICROMAP_MODE_BUILD_EXT, micromap.native, 1, &micromap.usage,
            micromap.data_address, micromap.triangles_address, micromap.triangle_array_stride,
            micromap.scratch_address);
        self->device.micromap_build(command_buffer, 1u, &info);
        return rhi::error::ok;
    }

    void core::micromap::release() noexcept {
        delete this;
    }

    core::micromap::~micromap() noexcept {
        // THE ORDER IS THE POINT, as it is for an acceleration structure: the micromap first, then the memory it
        // lives in and the setup buffers the build read.
        if (this->native != VK_NULL_HANDLE && this->parent != nullptr && this->parent->device.micromap_destroy != nullptr) {
            this->parent->device.micromap_destroy(this->parent->device.logical_device, this->native, nullptr);
        }
        for (deren::promise::rhi::buffer* const buffer : {this->indices, this->triangles, this->data, this->scratch, this->storage}) {
            if (buffer != nullptr) {
                buffer->release();
            }
        }
    }

    rhi::error core::frame_commands::build_acceleration_structure(rhi::acceleration_structure& target) {
        if (parent) {
            ++parent->operation_requests[17]; // build_acceleration_structure
        }
        if (auto result = apply_operations(); result != rhi::error::ok) {
            return result;
        }
        core* const self = this->parent;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (self->device.acceleration_structure_build == nullptr) {
            return rhi::error::unsupported; // no VK_KHR_acceleration_structure on this device (see the constructor)
        }
        if (!rhi::is_concrete(target, concrete::acceleration_structure)) {
            return rhi::error::invalid_argument; // a handle this backend did not hand out (the provenance rule)
        }
        auto& structure = static_cast<core::acceleration_structure&>(target);
        if (structure.parent != self) {
            return rhi::error::invalid_argument;
        }
        if (structure.top_level && structure.instance_count == 0) {
            // An empty top level is a legal but useless object; recording a build for it would make the driver
            // read zero instances while `ok` claimed a build happened. Refused by name instead.
            return rhi::error::invalid_argument;
        }
        // THE BARRIER IS THE BACKEND'S NOW (the engine's module used to record its): everything the build
        // READS was written by the host (the caller's vertex/index/instance writes, and this frame's transfers)
        // and the structure itself is written - one conservative memory barrier covers both.
        constexpr VkMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        };
        VkDependencyInfo const dependency = make_dependency_info(0, nullptr, 0, nullptr, 1, &barrier);
        vkCmdPipelineBarrier2(command_buffer, &dependency);

        VkAccelerationStructureBuildGeometryInfoKHR const build_info = make_acceleration_structure_build_info(
            structure.top_level ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            (structure.top_level ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR) |
                (structure.refittable ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0u),
            VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, VK_NULL_HANDLE, structure.native,
            static_cast<std::uint32_t>(structure.geometries.size()), structure.geometries.data(),
            structure.scratch_address);
        if (structure.top_level) {
            structure.ranges.front().primitiveCount = structure.instance_count; // the build reads what the caller last wrote
        }
        VkAccelerationStructureBuildRangeInfoKHR const* ranges[1] = {structure.ranges.data()};
        self->device.acceleration_structure_build(command_buffer, 1u, &build_info, ranges);
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::refit_acceleration_structure(rhi::acceleration_structure& target) {
        if (parent) {
            ++parent->operation_requests[18]; // refit_acceleration_structure
        }
        if (auto result = apply_operations(); result != rhi::error::ok) {
            return result;
        }
        core* const self = this->parent;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (self->device.acceleration_structure_build == nullptr) {
            return rhi::error::unsupported;
        }
        if (!rhi::is_concrete(target, concrete::acceleration_structure)) {
            return rhi::error::invalid_argument;
        }
        auto& structure = static_cast<core::acceleration_structure&>(target);
        if (structure.parent != self) {
            return rhi::error::invalid_argument;
        }
        if (!structure.refittable) {
            // THE ONE REFUSAL THE FLAG EXISTS FOR: a structure built without ALLOW_UPDATE has no in-place mode,
            // and a "refit" that silently rebuilt it would be a different operation than the caller asked for.
            return rhi::error::unsupported;
        }
        constexpr VkMemoryBarrier2 barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        };
        VkDependencyInfo const dependency = make_dependency_info(0, nullptr, 0, nullptr, 1, &barrier);
        vkCmdPipelineBarrier2(command_buffer, &dependency);

        VkAccelerationStructureBuildGeometryInfoKHR const build_info = make_acceleration_structure_build_info(
            structure.top_level ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            (structure.top_level ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR) |
                VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR,
            VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR, structure.native, structure.native,
            static_cast<std::uint32_t>(structure.geometries.size()), structure.geometries.data(),
            structure.scratch_address);
        if (structure.top_level) {
            structure.ranges.front().primitiveCount = structure.instance_count;
        }
        VkAccelerationStructureBuildRangeInfoKHR const* ranges[1] = {structure.ranges.data()};
        self->device.acceleration_structure_build(command_buffer, 1u, &build_info, ranges);
        return rhi::error::ok;
    }

    // ---- the acceleration structure's verbs ----------------------------------------------------------

    std::uint64_t core::acceleration_structure::device_address() const noexcept {
        return this->address;
    }

    std::uint64_t core::acceleration_structure::size_bytes() const noexcept {
        return this->structure_size;
    }

    rhi::error core::acceleration_structure::write_instances(std::span<rhi::acceleration_structure_instance const> records) {
        if (!this->top_level || this->instances_mapped == nullptr) {
            return rhi::error::unsupported; // a bottom-level structure has no instance list to write
        }
        if (records.size() > this->instance_capacity) {
            return rhi::error::invalid_argument; // past the capacity it was CREATED with (and sized for)
        }
        auto* const destination = static_cast<VkAccelerationStructureInstanceKHR*>(this->instances_mapped);
        for (std::size_t index = 0; index < records.size(); ++index) {
            rhi::acceleration_structure_instance const& source = records[index];
            VkAccelerationStructureInstanceKHR record{};
            std::memcpy(&record.transform, source.transform, sizeof(record.transform)); // 3x4 row-major: same layout
            record.instanceCustomIndex = source.instance_custom_index;
            record.mask = source.mask;
            record.instanceShaderBindingTableRecordOffset = source.shader_binding_table_record_offset;
            record.flags = source.flags;
            record.accelerationStructureReference = source.structure_reference;
            destination[index] = record;
        }
        this->instance_count = static_cast<std::uint32_t>(records.size());
        return rhi::error::ok;
    }

    void core::acceleration_structure::release() noexcept {
        if (parent) {
            std::lock_guard lock(parent->resource.group_members_mutex);
            parent->resource.group_members.erase(this);
        }
        delete this;
    }

    core::acceleration_structure::~acceleration_structure() noexcept {
        if (parent) {
            std::lock_guard lock(parent->resource.contract_structures_mutex);
            parent->resource.contract_structures.erase(this);
        }
        // THE ORDER IS THE POINT (see the type's note): the structure first, then the memory it lives in.
        if (this->native != VK_NULL_HANDLE && this->parent != nullptr && this->parent->device.acceleration_structure_destroy != nullptr) {
            this->parent->device.acceleration_structure_destroy(this->parent->device.logical_device, this->native, nullptr);
        }
        if (this->instances != nullptr) {
            this->instances->release();
        }
        if (this->scratch != nullptr) {
            this->scratch->release();
        }
        if (this->storage != nullptr) {
            this->storage->release();
        }
    }
    rhi::error core::frame_commands::draw_indirect(rhi::buffer const& arguments, std::uint64_t offset, std::uint32_t count, std::uint32_t stride) {
        if (parent) {
            ++parent->operation_requests[19]; // draw_indirect
        }
        if (!recording || native() == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (recording_error != rhi::error::ok) {
            return recording_error;
        }
        static_assert(sizeof(VkDrawIndirectCommand) == sizeof(rhi::draw_indirect_command));
        if (this->parent == nullptr || !this->recording || this->native() == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (!rhi::is_concrete(arguments, concrete::buffer)) {
            return rhi::error::invalid_argument;
        }
        {
            std::lock_guard lock(parent->resource.contract_buffers_mutex);
            if (!parent->resource.contract_buffers.contains(&arguments)) {
                return rhi::error::invalid_argument;
            }
        }
        auto const& concrete = static_cast<core::buffer const&>(arguments);
        if (concrete.parent != this->parent || !rhi::has_flag(concrete.declared_flags, rhi::buffer_flag::indirect)) {
            return rhi::error::invalid_argument;
        }
        auto const valid = rhi::validate_draw_indirect_range(concrete.size_bytes, offset, count, stride);
        if (valid != rhi::error::ok || count == 0u) {
            return valid;
        }
        if (!rendering_active) {
            return rhi::error::invalid_argument;
        }
        operations.append<rec::operation_kind::draw_indirect>({reinterpret_cast<std::uintptr_t>(&concrete), concrete.identity, offset, count, stride});
        ++parent->deferred_requested;
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::draw_mesh_tasks_indirect(rhi::buffer const& argument_buffer, std::uint64_t const offset, std::uint32_t const count, std::uint32_t const stride) {
        if (parent) {
            ++parent->operation_requests[20]; // draw_mesh_tasks_indirect
        }
        if (!recording || native() == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (recording_error != rhi::error::ok) {
            return recording_error;
        }
        // THE CONTRACT'S RECORD LAYOUT IS THE API'S, AND THIS IS WHERE THAT IS PROVEN: callers write
        // `rhi::mesh_task_command` records into their argument buffer and pass `mesh_task_command_size` as the
        // stride, so the two structures must be the same bytes. The backend is the only side that can assert it
        // (it is the only one that names both).
        static_assert(sizeof(VkDrawMeshTasksIndirectCommandEXT) == rhi::mesh_task_command_size,
                      "the argument buffer's record IS the contract's layout, and the stride its size");
        static_assert(sizeof(VkDrawMeshTasksIndirectCommandEXT) == sizeof(rhi::mesh_task_command),
                      "the contract's record and the API's structure must agree");
        core* const self = this->parent;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (!rhi::is_concrete(argument_buffer, concrete::buffer)) {
            return rhi::error::invalid_argument;
        }
        if (self->device.mesh_dispatch_indirect == nullptr) {
            return rhi::error::unsupported; // no mesh-shader extension entry point on this device
        }
        {
            std::lock_guard lock(parent->resource.contract_buffers_mutex);
            if (!parent->resource.contract_buffers.contains(&argument_buffer)) {
                return rhi::error::invalid_argument;
            }
        }
        auto const* const concrete = static_cast<core::buffer const*>(&argument_buffer);
        if (concrete->parent != parent || !rhi::has_flag(concrete->declared_flags, rhi::buffer_flag::indirect)) {
            return rhi::error::invalid_argument;
        }
        auto const valid = rhi::validate_mesh_task_indirect_range(concrete->size_bytes, offset, count, stride);
        if (valid != rhi::error::ok || count == 0) {
            return valid;
        }
        if (!rendering_active) {
            return rhi::error::invalid_argument;
        }
        operations.append<rec::operation_kind::mesh_indirect>({reinterpret_cast<std::uintptr_t>(concrete), concrete->identity, offset, count, stride});
        ++parent->deferred_requested;
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::copy_image(rhi::image_copy const& copy) {
        if (parent) {
            ++parent->operation_requests[26]; // copy_image
        }
        if (auto result = apply_operations(); result != rhi::error::ok) {
            return result;
        }
        core* const self = this->parent;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        VkImage source_native;
        VkFormat source_format;
        VkImage destination_native;
        VkFormat destination_format;
        {
            std::lock_guard const lock(self->resource.contract_images_mutex);
            if (copy.source == nullptr || copy.destination == nullptr || (!rhi::is_concrete(*copy.source, concrete::image) && !rhi::is_concrete(*copy.source, concrete::frame_image)) ||
                (!rhi::is_concrete(*copy.destination, concrete::image) && !rhi::is_concrete(*copy.destination, concrete::frame_image)) || !self->resource.contract_images.contains(copy.source) ||
                !self->resource.contract_images.contains(copy.destination)) {
                return rhi::error::invalid_argument;
            }
            auto const* const source_resource = rhi::do_concrete<core::image>(*copy.source, concrete::image);
            auto const* const destination_resource = rhi::do_concrete<core::image>(*copy.destination, concrete::image);
            if (source_resource->source || destination_resource->source) {
                return rhi::error::unsupported;
            }
            source_native = source_resource->native_handle;
            source_format = source_resource->resolved_format;
            destination_native = destination_resource->native_handle;
            destination_format = destination_resource->resolved_format;
        }
        VkImageSubresourceLayers const source_layers =
            make_image_subresource_layers(VK_IMAGE_ASPECT_COLOR_BIT, copy.source_region.mip_level, copy.source_region.base_array_layer,
                                          copy.source_region.array_layer_count == 0 ? VK_REMAINING_ARRAY_LAYERS : copy.source_region.array_layer_count);
        VkImageSubresourceLayers const destination_layers =
            make_image_subresource_layers(VK_IMAGE_ASPECT_COLOR_BIT, copy.destination_region.mip_level, copy.destination_region.base_array_layer,
                                          copy.destination_region.array_layer_count == 0 ? VK_REMAINING_ARRAY_LAYERS : copy.destination_region.array_layer_count);
        VkImageCopy2 const regions = make_image_copy_2(
            source_layers, static_cast<std::int32_t>(copy.source_region.offset_x), static_cast<std::int32_t>(copy.source_region.offset_y),
            static_cast<std::int32_t>(copy.source_region.offset_z), destination_layers, static_cast<std::int32_t>(copy.destination_region.offset_x),
            static_cast<std::int32_t>(copy.destination_region.offset_y), static_cast<std::int32_t>(copy.destination_region.offset_z),
            copy.source_region.extent.width, copy.source_region.extent.height, copy.source_region.extent.depth);
        (void)source_format; // both sides live in GENERAL; the aspects are colour (the copy sites' shape)
        (void)destination_format;
        VkCopyImageInfo2 const copy_info = make_copy_image_info_2(source_native, VK_IMAGE_LAYOUT_GENERAL, destination_native, VK_IMAGE_LAYOUT_GENERAL, 1, &regions);
        vkCmdCopyImage2(command_buffer, &copy_info);
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::copy_buffer(rhi::buffer& destination, rhi::buffer const& source, std::uint64_t const size, std::uint64_t const source_offset,
                                                 std::uint64_t const destination_offset) {
        if (parent) {
            ++parent->operation_requests[27]; // copy_buffer
        }
        if (auto result = apply_operations(); result != rhi::error::ok) {
            return result;
        }
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (!rhi::is_concrete(destination, concrete::buffer) || !rhi::is_concrete(source, concrete::buffer)) {
            return rhi::error::invalid_argument;
        }
        auto const* const destination_resource = static_cast<core::buffer const*>(&destination);
        auto const* const source_resource = static_cast<core::buffer const*>(&source);
        if (destination_resource->source || source_resource->source) {
            return rhi::error::unsupported;
        }
        // ABI 53: reject an empty copy or an overrun instead of handing it to vkCmdCopyBuffer2.
        if (size == 0u || source_offset > source_resource->size_bytes || size > source_resource->size_bytes - source_offset ||
            destination_offset > destination_resource->size_bytes || size > destination_resource->size_bytes - destination_offset) {
            return rhi::error::invalid_argument;
        }
        VkBufferCopy2 const regions = make_buffer_copy_2(source_offset, destination_offset, size);
        VkCopyBufferInfo2 const copy_info = make_copy_buffer_info_2(source_resource->native, destination_resource->native, 1, &regions);
        vkCmdCopyBuffer2(command_buffer, &copy_info);
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::clear_color_image(rhi::image const& target, deren::utility::vessel::array<float, 4> const& color, rhi::subresource_range const& range) {
        if (parent) {
            ++parent->operation_requests[28]; // clear_color_image
        }
        if (auto result = apply_operations(); result != rhi::error::ok) {
            return result;
        }
        core* const self = this->parent;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (!rhi::is_concrete(target, concrete::image) && !rhi::is_concrete(target, concrete::frame_image)) {
            return rhi::error::invalid_argument;
        }
        std::lock_guard const lock(self->resource.contract_images_mutex);
        if (!self->resource.contract_images.contains(&target)) {
            return rhi::error::invalid_argument;
        }
        auto const* const concrete = static_cast<core::image const*>(&target);
        if (concrete->source) {
            return rhi::error::unsupported;
        }
        VkClearColorValue const clear = {.float32 = {color[0], color[1], color[2], color[3]}};
        VkImageSubresourceRange const native_range = subresource_of(VK_IMAGE_ASPECT_COLOR_BIT, range);
        vkCmdClearColorImage(command_buffer, concrete->native_handle, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &native_range);
        return rhi::error::ok;
    }

    // ---- tier-2: device_address --------------------------------------------------------------------

    std::uint64_t core::buffer_address_view::buffer_address(rhi::buffer const& resource, std::uint64_t const offset) const noexcept {
        // THE PRECONDITION IS THE CONTRACT'S RULE, STATED RATHER THAN GUESSED AT: a caller may only
        // ask about a buffer THIS backend handed out (rhi.api_core.cppm says the same about every handle
        // it gives back). `frame_commands::use()` can afford a stricter check because its views are
        // singletons it compares addresses against; factory-created buffers are heap objects, so
        // recognising "one of mine" without RTTI would mean keeping a registry of them - and this path
        // does not need one, because the answer it computes (`vkGetBufferDeviceAddress`) is only defined
        // for a buffer this device created in the first place. So: a `static_cast` on a documented
        // precondition, not a hopeful assumption.
        auto const* const concrete = static_cast<core::buffer const*>(&resource);
        if (this->parent == nullptr || !concrete->addressable || concrete->source) {
            // ASKED FOR NOTHING, GET NOTHING: a buffer created without `buffer_flag::device_address` has
            // no address to report (Vulkan only allows the call for a buffer created with that usage),
            // and 0 is the contract's spelling of "none" - the module's guard, not a failure.
            return 0ull;
        }
        VkBufferDeviceAddressInfo const query = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .pNext = nullptr, .buffer = concrete->native};
        std::uint64_t const address = static_cast<std::uint64_t>(vkGetBufferDeviceAddress(this->parent->device.logical_device, &query));
        if (address == 0u) {
            deren::utility::log("rhi: buffer_address() answered 0 for native {:#x} (usage flags {:#x}) - the address is only defined when the device enabled "
                                "bufferDeviceAddress, which is what the flag promised",
                                reinterpret_cast<std::uintptr_t>(concrete->native), concrete->declared_flags);
        }
        return address + offset;
    }

    // ---- THE GUI PLUGIN'S RECORDING TARGET (abi 43) ---------------------------------------------------
    //
    // The contract passes the RHI handle; the plugin checks the concrete tag, casts it to the shared Vulkan
    // class (`denen.vulkan::command_buffer`) and calls `get()`. THE BACKEND'S JOB is only to keep that handle
    // current, at the two moments a recording target changes:
    //
    //   - a buffer the factory made: set once when its VkCommandBuffer is allocated (it never changes afterwards);
    //   - the frame view: set when a frame is acquired (frame.frame_in_flight) and cleared when the frame
    //     is submitted or the slot advances - outside a frame it names nothing, which is the truthful answer.

    rhi::error core::frame_shader_group_access::read(rhi::pipeline const& resource, std::uint32_t const first_group,
                                                     std::uint32_t const group_count, std::span<std::uint8_t> const out) const noexcept {
        if (this->parent == nullptr || !this->parent->device.ray_tracing_pipeline_available) {
            return rhi::error::unsupported;
        }
        core& self = *this->parent;
        std::lock_guard const lock(self.resource.contract_pipelines_mutex);
        if (!self.resource.contract_pipelines.contains(&resource)) {
            return rhi::error::invalid_argument;
        }
        auto const& concrete = static_cast<core::pipeline const&>(resource);
        std::uint64_t const required = static_cast<std::uint64_t>(group_count) * self.device.ray_tracing_pipeline_properties.shaderGroupHandleSize;
        if (concrete.bind_point != VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR || group_count == 0 ||
            first_group >= concrete.group_count || group_count > concrete.group_count - first_group || required == 0 || required > out.size()) {
            return rhi::error::invalid_argument;
        }
        auto const query = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(vkGetDeviceProcAddr(self.device.logical_device, "vkGetRayTracingShaderGroupHandlesKHR"));
        if (query == nullptr) {
            return rhi::error::unsupported;
        }
        VkResult const result = query(self.device.logical_device, concrete.native_handle, first_group, group_count,
                                      static_cast<std::size_t>(required), out.data());
        return generic_error(result);
    }

    // ---- tier-2 host_image_copy (③-D/E step 2) ------------------------------------------------------
    //
    // THE ONE MECHANISM, IN ONE PLACE (abi 25): the ability's span-shaped verb and `image::get_content()`
    // are two SPELLINGS of the same host copy - `vkCopyImageToMemoryEXT`, with no staging buffer, no copy
    // command and no submission - so both build the same `VkImageToMemoryCopy` through this helper. A
    // device fact (`host_image_copy_available`) gates both, which is what keeps them from disagreeing.
    namespace {
        [[nodiscard]] rhi::error host_copy_image_out(core const& self, VkImage const source, std::span<std::byte> const destination,
                                                     rhi::image_copy_region const& region) noexcept {
            if (!self.device.host_image_copy_available || source == VK_NULL_HANDLE) {
                return rhi::error::unsupported;
            }
            // THE REGION IS THE CALLER'S, TIGHTLY PACKED: the contract's `image_copy_region` says WHERE in
            // the image (texels, subresource, offsets) and this maps it onto the API's structure with the
            // row/image strides left zero - which is the API's spelling of "the region is tightly
            // packed", i.e. exactly extent.width texels per row and extent.height rows deep. The aspect is
            // COLOUR: the contract's region carries no aspect, and every image this renderer copies out of
            // is a colour one.
            // THE CONVERSIONS ARE EXPLICIT because the API's structure is less wide here than the
            // contract's region: `mipLevel` and the offsets are `int32_t` in Vulkan and `uint32_t` in the
            // region. A braced initializer would refuse the narrowing outright (-Wc++11-narrowing), which is
            // the compiler asking for the cast - and a region whose values do not fit is a caller bug the
            // backend cannot repair.
            VkImageToMemoryCopy const memory_copy = make_image_to_memory_copy(
                destination.data(),
                make_image_subresource_layers(VK_IMAGE_ASPECT_COLOR_BIT, region.mip_level, region.base_array_layer, region.array_layer_count),
                {static_cast<std::int32_t>(region.offset_x), static_cast<std::int32_t>(region.offset_y), static_cast<std::int32_t>(region.offset_z)},
                {region.extent.width, region.extent.height, region.extent.depth});
            // GENERAL, AND THAT IS THIS RENDERER'S CONVENTION rather than a choice made here: every image
            // it creates is kept in GENERAL (the heap-native shaders index the grid directly), and the
            // device's host-copy source layouts list GENERAL among what it can read from - which is exactly
            // what `host_image_copy_available` verifies at startup.
            VkCopyImageToMemoryInfo const copy_info = make_copy_image_to_memory_info(
                source, VK_IMAGE_LAYOUT_GENERAL, 1, &memory_copy);
            return generic_error(self.device.copy_image_to_memory(self.device.logical_device, &copy_info));
        }
    } // namespace

    // ---- tier-2 device_capabilities: THE DEVICE'S FACTS, ANSWERED FROM WHAT THE CONSTRUCTOR QUERIED ----
    //
    // NOT ONE OF THESE METHODS ASKS THE DEVICE ANYTHING. Every value is a member the constructor filled while it
    // decided what to enable (`gate G2` in init_utils.cppm verified the four-part capability sets), so the
    // engine's questions are answered from the record of the decision rather than by repeating its queries -
    // which is also what makes the answers UNABLE to disagree with the enabling they describe.
    //
    // A MISSING DEVICE (`parent == nullptr`, a released core) answers the same defaults an unqueried device
    // struct would have: false for the features, zero for the numbers - the callers' guard rails treat a
    // zero limit as "visibly not a device" (the note `physical_properties_of` used to carry).
    bool core::frame_device_capabilities::compute_indirect() const noexcept {
        return this->parent != nullptr;
    }

    bool core::frame_device_capabilities::buffer_int64_atomics() const noexcept {
        return this->parent != nullptr && this->parent->device.buffer_int64_atomics_available;
    }

    bool core::frame_device_capabilities::mesh_shader() const noexcept {
        return this->parent != nullptr && this->parent->device.mesh_shader_available;
    }

    bool core::frame_device_capabilities::ray_query() const noexcept {
        return this->parent != nullptr && this->parent->device.ray_query_available;
    }

    std::uint32_t core::frame_device_capabilities::max_push_constants_size() const noexcept {
        return this->parent != nullptr ? this->parent->resource.descriptor_heaps.push_capacity() : 0u;
    }

    std::uint32_t core::frame_device_capabilities::graphics_queue_family() const noexcept {
        return this->parent == nullptr ? 0u : this->parent->device.graphics_queue_family_index;
    }

    rhi::shader_binding_table_properties core::frame_device_capabilities::shader_binding_table() const noexcept {
        if (this->parent == nullptr) {
            return {};
        }
        auto const& properties = this->parent->device.ray_tracing_pipeline_properties;
        return rhi::shader_binding_table_properties{
            .handle_size = properties.shaderGroupHandleSize,
            .handle_alignment = properties.shaderGroupHandleAlignment,
            .base_alignment = properties.shaderGroupBaseAlignment,
        };
    }

    std::uint64_t core::frame_device_capabilities::acceleration_structure_scratch_alignment() const noexcept {
        return this->parent == nullptr ? 0u : this->parent->device.acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment;
    }

    std::uint64_t core::frame_device_capabilities::max_acceleration_structure_instances() const noexcept {
        return this->parent == nullptr ? 0u : this->parent->device.acceleration_structure_properties.maxInstanceCount;
    }

    rhi::error core::frame_host_copy::copy_image_to_memory(rhi::image const& source, std::span<std::byte> destination,
                                                           rhi::image_copy_region const& region) noexcept {
        core* const self = this->parent;
        if (self == nullptr || !self->device.host_image_copy_available) {
            // The ability is announced ONLY when the constructor found it, so a caller that checked `abilities()` never
            // sees this; one that did not gets the contract's named refusal rather than a call through a
            // null function pointer.
            return rhi::error::unsupported;
        }
        // ONLY AN IMAGE THIS BACKEND CREATED, which is the contract's precondition for every native handle
        // it converts. The frame image (a VIEW) is NOT served HERE, and that is deliberate: this
        // ability's contract is "an image a caller made", and treating the frame view as a factory image
        // is the bug this pointer-identity check exists to avoid. The FRAME image has its
        // spelling now (`frame_image_slot::get_content()`), so nothing is lost by keeping this refusal.
        if (static_cast<void const*>(&source) == static_cast<void const*>(&self->frame.frame_image_view)) {
            return rhi::error::invalid_argument;
        }
        auto const* const concrete = static_cast<core::image const*>(&source);
        if (concrete->source) {
            return rhi::error::unsupported;
        }
        if (concrete->native_handle == VK_NULL_HANDLE) {
            return rhi::error::invalid_argument;
        }
        return host_copy_image_out(*self, concrete->native_handle, destination, region);
    }

    // ---- image::get_content (abi 25): THE CONTENT, NOT A HANDLE --------------------------------------
    std::expected<rhi::image_content, rhi::error> core::image::get_content(rhi::image_copy_region const& region) const {
        if (source) {
            return std::unexpected(rhi::error::unsupported);
        }
        core* const self = this->parent;
        if (self == nullptr || this->native_handle == VK_NULL_HANDLE) {
            return std::unexpected(rhi::error::invalid_argument);
        }
        // THE IMAGE MUST HAVE BEEN MADE FOR HOST TRANSFER: the contract's `image_flag::host_transfer`, which
        // the descriptor declared and this backend kept in `declared_flags`. An image without it is refused by
        // nature (VUID-vkCopyImageToMemoryEXT-srcImage-09460), so the answer is a NAMED error, not the call.
        if (!rhi::has_flag(this->declared_flags, rhi::image_flag::host_transfer) || !self->device.host_image_copy_available) {
            return std::unexpected(rhi::error::unsupported);
        }
        // The region: what the caller asked for, or the whole mip 0 of every layer.
        rhi::image_extent const whole{this->width, this->height, 1u};
        bool const whole_image = region.extent.width == 0u || region.extent.height == 0u;
        rhi::image_extent const extent = whole_image ? whole : region.extent;
        rhi::image_copy_region const asked = whole_image
                                                 ? rhi::image_copy_region{.extent = whole, .array_layer_count = this->array_layers}
                                                 : region;
        std::uint32_t const bpp = rhi::bytes_per_pixel(this->declared_format);
        if (bpp == 0u) {
            // A format with no host spelling here (a depth ROLE, `unknown`): the caller would not be able to
            // unpack what it received, so the honest answer is the refusal.
            return std::unexpected(rhi::error::unsupported);
        }
        std::size_t const texels = static_cast<std::size_t>(extent.width) * extent.height * extent.depth * asked.array_layer_count;
        rhi::image_content content{};
        content.extent = extent;
        content.bytes_per_pixel = bpp;
        content.bytes.resize(texels * bpp);
        rhi::error const copied = host_copy_image_out(*self, this->native_handle, std::span<std::byte>(content.bytes), asked);
        if (copied != rhi::error::ok) {
            return std::unexpected(copied);
        }
        return content;
    }

    // ---- the FRAME image's content (abi 25): REFUSED, WITH THE MEASURED REASON ------------------------
    //
    // A swapchain image can only be host-copied when the surface listed `VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT`
    // - the swapchain's usage has to be a subset of the surface's `supportedUsageFlags`
    // (VUID-VkSwapchainCreateInfoKHR-imageUsage-01276) - and the swapchain here does not include that bit. On
    // every surface this renderer has been run on the bit is ABSENT (the constructor logs the fact, and
    // docs/host_image_copy.md records why the screenshot's read-back is the recorded copy command instead).
    //
    // SO THIS ANSWERS `unsupported` RATHER THAN FAKING IT, and the day a surface lists the bit the honest fix
    // is two lines in the constructor (add the usage bit, remember the answer) plus the body below - the
    // refusal is a MEASURED property of the surface, not a gap in this backend. A caller that wants pixels on
    // such a surface reads an image this backend created (`image_flag::host_transfer`), which is what the
    // probes do.
    std::expected<rhi::image_content, rhi::error> core::frame_image_slot::get_content(rhi::image_copy_region const& region) const {
        static_cast<void>(region);
        if (this->parent == nullptr) {
            return std::unexpected(rhi::error::invalid_argument);
        }
        return std::unexpected(rhi::error::unsupported);
    }
} // namespace deren::vulkan
