// -*- C++ -*-
// ============================================================================
// file: vulkan/core/core.api_core.cpp
//
// THE RHI CONTRACT'S VIRTUALS, DEFINED FOR THE REAL BACKEND (plan_rhi_v4.md §11.4, S1-C; the
// recording surface, the escape and the read-back slot are S2 batch 2).
// `core` derives from `deren::promise::rhi::api_core` (vulkan/core/core.declarations.cppm), so the
// object that owns the instance / device / swapchain IS the object a host gets from
// deren_make_api_core() - there is no wrapper type to keep in step with it.
//
// WHAT IS REAL HERE, AND WHAT IS DELIBERATELY NOT:
//
//   - `abilities()`: A BIT MEANS THE CONTRACT CAN SERVE IT, not that the device has the feature. For
//     every set bit, `query_extension(kind)` has to answer with an object that can carry out every
//     operation that ability declares, on the objects this backend can produce. THIS BACKEND REPORTS
//     `vulkan_escape` AND NOTHING ELSE: every handle the escape hands out is a member that already
//     exists, so the bit is servable in the strict sense. `device_address` and `host_image_copy` are
//     still NOT reported - both need a producible `buffer`/`image`, and the factories still answer
//     nullptr because their descriptors are S3.
//   - `query_extension()`: the escape object for `vulkan_escape`, nullptr for everything else -
//     exactly the two-way invariant the startup gate (core.constructor.cppm, G2) and
//     tests/test_dynamic_link.cpp (G1) check.
//   - the factories: nullptr. Their descriptors are still forward-declared (the resource model is
//     §6.4/S3), so building them here would mean inventing S3. A factory that cannot honour a
//     descriptor answers nullptr (§4.2: no throwing path across the boundary).
//   - the recording surface and the frame views: REAL, and they are the only `rhi` handles that can
//     exist while the factories answer nullptr - borrowed views of objects this class already owns
//     (the frame's primary command buffer, the swapchain image the frame draws into, the read-back
//     slot). See the view types in core.declarations.cppm.
//   - the shadow gate (plan §8.3, gate A5): `use()` derives the barrier from the contract's role pair
//     and the asserts right below prove - field by field, AT COMPILE TIME - that it lands exactly on
//     the two recipes this renderer shipped by hand. The same dump is emitted once per pair at run
//     time, so the log carries both sides.
//   - the frame calls: the machinery this file's class already had - `wait_frame_slot`,
//     `image_available_semaphores`, `submit()`, `present()`, `wait_idle()`. `frame_begin()` also
//     performs the acquire, which is the one piece that used to live in the runtime
//     (runtime.frames.cppm:115): the device and the swapchain are the backend's, so the acquire is
//     the backend's too.
// ============================================================================
module;

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <memory> // std::shared_ptr: make_command_buffer's control block
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

module deren.vulkan.core;

import deren.promise.rhi;

import deren.vulkan.constant_init;
import deren.vulkan.core.pipeline;

// After the imports it needs: the header names deren::promise::rhi types and declares the translators.

namespace deren::vulkan {

    namespace rhi = deren::promise::rhi;

    namespace {

        /// ONE ROW OF THE DECLARATION TABLE: which pair, and the fields the pair's barrier carries.
        ///
        /// THE FIELDS ARE COPIED FROM THE RENDERER'S OWN RECIPE CONSTANT, never transcribed - see
        /// `pair_of` below. Transcribing a mask into this table would create a SECOND truth beside
        /// `vulkan/constant_init/constant_init.cppm`, and the whole point of this face is that the pair
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
        constexpr std::array image_use_pairs = {
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
            // PLAN X4: the host-access role the census said the contract had none of. The probe reads its own
            // render back through `image::get_content()`, and this is the pair that says so.
            pair_of(rhi::image_use::color_attachment, rhi::image_use::host_read, deren::vulkan::color_attachment_to_host_transition),
            // THE DEPENDENCY (no layout change, store -> load inside one frame): see the note above
            pair_of(rhi::image_use::color_attachment, rhi::image_use::color_attachment, deren::vulkan::color_attachment_dependency),
            // ---- the shader-written family ----
            pair_of(rhi::image_use::shader_write, rhi::image_use::shader_read, deren::vulkan::general_to_sampling_transition),
            pair_of(rhi::image_use::shader_write, rhi::image_use::transfer_source, deren::vulkan::general_to_transfer_src_transition),
            // a read-modify-write self barrier: the destination needs BOTH access bits, which is why the
            // pair carries its own role (`shader_read_write`) instead of one of its halves
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
        // `transfer_src_to_sampling_transition`, which `vulkan/pass/megalights_temporal.cpp` uses). A green
        // assertion was certifying a WRONG NUMBER, which is worse than no assertion: it made the gap look
        // covered. The numbers below are the histogram output of `scripts/recording_face_census.py`, quoted
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
                      "(scripts/recording_face_census.py: 'measured pairs: 22'); a hand count is not evidence");
        static_assert(image_use_pair_count_from_census + unsupported_pair_count_from_census ==
                          image_use_value_count * image_use_value_count,
                      "22 measured pairs + 99 unsupported must be exactly the 121 role combinations the census "
                      "reports - if the enum grows, both numbers are re-read from the same run");

        /// The barrier one (from, to) role pair needs: the TABLE's row, with the caller's image filled in.
        ///
        /// THE DERIVATION FROM THE PAIR IS THE POINT of the shadow gate: it is spelled here in the
        /// contract's vocabulary (two roles), and the table's rows come from the renderer's own recipes -
        /// so a pair that is transcribed cannot drift from the recipe it was measured from, and the
        /// asserts below keep proving it field for field. A pair NO ROW CARRIES has no stages and no
        /// accesses at all, which is the honest answer for a transition nobody has defined yet -
        /// `use()` reports it once instead of recording nonsense.
        [[nodiscard]] constexpr VkImageMemoryBarrier2 barrier_for(rhi::image_use const from, rhi::image_use const to) noexcept {
            VkImageMemoryBarrier2 barrier = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = 0,
                .srcAccessMask = 0,
                .dstStageMask = 0,
                .dstAccessMask = 0,
                // EVERY IMAGE IN THIS RENDERER LIVES IN GENERAL: the layouts are an invariant, not a
                // parameter (docs/unified_image_layouts.md), which is exactly why the contract carries
                // no layout. The rows that differ (UNDEFINED as `from`, PRESENT_SRC as `to`) override it.
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = VK_NULL_HANDLE,
                // The whole image, one mip, one layer: what the screenshot read-back copies, and what
                // the hand-written recipes carry.
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
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
        //     * `std::array<image_use_pair, 21>` with twenty initializers VALUE-INITIALISES a twenty-first
        //       (dummy `undefined -> undefined`) row, so `size() == 21` still held;
        //     * `unsupported_reason` short-circuited on the table, so the check that was supposed to name
        //       the offending pair reduced to `in_table == !in_table` - a tautology, never false.
        // A substituted row therefore kept every number intact. Three rules follow, and they are now the
        // ACCEPTANCE STANDARD for this gate:
        //     * THE SET IS ASSERTED, NOT ONLY THE COUNT (a substituted row preserves any count);
        //     * THE TWO PREDICATES MUST NOT SHORT-CIRCUIT EACH OTHER (or the partition check is vacuous);
        //     * THE GATE MUST BE ABLE TO FAIL, and that is proven by MUTATION, not by reading the code.
        // ============================================================================================

        /// THE PAIRS THE CENSUS MEASURED - transcribed from `scripts/recording_face_census.py`'s output
        /// (the `measured pairs` line, printed as role pairs). This is the INDEPENDENT side of the set
        /// equality below: the table is checked against the measurement, never against itself.
        struct role_pair_expected {
            rhi::image_use from;
            rhi::image_use to;
        };
        constexpr std::array expected_pairs_from_census = {
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
                      "(scripts/recording_face_census.py: 'unsupported by this measurement: 79')");

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
        static_assert(table_rows_are_unique(), "the table must not carry the same pair twice - a duplicate is "
                                               "how a value-initialised (dummy) row silently replaced a real one");

        /// Set equality, both directions: the sizes agree (declared below) and no measured pair is absent.
        [[nodiscard]] consteval bool every_expected_pair_is_in_the_table() noexcept {
            for (role_pair_expected const& row : expected_pairs_from_census) {
                if (!image_use_pair_is_in_table(row.from, row.to)) {
                    return false;
                }
            }
            return true;
        }
        static_assert(every_expected_pair_is_in_the_table(),
                      "every pair the census measured must be a table row - a missing row is a gap, and this "
                      "assert exists because the first version of the gate stayed GREEN while one was gone");
        // THE SIZE IS DEDUCED (CTAD above), so a deleted row cannot hide behind a declared size.
        static_assert(image_use_pairs.size() == expected_pairs_from_census.size(),
                      "the table and the census must have the same number of pairs (21)");

        /// THE FIRST COMBINATION THAT IS NEITHER IN THE TABLE NOR UNDER A RULE, ENCODED as from * 10 + to
        /// (100 = every combination is accounted for). NOTE the shape: the two sides are INDEPENDENT
        /// predicates, so "in the table AND under a rule" (a dummy row) is flagged just as loudly as
        /// "neither" (a deleted row) - the first version compared a predicate with its own negation.
        [[nodiscard]] consteval std::uint32_t first_uncovered_pair() noexcept {
            for (std::uint32_t from = 0; from < image_use_value_count; ++from) {
                for (std::uint32_t to = 0; to < image_use_value_count; ++to) {
                    auto const f = static_cast<rhi::image_use>(from);
                    auto const t = static_cast<rhi::image_use>(to);
                    if (image_use_pair_is_in_table(f, t) == (image_use_pair_rule(f, t) != nullptr)) {
                        return from * image_use_value_count + to;
                    }
                }
            }
            return image_use_value_count * image_use_value_count;
        }
        template <std::uint32_t EncodedPair>
        struct image_use_pair_must_be_in_the_table_or_under_a_rule;
        /// The complete case: every combination is accounted for. Empty ON PURPOSE - a member nothing reads
        /// is a `-Wunused-const-variable` error in this build.
        template <>
        struct image_use_pair_must_be_in_the_table_or_under_a_rule<100> {};
        // THE GATE, and it can FAIL: the alias instantiates a DELIBERATELY INCOMPLETE template carrying the
        // encoded offending pair, so a mutation produces a diagnostic naming it (proven by mutation).
        using image_use_coverage_gate = image_use_pair_must_be_in_the_table_or_under_a_rule<first_uncovered_pair()>;

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
        // the pilot's own transitions)
        constexpr VkImageMemoryBarrier2 derived_undefined_to_attachment = barrier_for(rhi::image_use::undefined, rhi::image_use::color_attachment);
        constexpr VkImageMemoryBarrier2 derived_attachment_to_sampling = barrier_for(rhi::image_use::color_attachment, rhi::image_use::shader_read);
        static_assert(same_barrier(derived_undefined_to_attachment, deren::vulkan::color_attachment_transition),
                      "A5: use(undefined, color_attachment) must land field-for-field on color_attachment_transition");
        static_assert(same_barrier(derived_attachment_to_sampling, deren::vulkan::hdr_sampling_transition),
                      "A5: use(color_attachment, shader_read) must land field-for-field on hdr_sampling_transition");

        /// every field of one barrier on one line, for the shadow gate's run-time dump (A5 asks for
        /// BOTH sides in the log, not just for "equal or not")
        [[nodiscard]] std::string barrier_text(VkImageMemoryBarrier2 const& barrier) {
            return std::format("sType={:#x} pNext={:#x} srcStage={:#x} srcAccess={:#x} dstStage={:#x} dstAccess={:#x} oldLayout={:#x} newLayout={:#x} "
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

        /// the caller's buffer descriptor with every field outside the bytes it declared left at THIS
        /// build's default
        deren::promise::rhi::buffer_desc sanitize_buffer_desc(deren::promise::rhi::buffer_desc const& desc) {
            using rhi_buffer_desc = deren::promise::rhi::buffer_desc;

            uint32_t const declared = desc.struct_size;
            uint32_t const known = static_cast<uint32_t>(sizeof(rhi_buffer_desc));
            if (declared != known) {
                deren::utility::log("core: the RHI buffer_desc is {} B here and {} B in the caller -> the caller's structure is treated as TOO SHORT: "
                                    "a field whose whole extent is not inside those {} B keeps this build's default",
                                    known, declared, declared);
            }

            rhi_buffer_desc options = {};
            if (covered_by(declared, offsetof(rhi_buffer_desc, size), sizeof(rhi_buffer_desc::size))) {
                options.size = desc.size;
            }
            if (covered_by(declared, offsetof(rhi_buffer_desc, usage), sizeof(rhi_buffer_desc::usage))) {
                options.usage = desc.usage;
            }
            if (covered_by(declared, offsetof(rhi_buffer_desc, flags), sizeof(rhi_buffer_desc::flags))) {
                options.flags = desc.flags;
            }
            if (covered_by(declared, offsetof(rhi_buffer_desc, initial_bytes), sizeof(rhi_buffer_desc::initial_bytes))) {
                options.initial_bytes = desc.initial_bytes;
            }
            return options;
        }

        // ---- ABI 7'S IMAGE FACE: THE CONTRACT'S VOCABULARY IN THIS BACKEND'S ------------------------
        // The same one-to-one, written-out mapping rule create_buffer uses: a new contract value must
        // not silently become whatever the integer happens to mean here.

        /// the contract's format name in this backend's spelling; UNDEFINED means "refuse" (unknown),
        /// and the `depth` ROLE resolves to the device's own depth attachment format - which concrete
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
            case rhi_image_format::depth:
                return depth_format;
            case rhi_image_format::unknown:
                return VK_FORMAT_UNDEFINED;
            }
            return VK_FORMAT_UNDEFINED;
        }

        /// the contract's capability flags as the usage bits they LET THE CALLER DO.
        [[nodiscard]] VkImageUsageFlags native_image_usage(rhi::image_flags const flags) noexcept {
            namespace rf = rhi;
            VkImageUsageFlags usage = 0;
            if (rf::has_flag(flags, rf::image_flag::sampled)) {
                usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
            }
            if (rf::has_flag(flags, rf::image_flag::storage)) {
                usage |= VK_IMAGE_USAGE_STORAGE_BIT;
            }
            if (rf::has_flag(flags, rf::image_flag::color_attachment)) {
                usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            }
            if (rf::has_flag(flags, rf::image_flag::depth_attachment)) {
                usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            }
            if (rf::has_flag(flags, rf::image_flag::transfer_source)) {
                usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            }
            if (rf::has_flag(flags, rf::image_flag::transfer_destination)) {
                usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            }
            if (rf::has_flag(flags, rf::image_flag::host_transfer)) {
                usage |= VK_IMAGE_USAGE_HOST_TRANSFER_BIT_EXT;
            }
            if (rf::has_flag(flags, rf::image_flag::cube_compatible)) {
                // no usage bit: the flag's other half is the ALLOCATOR's type (see create_image), which
                // is what makes the six layers cube-creatable at all
            }
            return usage;
        }

        /// the same ABI guard `sanitize_buffer_desc` runs, field by field for `image_desc`.
        [[nodiscard]] deren::promise::rhi::image_desc sanitize_image_desc(deren::promise::rhi::image_desc const& desc) {
            using rhi_image_desc = deren::promise::rhi::image_desc;

            uint32_t const declared = desc.struct_size;
            uint32_t const known = static_cast<uint32_t>(sizeof(rhi_image_desc));
            if (declared != known) {
                deren::utility::log("core: the RHI image_desc is {} B here and {} B in the caller -> only the caller's declared prefix is read",
                                    known, declared);
            }

            rhi_image_desc options = {};
            if (covered_by(declared, offsetof(rhi_image_desc, extent), sizeof(rhi_image_desc::extent))) {
                options.extent = desc.extent;
            }
            if (covered_by(declared, offsetof(rhi_image_desc, mip_levels), sizeof(rhi_image_desc::mip_levels))) {
                options.mip_levels = desc.mip_levels;
            }
            if (covered_by(declared, offsetof(rhi_image_desc, array_layers), sizeof(rhi_image_desc::array_layers))) {
                options.array_layers = desc.array_layers;
            }
            if (covered_by(declared, offsetof(rhi_image_desc, format), sizeof(rhi_image_desc::format))) {
                options.format = desc.format;
            }
            if (covered_by(declared, offsetof(rhi_image_desc, flags), sizeof(rhi_image_desc::flags))) {
                options.flags = desc.flags;
            }
            if (covered_by(declared, offsetof(rhi_image_desc, initial_bytes), sizeof(rhi_image_desc::initial_bytes))) {
                options.initial_bytes = desc.initial_bytes;
            }
            if (covered_by(declared, offsetof(rhi_image_desc, debug_name), sizeof(rhi_image_desc::debug_name))) {
                options.debug_name = desc.debug_name;
            }
            return options;
        }

        /// the same ABI guard for `image_view_desc`.
        [[nodiscard]] deren::promise::rhi::image_view_desc sanitize_image_view_desc(deren::promise::rhi::image_view_desc const& desc) {
            using rhi_view_desc = deren::promise::rhi::image_view_desc;

            uint32_t const declared = desc.struct_size;
            uint32_t const known = static_cast<uint32_t>(sizeof(rhi_view_desc));
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

        /// the same ABI guard for `shader_desc`.
        [[nodiscard]] deren::promise::rhi::shader_desc sanitize_shader_desc(deren::promise::rhi::shader_desc const& desc) {
            using rhi_shader_desc = deren::promise::rhi::shader_desc;

            uint32_t const declared = desc.struct_size;
            uint32_t const known = static_cast<uint32_t>(sizeof(rhi_shader_desc));
            if (declared != known) {
                deren::utility::log("core: the RHI shader_desc is {} B here and {} B in the caller -> only the caller's declared prefix is read",
                                    known, declared);
            }

            rhi_shader_desc options = {};
            if (covered_by(declared, offsetof(rhi_shader_desc, stage), sizeof(rhi_shader_desc::stage))) {
                options.stage = desc.stage;
            }
            if (covered_by(declared, offsetof(rhi_shader_desc, code), sizeof(rhi_shader_desc::code))) {
                options.code = desc.code;
            }
            if (covered_by(declared, offsetof(rhi_shader_desc, debug_name), sizeof(rhi_shader_desc::debug_name))) {
                options.debug_name = desc.debug_name;
            }
            return options;
        }

        /// the same ABI guard for `pipeline_desc`.
        [[nodiscard]] deren::promise::rhi::pipeline_desc sanitize_pipeline_desc(deren::promise::rhi::pipeline_desc const& desc) {
            using rhi_pipeline_desc = deren::promise::rhi::pipeline_desc;

            uint32_t const declared = desc.struct_size;
            uint32_t const known = static_cast<uint32_t>(sizeof(rhi_pipeline_desc));
            if (declared != known) {
                deren::utility::log("core: the RHI pipeline_desc is {} B here and {} B in the caller -> only the caller's declared prefix is read",
                                    known, declared);
            }

            rhi_pipeline_desc options = {};
            if (covered_by(declared, offsetof(rhi_pipeline_desc, color_formats), sizeof(rhi_pipeline_desc::color_formats))) {
                options.color_formats = desc.color_formats;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, depth_format), sizeof(rhi_pipeline_desc::depth_format))) {
                options.depth_format = desc.depth_format;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, vertex_code), sizeof(rhi_pipeline_desc::vertex_code))) {
                options.vertex_code = desc.vertex_code;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, fragment_code), sizeof(rhi_pipeline_desc::fragment_code))) {
                options.fragment_code = desc.fragment_code;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, first_stage), sizeof(rhi_pipeline_desc::first_stage))) {
                options.first_stage = desc.first_stage;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, sample_count), sizeof(rhi_pipeline_desc::sample_count))) {
                options.sample_count = desc.sample_count;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, depth_test), sizeof(rhi_pipeline_desc::depth_test))) {
                options.depth_test = desc.depth_test;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, depth_bias_constant_factor), sizeof(rhi_pipeline_desc::depth_bias_constant_factor))) {
                options.depth_bias_constant_factor = desc.depth_bias_constant_factor;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, depth_bias_slope_factor), sizeof(rhi_pipeline_desc::depth_bias_slope_factor))) {
                options.depth_bias_slope_factor = desc.depth_bias_slope_factor;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, depth_bias_clamp), sizeof(rhi_pipeline_desc::depth_bias_clamp))) {
                options.depth_bias_clamp = desc.depth_bias_clamp;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, blend_modes), sizeof(rhi_pipeline_desc::blend_modes))) {
                options.blend_modes = desc.blend_modes;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, compare), sizeof(rhi_pipeline_desc::compare))) {
                options.compare = desc.compare;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, debug_name), sizeof(rhi_pipeline_desc::debug_name))) {
                options.debug_name = desc.debug_name;
            }
            // THE COMPUTE SPELLING (appended): a caller that declares only the graphics prefix keeps an EMPTY
            // `compute_code`, which the compute branch below refuses by name - an older caller cannot ask for a
            // compute pipeline by accident, and a newer one is read exactly as far as it declared.
            if (covered_by(declared, offsetof(rhi_pipeline_desc, compute_code), sizeof(rhi_pipeline_desc::compute_code))) {
                options.compute_code = desc.compute_code;
            }
            // THE RAY-TRACING SPELLING (appended with it): same rule - an older caller's prefix keeps the three
            // fields empty, which is "not a ray-tracing pipeline" rather than a mis-read.
            if (covered_by(declared, offsetof(rhi_pipeline_desc, ray_tracing_stages), sizeof(rhi_pipeline_desc::ray_tracing_stages))) {
                options.ray_tracing_stages = desc.ray_tracing_stages;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, ray_tracing_groups), sizeof(rhi_pipeline_desc::ray_tracing_groups))) {
                options.ray_tracing_groups = desc.ray_tracing_groups;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, max_ray_recursion), sizeof(rhi_pipeline_desc::max_ray_recursion))) {
                options.max_ray_recursion = desc.max_ray_recursion;
            }
            if (covered_by(declared, offsetof(rhi_pipeline_desc, acceleration_structure_bindings), sizeof(rhi_pipeline_desc::acceleration_structure_bindings))) {
                options.acceleration_structure_bindings = desc.acceleration_structure_bindings;
            }
            return options;
        }

        /// the same ABI guard for `sampler_desc`.
        [[nodiscard]] deren::promise::rhi::sampler_desc sanitize_sampler_desc(deren::promise::rhi::sampler_desc const& desc) {
            using rhi_sampler_desc = deren::promise::rhi::sampler_desc;

            uint32_t const declared = desc.struct_size;
            uint32_t const known = static_cast<uint32_t>(sizeof(rhi_sampler_desc));
            if (declared != known) {
                deren::utility::log("core: the RHI sampler_desc is {} B here and {} B in the caller -> only the caller's declared prefix is read",
                                    known, declared);
            }

            rhi_sampler_desc options = {};
            if (covered_by(declared, offsetof(rhi_sampler_desc, address_mode), sizeof(rhi_sampler_desc::address_mode))) {
                options.address_mode = desc.address_mode;
            }
            if (covered_by(declared, offsetof(rhi_sampler_desc, max_lod), sizeof(rhi_sampler_desc::max_lod))) {
                options.max_lod = desc.max_lod;
            }
            // abi 16's APPENDED fields, read the same way: the default-constructed `options` carries today's
            // defaults (linear / linear / linear, no comparison), so a caller built against the previous
            // revision keeps exactly the behaviour it compiled against.
            if (covered_by(declared, offsetof(rhi_sampler_desc, mag_filter), sizeof(rhi_sampler_desc::mag_filter))) {
                options.mag_filter = desc.mag_filter;
            }
            if (covered_by(declared, offsetof(rhi_sampler_desc, min_filter), sizeof(rhi_sampler_desc::min_filter))) {
                options.min_filter = desc.min_filter;
            }
            if (covered_by(declared, offsetof(rhi_sampler_desc, mipmap_mode), sizeof(rhi_sampler_desc::mipmap_mode))) {
                options.mipmap_mode = desc.mipmap_mode;
            }
            if (covered_by(declared, offsetof(rhi_sampler_desc, compare_enable), sizeof(rhi_sampler_desc::compare_enable))) {
                options.compare_enable = desc.compare_enable;
            }
            if (covered_by(declared, offsetof(rhi_sampler_desc, compare_op), sizeof(rhi_sampler_desc::compare_op))) {
                options.compare_op = desc.compare_op;
            }
            return options;
        }

    } // namespace

    std::uint32_t core::api_version() const noexcept {
        // THE OBJECT'S OWN ATTESTATION (abi 19): the number BOTH halves compiled from the contract
        // module. The engine asks it before it uses this object, which covers the case the entry's
        // argument cannot - an `api_core` the engine did not create. Here it is trivially the constant
        // this image compiled, and the returned type is the contract's own `std::uint32_t`.
        return rhi::abi_version;
    }

    rhi::ability_bits core::abilities() const noexcept {
        // ---- TWO BITS NOW, AND EACH IS A PROMISE ABOUT SERVICE (batch-1 spec §4.1) -----------------
        //
        // `vulkan_escape` is the one ability this backend could serve from the start, and it is servable
        // for the strongest reason there is: every handle it hands out is a member that already exists
        // (`instance`, `physical_device`, `logical_device`, `graphics_queue_handle`, the frame's own
        // command buffer), so `query_extension()` answers with a real object and every operation the
        // ability declares is performable on objects this backend produces. The bit is therefore NOT
        // "the device has the feature" - a device-level fact is not an ability until something can
        // serve it (batch-1 §4.1) - it is "this backend can carry out everything vulkan_escape declares".
        //
        // `device_address` JOINED IT WHEN BUFFERS BECAME PRODUCTIBLE, which is the condition the previous
        // comment here recorded as missing: the ability now declares only `buffer_address()`, and a
        // buffer is something `create_buffer()` hands out. Its former acceleration-structure half moved
        // to `ray_tracing` (abi 5) precisely so this bit would stop being hostage to a resource no
        // backend could make - see rhi.extension.cppm's `device_address` note and the view's own.
        //
        // descriptor_heap只在heap真正可用时广播；mesh_shader和ray_tracing仍由pass通过原生接口录制。
        //
        // host_image_copy IS SERVED NOW (③-D/E step 2), so it is announced exactly when the device lets
        // the view do what it promises: the extension, its `hostImageCopy` feature, and GENERAL among the
        // layouts a host copy may READ FROM (all three are `host_image_copy_available`, verified at
        // construction). Until this batch nobody served the ability and the bit was therefore not set -
        // and the engine's heap probes called the device entry point off `core` instead; that is the
        // consumer this bit was waiting for.
        return rhi::to_bits(rhi::extension_kind::vulkan_escape) | rhi::to_bits(rhi::extension_kind::device_address) |
               (this->heap_view.ready() ? rhi::to_bits(rhi::extension_kind::descriptor_heap) : 0u) |
               (this->host_image_copy_available ? rhi::to_bits(rhi::extension_kind::host_image_copy) : 0u) |
               // AND THE DEVICE'S OWN FACTS, SERVABLE THE MOMENT THE DEVICE EXISTS (a device with no mesh shader
               // ANSWERS false - that is an answer, not an unserved ability), which is why this bit carries no
               // device condition while the two above it do.
               rhi::to_bits(rhi::extension_kind::device_capabilities);
    }

    bool core::frame_heap::ready() const noexcept {
        return this->owner != nullptr && this->owner->descriptor_heaps.ready();
    }

    rhi::descriptor_heap_properties core::frame_heap::properties() const noexcept {
        if (this->owner == nullptr)
            return {};
        auto const& limits = this->owner->descriptor_heaps.limits();
        return {
            .resource_size = this->owner->descriptor_heaps.resource_size(),
            .max_resource_size = limits.max_resource_size,
            .max_sampler_size = limits.max_sampler_size,
            .resource_alignment = limits.resource_alignment,
            .sampler_alignment = limits.sampler_alignment,
            .resource_reserved = limits.resource_reserved,
            .sampler_reserved_with_embedded = limits.sampler_reserved_with_embedded,
            .buffer_descriptor_size = limits.buffer_descriptor_size,
            .image_descriptor_size = limits.image_descriptor_size,
            .sampler_descriptor_size = limits.sampler_descriptor_size,
            .max_push_data = limits.max_push_data,
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

        [[nodiscard]] VkCommandBuffer heap_commands(core& owner, rhi::command_buffer* const commands,
                                                    rhi::structure_header const* const next, rhi::error& result) noexcept {
            if (next != nullptr) {
                if (next->s_type != rhi::structure_type::vulkan_command_buffer) {
                    result = rhi::error::unsupported;
                    return VK_NULL_HANDLE;
                }
                result = rhi::validate_structure(*next, rhi::structure_type::vulkan_command_buffer, sizeof(rhi::vulkan_command_buffer_info));
                if (result != rhi::error::ok)
                    return VK_NULL_HANDLE;
                auto const& native = *reinterpret_cast<rhi::vulkan_command_buffer_info const*>(next);
                if (commands != nullptr || native.context != static_cast<rhi::api_core const*>(&owner) || native.commands == nullptr) {
                    result = rhi::error::invalid_argument;
                    return VK_NULL_HANDLE;
                }
                // 原生命令缓冲的归属和录制状态仍是调用方前提；不强转frame_commands。
                return static_cast<VkCommandBuffer>(native.commands);
            }
            if (commands != &owner.commands_view) {
                // ANY OTHER COMMAND BUFFER THIS BACKEND HANDED OUT IS ACCEPTED TOO (a correction to the refusal
                // that stood here): `create_command_buffer` / `make_command_buffer` produce
                // `owned_command_buffer`s - the probes' own isolated buffers are exactly that - and
                // `frame_commands::native()` answers for EITHER shape (the frame's slot buffer or an owned
                // buffer's, per `target`). The guard was written when the frame's borrowed view was the only
                // `command_buffer` the engine could pass to a heap verb.
                //
                // THE CAST IS THIS BACKEND'S ESTABLISHED CONVENTION for a handle IT handed out (`native_buffer`
                // / `native_image` / `native_sampler` / `native_pipeline` all cast the contract reference back to
                // the owned type without RTTI), and a foreign pointer would be a caller bug rather than a case to
                // survive: the contract's ownership note says touching a released handle is undefined, and
                // nothing on the engine side can obtain one of these without going through this backend.
                auto* const buffer = static_cast<core::frame_commands*>(commands);
                result = rhi::error::ok;
                return buffer->native();
            }
            if (!owner.frame_in_flight) {
                result = rhi::error::not_ready;
                return VK_NULL_HANDLE;
            }
            return owner.frame_command_buffer();
        }
    } // namespace

    rhi::error core::frame_heap::write_image(rhi::heap_image_write_info const& info) noexcept {
        rhi::error const checked = rhi::validate_structure(info.header, rhi::structure_type::heap_image_write, sizeof(info), true);
        if (checked != rhi::error::ok)
            return checked;
        if (!this->ready())
            return rhi::error::not_ready;
        VkDescriptorType const type = heap_descriptor_type(info.type);
        if (type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
            return rhi::error::unsupported;
        auto const capacity = this->owner->descriptor_heaps.resource_size();
        auto const stride = this->owner->descriptor_heaps.descriptor_stride(type);
        if (info.offset > capacity || stride > capacity - info.offset)
            return rhi::error::invalid_argument;
        VkImageViewCreateInfo view{};
        VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
        if (info.header.next != nullptr) {
            if (info.header.next->s_type != rhi::structure_type::vulkan_heap_image)
                return rhi::error::unsupported;
            rhi::error const native_checked = rhi::validate_structure(*info.header.next, rhi::structure_type::vulkan_heap_image, sizeof(rhi::vulkan_heap_image_info));
            if (native_checked != rhi::error::ok)
                return native_checked;
            auto const& native = *reinterpret_cast<rhi::vulkan_heap_image_info const*>(info.header.next);
            auto const& desc = native.view;
            if (info.resource != nullptr || info.view != nullptr || native.context != static_cast<rhi::api_core const*>(this->owner) ||
                desc.struct_size < sizeof(desc) || desc.native_image == nullptr)
                return rhi::error::invalid_argument;
            view = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .pNext = nullptr,
                .flags = desc.view_flags,
                .image = static_cast<VkImage>(desc.native_image),
                .viewType = static_cast<VkImageViewType>(desc.view_type),
                .format = static_cast<VkFormat>(desc.format),
                .components = {static_cast<VkComponentSwizzle>(desc.components[0]), static_cast<VkComponentSwizzle>(desc.components[1]),
                               static_cast<VkComponentSwizzle>(desc.components[2]), static_cast<VkComponentSwizzle>(desc.components[3])},
                .subresourceRange = {desc.aspect_mask, desc.base_mip, desc.mip_count, desc.base_layer, desc.layer_count},
            };
            layout = static_cast<VkImageLayout>(native.layout);
        } else {
            if (info.resource == nullptr || info.view == nullptr || info.view->struct_size < sizeof(rhi::image_view_desc))
                return rhi::error::invalid_argument;
            // 类型标签相同不代表具体布局相同；先核实本core发出的活跃image，再访问owned_image。
            {
                std::lock_guard const lock(this->owner->contract_images_mutex);
                if (!this->owner->contract_images.contains(info.resource))
                    return rhi::error::invalid_argument;
            }
            auto const& image = *static_cast<owned_image const*>(info.resource);
            auto const& range = *info.view;
            if (range.base_layer >= image.array_layers || range.base_mip >= image.mip_levels)
                return rhi::error::invalid_argument;
            auto const layers = range.layer_count == 0 ? image.array_layers - range.base_layer : range.layer_count;
            auto const mips = range.mip_count == 0 ? image.mip_levels - range.base_mip : range.mip_count;
            if (layers > image.array_layers - range.base_layer || mips > image.mip_levels - range.base_mip)
                return rhi::error::invalid_argument;
            bool const sampled = type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            if ((sampled && range.role != rhi::view_role::sampled) || (!sampled && range.role != rhi::view_role::storage) ||
                !rhi::has_flag(image.declared_flags, sampled ? rhi::image_flag::sampled : rhi::image_flag::storage))
                return rhi::error::unsupported;
            bool const cube = image.cube_compatible && image.array_layers == 6 && range.base_layer == 0 && layers == 6;
            view = make_image_view_info(image.native_handle, image.resolved_format,
                                        cube ? VK_IMAGE_VIEW_TYPE_CUBE : layers == 1 ? VK_IMAGE_VIEW_TYPE_2D
                                                                                     : VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                        image.declared_format == rhi::image_format::depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                                        mips, layers);
            view.subresourceRange.baseMipLevel = range.base_mip;
            view.subresourceRange.baseArrayLayer = range.base_layer;
        }
        return this->owner->descriptor_heaps.write_image(info.offset, view, layout, type) ? rhi::error::ok : rhi::error::operation_failed;
    }

    rhi::error core::frame_heap::write_buffer(rhi::heap_buffer_write_info const& info) noexcept {
        rhi::error const checked = rhi::validate_structure(info.header, rhi::structure_type::heap_buffer_write, sizeof(info));
        if (checked != rhi::error::ok)
            return checked;
        if (!this->ready())
            return rhi::error::not_ready;
        VkDescriptorType const type = heap_descriptor_type(info.type);
        if (type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER &&
            type != VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR)
            return rhi::error::unsupported;
        auto const capacity = this->owner->descriptor_heaps.resource_size();
        auto const stride = this->owner->descriptor_heaps.descriptor_stride(type);
        if (info.address == 0 || info.size == 0 || info.size > UINT64_MAX - info.address ||
            info.offset > capacity || stride > capacity - info.offset)
            return rhi::error::invalid_argument;
        return this->owner->descriptor_heaps.write_buffer(info.offset, info.address, info.size, type) ? rhi::error::ok : rhi::error::operation_failed;
    }

    rhi::error core::frame_heap::bind(rhi::heap_bind_info const& info) const noexcept {
        rhi::error result = rhi::validate_structure(info.header, rhi::structure_type::heap_bind, sizeof(info), true);
        if (result != rhi::error::ok)
            return result;
        if (!this->ready())
            return rhi::error::not_ready;
        VkCommandBuffer const commands = heap_commands(*this->owner, info.commands, info.header.next, result);
        if (result != rhi::error::ok)
            return result;
        this->owner->descriptor_heaps.record_bind(commands);
        return rhi::error::ok;
    }

    rhi::error core::frame_heap::push_data(rhi::heap_push_info const& info) const noexcept {
        rhi::error result = rhi::validate_structure(info.header, rhi::structure_type::heap_push, sizeof(info), true);
        if (result != rhi::error::ok)
            return result;
        if (!this->ready())
            return rhi::error::not_ready;
        result = rhi::validate_heap_push_range(info.offset, info.data.size(), this->properties().max_push_data);
        if (result != rhi::error::ok)
            return result;
        VkCommandBuffer const commands = heap_commands(*this->owner, info.commands, info.header.next, result);
        if (result != rhi::error::ok)
            return result;
        return this->owner->descriptor_heaps.push_data(commands, info.offset, info.data) ? rhi::error::ok : rhi::error::operation_failed;
    }

    rhi::heap_bindings core::frame_heap::bindings() const noexcept {
        if (!this->ready()) {
            return {};
        }
        VkBindHeapInfoEXT resource{}, sampler{};
        this->owner->descriptor_heaps.bind_infos(resource, sampler);
        return {
            .resource = {resource.heapRange.address, resource.heapRange.size, resource.reservedRangeOffset, resource.reservedRangeSize},
            .sampler = {sampler.heapRange.address, sampler.heapRange.size, sampler.reservedRangeOffset, sampler.reservedRangeSize},
        };
    }

    rhi::extension* core::query_extension(rhi::extension_kind const kind) noexcept {
        // The invariant is two-way and is checked in two places: at startup on the REAL backend
        // (core.constructor.cppm, gate G2) and on the probe backend in tests/test_dynamic_link.cpp
        // (G1). Every kind this backend announces answers with an object whose kind() is the kind that
        // was asked for, and every kind it does not announce answers nullptr.
        if (kind == rhi::extension_kind::vulkan_escape) {
            return &this->escape_view;
        }
        if (kind == rhi::extension_kind::descriptor_heap && this->heap_view.ready()) {
            return &this->heap_view;
        }
        if (kind == rhi::extension_kind::device_address) {
            return &this->address_view;
        }
        if (kind == rhi::extension_kind::host_image_copy && this->host_image_copy_available) {
            return &this->host_copy_view;
        }
        if (kind == rhi::extension_kind::device_capabilities) {
            // UNCONDITIONAL, and that is the difference from host_image_copy just above: every method of this
            // ability is answerable once the device exists (a device with no mesh shader answers false), so no
            // device fact can make it unserved.
            return &this->capabilities_view;
        }
        return nullptr;
    }

    rhi::swapchain* core::create_swapchain(rhi::swapchain_desc const& /*desc*/) {
        return nullptr;
    }

    rhi::buffer* core::create_buffer(rhi::buffer_desc const& declared_desc) {
        // ---- THE ABI GUARD, THEN THE DESCRIPTOR ------------------------------------------------
        // Same rule the context's descriptor follows (core.constructor.cppm): the caller declares how
        // many bytes of the structure IT compiled, and a field whose whole extent is not inside them
        // keeps this build's default. A shorter structure is an older caller; a longer one is a newer
        // caller whose appended tail this build has never heard of - either way only the prefix is read.
        rhi::buffer_desc const desc = sanitize_buffer_desc(declared_desc);

        if (desc.size == 0) {
            // A ZERO-BYTE BUFFER IS NOT A BUFFER, and the descriptor asked for nothing. Answering
            // nullptr is the contract's "this descriptor cannot be honoured" (§4.2: no throwing path).
            return nullptr;
        }

        // ---- THE CONTRACT'S VOCABULARY IN THE BACKEND'S -----------------------------------------
        // `buffer_usage` names what the CALLER does with the buffer; the allocator's `buffer_type` is
        // the same intent in this backend's spelling, so the mapping is one to one and deliberately
        // written out rather than cast: a new contract value must not silently become whatever the
        // integer happens to mean here.
        buffer_type type = buffer_type::storage_gpu_only;
        switch (desc.usage) {
        case rhi::buffer_usage::vertex:
            type = buffer_type::vertex;
            break;
        case rhi::buffer_usage::index:
            type = buffer_type::index;
            break;
        case rhi::buffer_usage::uniform_gpu_only:
            type = buffer_type::uniform_gpu_only;
            break;
        case rhi::buffer_usage::uniform_coherent:
            type = buffer_type::uniform_coherent;
            break;
        case rhi::buffer_usage::uniform_cached:
            type = buffer_type::uniform_cached;
            break;
        case rhi::buffer_usage::storage_coherent:
            type = buffer_type::storage_coherent;
            break;
        case rhi::buffer_usage::readback_coherent:
            type = buffer_type::readback_coherent;
            break;
        case rhi::buffer_usage::acceleration_structure_storage:
            type = buffer_type::acceleration_structure_storage;
            break;
        case rhi::buffer_usage::acceleration_structure_scratch:
            type = buffer_type::acceleration_structure_scratch;
            break;
        case rhi::buffer_usage::storage_gpu_only:
            type = buffer_type::storage_gpu_only;
            break;
        }

        // THE CAPABILITY FLAGS ARE THE ONLY VULKAN USAGE BITS THE CONTRACT NAMES, and they are named
        // for what they LET THE CALLER DO rather than for the bit: the renderer's twelve creation
        // sites ask for exactly these three (a device address, an acceleration-structure build input,
        // micromap storage) and for nothing else.
        uint32_t extra_usage = 0;
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::device_address)) {
            extra_usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        }
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::acceleration_structure_input)) {
            extra_usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        }
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::micromap_storage)) {
            extra_usage |= VK_BUFFER_USAGE_MICROMAP_STORAGE_BIT_EXT;
        }
        // THE THREE THAT CAME FROM THE ENGINE'S OWN CENSUS RATHER THAN FROM A GUESS (the writers found
        // them, and each one is a case where a missing bit is a SILENT wrong-data bug rather than a
        // validation error):
        //   - `storage`: the renderer writes its per-slot uniform blocks as STORAGE descriptors and reads
        //     them through a Slang StorageBuffer pointer; without the bit the mismatch "reads as zeros
        //     with NO validation finding" (vulkan/runtime/runtime.constructor.cppm's own note).
        //   - `indirect`: a buffer that carries dispatch/draw commands.
        //   - `shader_binding_table`: a ray-tracing SBT.
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::storage)) {
            extra_usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        }
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::indirect)) {
            extra_usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        }
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::shader_binding_table)) {
            extra_usage |= VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;
        }
        if (rhi::has_flag(desc.flags, rhi::buffer_flag::micromap_build_input)) {
            extra_usage |= VK_BUFFER_USAGE_MICROMAP_BUILD_INPUT_READ_ONLY_BIT_EXT;
        }

        // ---- THE ALLOCATION, AND THE ONE REFERENCE IT HANDS OVER --------------------------------
        // `initial_bytes` empty means ALLOCATE ONLY - which is what the GPU-only targets, the read-back
        // slot and an acceleration structure's storage ask for, and what a content-keyed allocator must
        // never match against anything.
        auto* const answer = new owned_buffer{};
        // `initial_bytes` is the contract's `std::byte` view and the allocator takes `uint8_t const*`:
        // the cast is the boundary between the contract's byte type and VMA's, spelled here so neither
        // side has to know the other's.
        answer->owned = this->vma.create_buffer(
            desc.initial_bytes.empty() ? nullptr : reinterpret_cast<uint8_t const*>(desc.initial_bytes.data()), desc.size, type, extra_usage);
        if (!answer->owned.valid()) {
            deren::utility::log("rhi: create_buffer could not allocate {} B (usage {}, flags {:#x})", desc.size,
                                static_cast<std::uint32_t>(desc.usage), desc.flags);
            delete answer;
            return nullptr;
        }
        answer->size_bytes = desc.size;
        answer->addressable = rhi::has_flag(desc.flags, rhi::buffer_flag::device_address);
        answer->declared_flags = desc.flags;

        // THE DETAIL IS READ, NEVER KEPT: `get_buffer_detail` answers with a pointer into the
        // allocator's own map and has already released its lock by the time it returns, so what is
        // copied out here are the two VALUES the contract needs - the Vulkan handle and the mapped
        // address - not the pointer to the record (see the type's note).
        auto const* const detail = this->vma.get_buffer_detail(answer->owned.handle());
        if (detail != nullptr) {
            answer->native = detail->buffer;
            answer->mapped_bytes = detail->allocation_info.pMappedData;
        }
        deren::utility::log("rhi: create_buffer {} B usage {} flags {:#x} -> extra {:#x}, native {:#x}, mapped {}",
                            desc.size, static_cast<std::uint32_t>(desc.usage), desc.flags, extra_usage,
                            reinterpret_cast<std::uintptr_t>(answer->native), answer->mapped_bytes != nullptr);
        return answer;
    }

    std::uint64_t core::owned_buffer::size() const noexcept {
        return this->size_bytes;
    }

    std::span<std::byte> core::owned_buffer::mapped() noexcept {
        // EMPTY when this buffer cannot be mapped - the contract's spelling of "this one is not
        // host-visible", so a caller decides from the span instead of guessing (the same answer the
        // frame's read-back view gives).
        if (this->mapped_bytes == nullptr || this->size_bytes == 0) {
            return {};
        }
        return std::span<std::byte>(static_cast<std::byte*>(this->mapped_bytes), static_cast<std::size_t>(this->size_bytes));
    }

    void core::owned_buffer::release() noexcept {
        // RELEASE IS THE ALLOCATOR'S REFERENCE-COUNT DECREMENT, and it happens inside the backend: the
        // destructor resets the `vk_buffer` owner, whose release lambda reaches `free_buffer` - which
        // destroys the buffer only if this was the last reference (rhi.api_core.cppm's ownership note;
        // vma_allocator's own comment says the same). A heap object because the FACTORY made it, so the
        // matching delete is the backend's own operator delete.
        delete this;
    }

    rhi::image* core::create_image(rhi::image_desc const& declared_desc) {
        // ---- THE ABI GUARD, THEN THE DESCRIPTOR ------------------------------------------------
        // Same rule create_buffer runs: only the caller's declared prefix is read.
        rhi::image_desc const desc = sanitize_image_desc(declared_desc);
        char const* const what = desc.debug_name != nullptr ? desc.debug_name : "unnamed image";

        // ---- THE REFUSALS, EACH NAMED -----------------------------------------------------------
        // The contract's answer to "this descriptor cannot be honoured" is nullptr plus a log line
        // (§4.2: no throwing path) - a caller that needs the reason looks at the log the name marks.
        if (desc.extent.width == 0 || desc.extent.height == 0) {
            deren::utility::log("rhi: create_image {} refused: zero extent ({}x{})", what, desc.extent.width, desc.extent.height);
            return nullptr;
        }
        if (desc.format == rhi::image_format::unknown) {
            deren::utility::log("rhi: create_image {} refused: image_format::unknown names no format", what);
            return nullptr;
        }

        // ---- THE CONTRACT'S VOCABULARY IN THE BACKEND'S ------------------------------------------
        // The format: named formats map one to one; the `depth` ROLE resolves to the device's own depth
        // attachment format, which is the capability question §17 moved out of the caller's hands.
        VkFormat const native_format = native_image_format(desc.format, this->depth_attachment_format);
        // The allocator's type: the shape flags pick it, because the type is what carries the memory
        // intent and the cube-creatability (vma.cppm's image_type note).
        image_type type = image_type::texture_2d;
        if (desc.format == rhi::image_format::depth || rhi::has_flag(desc.flags, rhi::image_flag::depth_attachment)) {
            type = image_type::texture_2d_depth;
        } else if (rhi::has_flag(desc.flags, rhi::image_flag::cube_compatible)) {
            if (desc.array_layers != 6) {
                deren::utility::log("rhi: create_image {} refused: cube_compatible needs 6 layers, asked for {}", what, desc.array_layers);
                return nullptr;
            }
            type = image_type::texture_cubemap;
        }
        image_create_info const create_info = {
            .width = desc.extent.width,
            .height = desc.extent.height,
            .mip_levels = desc.mip_levels == 0
                              ? static_cast<uint32_t>(std::bit_width(std::max(desc.extent.width, desc.extent.height)))
                              : desc.mip_levels, // 0 means "the full chain", the contract's spelling
            .array_layers = desc.array_layers,
            .format = native_format,
            .extra_usage = native_image_usage(desc.flags),
        };

        // ---- THE ALLOCATION (the allocator's own dedup sees the content at creation) --------------
        // create_image uploads the staging copy itself for data-carrying types and answers an owning
        // RAII handle (vma.cppm's type table); empty bytes mean allocate-only.
        vk_image const owned = this->vma.create_image(
            reinterpret_cast<uint8_t const*>(desc.initial_bytes.data()), desc.initial_bytes.size(), create_info, type);
        if (owned.handle() == 0) {
            deren::utility::log("rhi: create_image {} refused: the allocator could not serve {}x{}, {} layer(s), {} mip(s)",
                                what, create_info.width, create_info.height, create_info.array_layers, create_info.mip_levels);
            return nullptr;
        }

        // The detail lookup is an UNLOCKED BORROW: the VkImage is copied out NOW (the same rule
        // owned_buffer's comment states), never the pointer into the allocator's map.
        image_detail const* const detail = this->vma.get_image_detail(owned.handle());
        if (detail == nullptr) {
            deren::utility::log("rhi: create_image {} refused: the allocator holds no detail for the allocation", what);
            return nullptr;
        }
        auto* const answer = new owned_image();
        answer->owner = this;
        answer->owned = owned;
        answer->native_handle = detail->image;
        answer->resolved_format = native_format;
        answer->width = create_info.width;
        answer->height = create_info.height;
        answer->mip_levels = create_info.mip_levels;
        answer->array_layers = create_info.array_layers;
        answer->cube_compatible = rhi::has_flag(desc.flags, rhi::image_flag::cube_compatible);
        answer->declared_format = desc.format;
        answer->declared_flags = desc.flags;
        {
            std::lock_guard const lock(this->contract_images_mutex);
            this->contract_images.insert(answer);
        }
        deren::utility::log("rhi: create_image {} {}x{} layers {} mips {} -> handle {:#x}",
                            what, create_info.width, create_info.height, create_info.array_layers, create_info.mip_levels, owned.handle());
        return answer;
    }

    rhi::sampler* core::create_sampler(rhi::sampler_desc const& declared_desc) {
        rhi::sampler_desc const desc = sanitize_sampler_desc(declared_desc);

        // The contract's four modes are exactly VkSamplerAddressMode's common four, written out per
        // the same rule as every mapping above.
        VkSamplerAddressMode mode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        switch (desc.address_mode) {
        case rhi::sampler_address_mode::repeat:
            mode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            break;
        case rhi::sampler_address_mode::mirrored_repeat:
            mode = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            break;
        case rhi::sampler_address_mode::clamp_to_edge:
            mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            break;
        case rhi::sampler_address_mode::clamp_to_border:
            mode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            break;
        }

        // abi 16's fields ride on the SAME builder the backend's own six samplers use
        // (`make_texture_sampler_info`), so a description that spells out those six is value-for-value the
        // sampler the backend used to create on the engine's behalf - that equality is what keeps the
        // pictures identical, and it is why only the four knobs are overridden here.
        VkSamplerCreateInfo info = make_texture_sampler_info(mode, desc.max_lod);
        info.magFilter = desc.mag_filter == rhi::sampler_filter::nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        info.minFilter = desc.min_filter == rhi::sampler_filter::nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        info.mipmapMode = desc.mipmap_mode == rhi::sampler_mipmap_mode::nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
        info.compareEnable = desc.compare_enable ? VK_TRUE : VK_FALSE;
        info.compareOp = desc.compare_enable ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_NEVER;
        VkSampler handle = VK_NULL_HANDLE;
        if (vkCreateSampler(this->logical_device, &info, nullptr, &handle) != VK_SUCCESS || handle == VK_NULL_HANDLE) {
            deren::utility::log("rhi: create_sampler refused: vkCreateSampler failed (mode {}, max lod {})",
                                static_cast<int>(desc.address_mode), desc.max_lod);
            return nullptr;
        }
        auto* const answer = new owned_sampler();
        answer->native_sampler_handle = handle;
        answer->device = this->logical_device;
        return answer;
    }

    // ---- THE OWNED IMAGE FACE'S OBJECTS (abi 7, §17's design) --------------------------------------

    rhi::image_extent core::owned_image::extent() const noexcept {
        return {.width = this->width, .height = this->height, .depth = 1};
    }

    rhi::image_format core::owned_image::format() const noexcept {
        return this->declared_format;
    }

    rhi::image_view* core::owned_image::make_view(rhi::image_view_desc const& declared_desc) {
        // The range is validated against THIS image's shape, per the contract's rule: a range the
        // image does not have is refused, not clamped - a clamped view would sample the wrong mip and
        // say nothing.
        rhi::image_view_desc const desc = sanitize_image_view_desc(declared_desc);
        uint32_t const layers = desc.layer_count == 0 ? this->array_layers - desc.base_layer : desc.layer_count;
        uint32_t const mips = desc.mip_count == 0 ? this->mip_levels - desc.base_mip : desc.mip_count;
        if (desc.base_layer >= this->array_layers || layers == 0 || desc.base_layer + layers > this->array_layers) {
            deren::utility::log("rhi: make_view refused: layer range [{} + {}) outside the image's {}", desc.base_layer, layers, this->array_layers);
            return nullptr;
        }
        if (desc.base_mip >= this->mip_levels || mips == 0 || desc.base_mip + mips > this->mip_levels) {
            deren::utility::log("rhi: make_view refused: mip range [{} + {}) outside the image's {}", desc.base_mip, mips, this->mip_levels);
            return nullptr;
        }

        // The role picks the aspect (the format is the image's own). The view TYPE follows the image's
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
        VkImageViewType const view_type = whole_cube    ? VK_IMAGE_VIEW_TYPE_CUBE
                                          : layers == 1 ? VK_IMAGE_VIEW_TYPE_2D
                                                        : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        VkImageViewCreateInfo const base_info = make_image_view_info(this->native_handle,
                                                                     this->resolved_format,
                                                                     view_type, aspect, mips, layers);
        // make_image_view_info always bases at 0; the contract's desc carries the base explicitly.
        VkImageViewCreateInfo view_info = base_info;
        view_info.subresourceRange.baseMipLevel = desc.base_mip;
        view_info.subresourceRange.baseArrayLayer = desc.base_layer;
        view_info.subresourceRange.levelCount = mips;
        view_info.subresourceRange.layerCount = layers;

        VkImageView handle = VK_NULL_HANDLE;
        if (vkCreateImageView(this->owner->logical_device, &view_info, nullptr, &handle) != VK_SUCCESS || handle == VK_NULL_HANDLE) {
            deren::utility::log("rhi: make_view refused: vkCreateImageView failed");
            return nullptr;
        }
        auto* const answer = new owned_image_view();
        answer->native_view = handle;
        answer->device = this->owner->logical_device;
        return answer;
    }

    void core::owned_image::release() noexcept {
        {
            std::lock_guard const lock(this->owner->contract_images_mutex);
            this->owner->contract_images.erase(this);
        }
        // `delete this`: the destructor resets the `vk_image` RAII owner, which is the allocator's
        // reference-count decrement (rhi.api_core.cppm's ownership note - release, not necessarily
        // destruction: a content-deduplicated image dies when its LAST reference goes).
        delete this;
    }

    core::owned_image_view::~owned_image_view() noexcept {
        if (this->native_view != VK_NULL_HANDLE && this->device != VK_NULL_HANDLE) {
            vkDestroyImageView(this->device, this->native_view, nullptr);
            this->native_view = VK_NULL_HANDLE;
        }
    }

    void core::owned_image_view::release() noexcept {
        delete this;
    }

    core::owned_sampler::~owned_sampler() noexcept {
        if (this->native_sampler_handle != VK_NULL_HANDLE && this->device != VK_NULL_HANDLE) {
            vkDestroySampler(this->device, this->native_sampler_handle, nullptr);
            this->native_sampler_handle = VK_NULL_HANDLE;
        }
    }

    void core::owned_sampler::release() noexcept {
        delete this;
    }

    void core::owned_shader::release() noexcept {
        delete this;
    }

    void core::owned_pipeline::release() noexcept {
        delete this;
    }

    rhi::shader* core::create_shader(rhi::shader_desc const& declared_desc) {
        rhi::shader_desc const desc = sanitize_shader_desc(declared_desc);
        char const* const what = desc.debug_name != nullptr ? desc.debug_name : "unnamed shader";

        VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT;
        switch (desc.stage) {
        case rhi::shader_stage::vertex:
            stage = VK_SHADER_STAGE_VERTEX_BIT;
            break;
        case rhi::shader_stage::mesh:
            stage = VK_SHADER_STAGE_MESH_BIT_EXT;
            break;
        case rhi::shader_stage::fragment:
            stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            break;
        case rhi::shader_stage::compute:
            stage = VK_SHADER_STAGE_COMPUTE_BIT;
            break;
        // THE RAY-TRACING STAGES (appended with the ray-tracing pipeline spelling, abi 21): the module is
        // stage-less, so this mapping is only what a caller's stage value MEANS - but a switch that silently
        // fell through to the vertex default would be a wrong answer at the one place the contract states it.
        case rhi::shader_stage::ray_generation:
            stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
            break;
        case rhi::shader_stage::miss:
            stage = VK_SHADER_STAGE_MISS_BIT_KHR;
            break;
        case rhi::shader_stage::closest_hit:
            stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
            break;
        case rhi::shader_stage::any_hit:
            stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
            break;
        case rhi::shader_stage::intersection:
            stage = VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
            break;
        }
        (void)stage; // the module itself is stage-less; the stage rides the pipeline's stage info

        std::optional<vk_shader_module> module = ::deren::vulkan::make_shader_module(
            std::span<uint8_t const>(reinterpret_cast<uint8_t const*>(desc.code.data()), desc.code.size()),
            this->logical_device);
        if (!module.has_value()) {
            deren::utility::log("rhi: create_shader {} refused: vkCreateShaderModule failed", what);
            return nullptr;
        }
        auto* const answer = new owned_shader();
        answer->owned.emplace(std::move(module.value()));
        answer->native_handle = answer->owned->get();
        return answer;
    }

    rhi::pipeline* core::create_pipeline(rhi::pipeline_desc const& declared_desc) {
        rhi::pipeline_desc const desc = sanitize_pipeline_desc(declared_desc);
        char const* const what = desc.debug_name != nullptr ? desc.debug_name : "unnamed pipeline";

        // ---- THE COMPUTE SPELLING IS ITS OWN PATH (abi 21) ---------------------------------------
        // ONE stage, no attachment state: `first_stage == compute` is the whole difference the caller
        // states, and everything the graphics path below derives from formats and blends is meaningless
        // here. The branch is FIRST so the graphics field reads below cannot be reached with a compute
        // descriptor (a compute pipeline whose `color_formats` names `unknown` would be refused for the
        // wrong reason).
        if (!desc.acceleration_structure_bindings.empty() &&
            (desc.first_stage == rhi::shader_stage::compute || desc.ray_tracing_stages.empty())) {
            deren::utility::log("rhi: create_pipeline {} refused: acceleration-structure bindings require a ray-tracing pipeline", what);
            return nullptr;
        }
        if (desc.first_stage == rhi::shader_stage::compute) {
            return this->create_compute_pipeline(desc, what);
        }
        // ---- AND THE RAY-TRACING SPELLING (abi 21) ------------------------------------------------
        // The switch is the STAGES, not a first stage: a ray-tracing pipeline is the only kind built from
        // several named entry points and a group table (see the descriptor's own note), so their presence is
        // what says which path this is.
        if (!desc.ray_tracing_stages.empty()) {
            return this->create_ray_tracing_pipeline(desc, what);
        }

        // ---- THE CONTRACT'S VOCABULARY IN THE BACKEND'S ------------------------------------------
        // Color formats one to one; the `depth` ROLE resolves to the device's own depth attachment
        // format; `unknown` as the depth format means NO depth attachment (make_pipeline's
        // VK_FORMAT_UNDEFINED spelling).
        std::vector<VkFormat> color_formats(desc.color_formats.size());
        for (std::size_t index = 0; index < desc.color_formats.size(); ++index) {
            color_formats[index] = native_image_format(desc.color_formats[index], this->depth_attachment_format);
            if (color_formats[index] == VK_FORMAT_UNDEFINED) {
                deren::utility::log("rhi: create_pipeline {} refused: color attachment {} is image_format::unknown", what, index);
                return nullptr;
            }
        }
        VkFormat const depth_format = desc.depth_format == rhi::image_format::unknown
                                          ? VK_FORMAT_UNDEFINED
                                          : native_image_format(desc.depth_format, this->depth_attachment_format);

        // The blend modes are the FOUR RECIPES the survey found; empty means every target is
        // overwritten (opaque), which is the default make_pipeline itself spells.
        std::vector<VkPipelineColorBlendAttachmentState> blends;
        blends.reserve(desc.blend_modes.size());
        for (rhi::blend_mode const mode : desc.blend_modes) {
            switch (mode) {
            case rhi::blend_mode::opaque:
                blends.push_back(make_color_blend_attachment_opaque());
                break;
            case rhi::blend_mode::alpha:
                blends.push_back(make_color_blend_attachment());
                break;
            case rhi::blend_mode::additive:
                blends.push_back(make_color_blend_attachment_additive());
                break;
            case rhi::blend_mode::multiply:
                blends.push_back(make_color_blend_attachment_multiply());
                break;
            }
        }

        VkSampleCountFlagBits const samples = static_cast<VkSampleCountFlagBits>(desc.sample_count);
        VkCompareOp const compare = desc.compare == rhi::depth_compare::equal ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
        if (desc.first_stage == rhi::shader_stage::mesh) {
            // the full overload takes the compare op; the simple one the mesh spelling routes through
            // carries the same default. Routed here because the mesh stage REPLACES the vertex stage.
        }
        std::expected<vk_pipeline, std::string_view> pipeline = make_pipeline(
            this->logical_device,
            std::span<VkFormat const>(color_formats.data(), color_formats.size()),
            depth_format,
            std::span<uint8_t const>(reinterpret_cast<uint8_t const*>(desc.vertex_code.data()), desc.vertex_code.size()),
            std::span<uint8_t const>(reinterpret_cast<uint8_t const*>(desc.fragment_code.data()), desc.fragment_code.size()),
            samples,
            desc.depth_test,
            desc.depth_bias_constant_factor,
            desc.depth_bias_slope_factor,
            desc.depth_bias_clamp,
            std::span<VkPipelineColorBlendAttachmentState const>(blends.data(), blends.size()),
            desc.first_stage == rhi::shader_stage::mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT,
            compare);
        if (!pipeline.has_value()) {
            deren::utility::log("rhi: create_pipeline {} refused: {}", what, pipeline.error());
            return nullptr;
        }
        auto* const answer = new owned_pipeline();
        answer->owned.emplace(std::move(pipeline.value()));
        answer->native_handle = answer->owned->get_pipeline();
        return answer;
    }

    /// THE COMPUTE PIPELINE, in the shape the engine's raw builders had (pipelines.cppm's
    /// `vkCreateComputePipelines` sites) - the same heap-native rules, moved to the side that owns them.
    ///
    /// TWO FACTS MAKE IT A SEPARATE PATH rather than a branch inside the graphics one:
    ///
    ///  * A COMPUTE PIPELINE HAS NO ATTACHMENT STATE. Everything the graphics path derives (the vertex-input
    ///    interface, the colour formats, the blend attachments, the sample count, the depth test and bias) is
    ///    absent: the module is built from `compute_code` and the create info carries one stage, one NULL
    ///    layout and nothing else.
    ///  * THE BIND POINT IS DECIDED HERE, ONCE. `owned_pipeline::bind_point` is what `bind_pipeline` (abi 20)
    ///    hands the API, and the graphics path leaves it at its GRAPHICS default - a compute pipeline that
    ///    forgot to set it would be bound to the wrong point with no error anywhere. (`create_pipeline`'s
    ///    own note explains why the contract made the bind point the backend's business.)
    rhi::pipeline* core::create_compute_pipeline(rhi::pipeline_desc const& desc, char const* const what) {
        // THE ONE REFUSAL OF THIS PATH, and it is the ABI guard's own consequence: a caller that declared only
        // the graphics prefix has no `compute_code`, so "compute without SPIR-V" is refused by name instead of
        // being built from whatever the empty span points at.
        if (desc.compute_code.empty()) {
            deren::utility::log("rhi: create_pipeline {} refused: a compute pipeline names its SPIR-V in compute_code", what);
            return nullptr;
        }
        std::optional<vk_shader_module> module = ::deren::vulkan::make_shader_module(
            std::span<uint8_t const>(reinterpret_cast<uint8_t const*>(desc.compute_code.data()), desc.compute_code.size()),
            this->logical_device);
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
        VkPipelineCreateFlags2CreateInfo const heap_flags = {
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
        if (vkCreateComputePipelines(this->logical_device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
            deren::utility::log("rhi: create_pipeline {} refused: vkCreateComputePipelines failed", what);
            return nullptr;
        }
        auto* const answer = new owned_pipeline();
        // THE MODULE DIES WITH THIS CALL, the pipeline does not: a VkShaderModule is only needed while the
        // pipeline is being created, and holding one per pipeline would keep N modules alive for nothing. The
        // RAII wrapper above destroys it when this function returns, whatever the path out.
        answer->owned.emplace(pipeline, this->logical_device);
        answer->native_handle = pipeline;
        answer->bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
        return answer;
    }

    /// THE RAY-TRACING PIPELINE, in the shape the engine's raw builder had (pipelines.cppm's
    /// `vkCreateRayTracingPipelinesKHR` site) - the same heap-native rules, on the side that owns them.
    ///
    /// THREE THINGS MAKE IT ITS OWN PATH: it is built from SEVERAL named entry points (one module each), its
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
        auto const create_ray_tracing = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(vkGetDeviceProcAddr(this->logical_device, "vkCreateRayTracingPipelinesKHR"));
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
        // Ordinary AS bindings still read the same heap. This avoids heap-native integer-to-AS
        // conversion, which loses the device on affected NVIDIA drivers during traversal.
        // Own all stage mapping arrays until pipeline creation has consumed them.
        std::vector<std::vector<VkDescriptorSetAndBindingMappingEXT>> mappings(desc.ray_tracing_stages.size());
        std::vector<VkShaderDescriptorSetAndBindingMappingInfoEXT> mapping_infos(desc.ray_tracing_stages.size());
        for (auto const& binding : desc.acceleration_structure_bindings) {
            auto const capacity = this->descriptor_heaps.resource_size();
            auto const descriptor_size = this->descriptor_heaps.descriptor_stride(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
            std::uint64_t const last_offset = binding.array_count == 0 ? 0 : static_cast<std::uint64_t>(binding.array_count - 1) * binding.array_stride;
            if (!this->descriptor_heaps.ready() || descriptor_size == 0 || binding.array_count == 0 || binding.array_stride < descriptor_size ||
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
                VkDescriptorSetAndBindingMappingEXT mapping{};
                mapping.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
                mapping.descriptorSet = binding.descriptor_set;
                mapping.firstBinding = binding.binding;
                mapping.bindingCount = 1;
                mapping.resourceMask = VK_SPIRV_RESOURCE_TYPE_ACCELERATION_STRUCTURE_BIT_EXT;
                mapping.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
                mapping.sourceData.constantOffset.heapOffset = binding.byte_offset;
                mapping.sourceData.constantOffset.heapArrayStride = binding.array_stride;
                mappings[index].push_back(mapping);
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
        std::vector<std::optional<vk_shader_module>> modules(desc.ray_tracing_stages.size());
        std::vector<VkPipelineShaderStageCreateInfo> stages;
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
                std::span<uint8_t const>(reinterpret_cast<uint8_t const*>(declared.code.data()), declared.code.size()), this->logical_device);
            if (!modules[index].has_value()) {
                deren::utility::log("rhi: create_pipeline {} refused: vkCreateShaderModule failed for ray-tracing stage {}", what, index);
                return nullptr;
            }
            stages.push_back(VkPipelineShaderStageCreateInfo{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                             .pNext = mappings[index].empty() ? nullptr : &mapping_infos[index],
                                                             .flags = 0,
                                                             .stage = kind.value(),
                                                             .module = modules[index]->get(),
                                                             .pName = "main", // every entry point this renderer builds is "main"
                                                             .pSpecializationInfo = nullptr});
        }
        // THE GROUP TABLE, with the kind following from WHICH SLOTS ARE FILLED (the descriptor's own rule):
        // any hit slot makes it a hit group, and `triangles` says whether its geometry is triangles. This is the
        // one place the two spellings meet, and it is a translation rather than a second vocabulary.
        std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
        groups.reserve(desc.ray_tracing_groups.size());
        for (rhi::ray_tracing_group const& group : desc.ray_tracing_groups) {
            bool const hit_group = group.closest_hit != rhi::shader_group_none || group.any_hit != rhi::shader_group_none || group.intersection != rhi::shader_group_none;
            groups.push_back(VkRayTracingShaderGroupCreateInfoKHR{.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,
                                                                  .pNext = nullptr,
                                                                  .type = !hit_group        ? VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR
                                                                          : group.triangles ? VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR
                                                                                            : VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR,
                                                                  .generalShader = group.general,
                                                                  .closestHitShader = group.closest_hit,
                                                                  .anyHitShader = group.any_hit,
                                                                  .intersectionShader = group.intersection,
                                                                  .pShaderGroupCaptureReplayHandle = nullptr});
        }
        // THE HEAP FLAG IS NOT OPTIONAL WHEN THE LAYOUT IS NULL (VUID-VkRayTracingPipelineCreateInfoKHR-...):
        // the same "both or neither" rule the compute path states, through the same flags2 structure.
        VkPipelineCreateFlags2CreateInfo const heap_flags = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT,
        };
        VkRayTracingPipelineCreateInfoKHR const info = {.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,
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
                                                        .basePipelineIndex = -1};
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (create_ray_tracing(this->logical_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
            deren::utility::log("rhi: create_pipeline {} refused: vkCreateRayTracingPipelinesKHR failed", what);
            return nullptr;
        }
        auto* const answer = new owned_pipeline();
        answer->owned.emplace(pipeline, this->logical_device);
        answer->native_handle = pipeline;
        answer->bind_point = VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
        return answer;
    }

    rhi::query* core::create_query(rhi::query_desc const& /*desc*/) {
        return nullptr;
    }

    rhi::command_buffer* core::begin_commands() {
        // THE RECORDING VIEW OF THE FRAME IN FLIGHT, OR nullptr WHEN THERE IS NONE. The verb is not
        // literal yet: the frame's own vkBeginCommandBuffer/vkEndCommandBuffer still belong to the
        // engine, which also decides the present recipe - this call starts nothing, it hands out the
        // list the frame's recording is already going into. The `nullptr` when no frame is in flight
        // is the contract's way of saying "there is nothing to record into".
        if (!this->frame_in_flight) {
            return nullptr;
        }
        return &this->commands_view;
    }

    // ---- THE OWNED COMMAND BUFFER (abi 15) ----------------------------------------------------------

    rhi::command_buffer* core::create_command_buffer(rhi::command_buffer_desc const& declared_desc) {
        // THE ABI GUARD, the same rule every factory descriptor follows: only the prefix the caller
        // declares is read, so an older caller's structure keeps this build's default.
        rhi::command_buffer_kind kind = rhi::command_buffer_kind::primary;
        if (covered_by(declared_desc.struct_size, offsetof(rhi::command_buffer_desc, kind), sizeof(rhi::command_buffer_desc::kind))) {
            kind = declared_desc.kind;
        }
        // THE KIND IS THE ONE REFUSAL THIS FACTORY HAS: a value outside the two roles the contract
        // names is a caller bug, and the answer is nullptr plus the reason on the record (§4.2: no
        // throwing path).
        if (kind != rhi::command_buffer_kind::primary && kind != rhi::command_buffer_kind::secondary) {
            deren::utility::log("rhi: create_command_buffer refused: unknown command_buffer_kind {}", static_cast<std::uint32_t>(kind));
            return nullptr;
        }

        auto* const answer = new owned_command_buffer();
        answer->owner = this;
        // ONE POOL PER BUFFER, created and owned by the wrapper (handles/handles.cppm): the handle is a
        // self-contained device resource and the recording thread that owns it never shares a pool.
        answer->buffer = ::deren::vulkan::make_command_buffer(this->logical_device, this->graphics_queue_family_index,
                                                              kind == rhi::command_buffer_kind::secondary ? VK_COMMAND_BUFFER_LEVEL_SECONDARY
                                                                                                          : VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        if (*answer->buffer == VK_NULL_HANDLE) {
            delete answer;
            deren::utility::log("rhi: create_command_buffer refused: the backend could not allocate the buffer");
            return nullptr;
        }
        // ITS RECORDING VIEW IS THIS BUFFER'S OWN (abi 15): the list the contract hands back knows which
        // buffer it records into, which is what keeps one `command_buffer` type serving both the frame's
        // list and every owned buffer.
        answer->owner = this;
        answer->target = *answer->buffer;
        {
            // THE PROVENANCE REGISTRY, the same shape `contract_images` uses: `execute()` and the
            // escape's native-handle answer must tell a buffer this backend made from a pointer a
            // caller holds, and the cast that reads it is only defined once that is known.
            std::lock_guard const lock(this->contract_command_buffers_mutex);
            this->contract_command_buffers.insert(answer);
        }
        deren::utility::log("rhi: create_command_buffer {} -> native {:#x} (its own command pool)",
                            kind == rhi::command_buffer_kind::secondary ? "secondary" : "primary",
                            reinterpret_cast<std::uintptr_t>(*answer->buffer));
        return answer;
    }

    std::shared_ptr<rhi::command_buffer> core::make_command_buffer(rhi::command_buffer_desc const& desc) {
        rhi::command_buffer* const raw = this->create_command_buffer(desc);
        if (raw == nullptr) {
            return {}; // the refusal was named where it happened; an empty shared_ptr is the same answer
        }
        // THE CONTROL BLOCK OWNS THE DROP, and the drop IS `release()`: this introduces no second
        // lifetime rule - it is the contract's one-reference drop, called from the deleter instead of
        // from a call site.
        return std::shared_ptr<rhi::command_buffer>(raw, [](rhi::command_buffer* const p) noexcept {
            if (p != nullptr) {
                p->release();
            }
        });
    }

    void core::owned_command_buffer::release() noexcept {
        {
            std::lock_guard const lock(this->owner->contract_command_buffers_mutex);
            this->owner->contract_command_buffers.erase(this);
        }
        // `delete this`: the destructor releases the `vk_command_buffer`, whose own release DESTROYS THE
        // COMMAND POOL it created - the contract's one reference is the whole lifetime rule here (there
        // is no allocator registry behind this resource, unlike a buffer or an image).
        delete this;
    }

    rhi::error core::owned_command_buffer::begin_recording(rhi::command_buffer_begin_info const& declared_info) {
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
            return rhi::error::not_ready; // this handle holds no reference any more
        }

        VkCommandBufferInheritanceRenderingInfo rendering = {};
        VkCommandBufferInheritanceDescriptorHeapInfoEXT heap_inheritance = {};
        VkBindHeapInfoEXT resource_bind = {};
        VkBindHeapInfoEXT sampler_bind = {};
        VkCommandBufferInheritanceInfo inheritance = {};
        bool const inherits = next != nullptr;
        if (inherits) {
            // THE CHAIN IS READ, NEVER DROPPED (the rule the heap requests live by). Today's one
            // structure is the Vulkan attachment inheritance a `render_pass_continue` secondary MUST
            // declare; anything else is refused BY NAME below.
            if (next->s_type != rhi::structure_type::vulkan_command_buffer_inheritance ||
                rhi::validate_structure(*next, rhi::structure_type::vulkan_command_buffer_inheritance, sizeof(rhi::vulkan_command_buffer_inheritance_info)) != rhi::error::ok) {
                if (!this->refused_chain_logged) {
                    this->refused_chain_logged = true;
                    deren::utility::log("rhi: begin_recording refused a parameter chain of type {:#x} - this backend serves only "
                                        "vulkan_command_buffer_inheritance (a chain is never dropped silently)",
                                        static_cast<std::uint32_t>(next->s_type));
                }
                return rhi::error::unsupported;
            }
            auto const& declared_inheritance = *reinterpret_cast<rhi::vulkan_command_buffer_inheritance_info const*>(next);
            rendering.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
            rendering.pNext = nullptr;
            rendering.flags = 0;
            rendering.viewMask = declared_inheritance.view_mask;
            rendering.colorAttachmentCount = declared_inheritance.color_format_count;
            rendering.pColorAttachmentFormats = reinterpret_cast<VkFormat const*>(declared_inheritance.color_formats);
            rendering.depthAttachmentFormat = static_cast<VkFormat>(declared_inheritance.depth_format);
            rendering.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
            rendering.rasterizationSamples = static_cast<VkSampleCountFlagBits>(declared_inheritance.samples);
            inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
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
        // BACKEND's own state - this class owns both heaps, `heap_inheritance`'s two bind infos come
        // straight out of `descriptor_heaps` (the same two `record_bind` binds for a primary), and the
        // CONTRACT's `begin_recording` says so in its own words: the parameter chain carries what the
        // caller knows (which attachment formats the instance has), and "the backend owns the
        // compatibility rules" - which heap state a secondary is validated against is exactly such a
        // rule. Asking every caller to reach for `VkCommandBufferInheritanceDescriptorHeapInfoEXT`
        // would put this backend's heap layout into the engine half (the boundary this whole face
        // exists to hold), and a caller that forgot it would produce a BLACK secondary with the only
        // symptom being a VUID nobody reads.
        //
        // A SECONDARY IS VALIDATED ON ITS OWN, so the bind the primary recorded never reaches it - and
        // an inherited heap is only meaningful for a continuation, which is the one case where the
        // secondary's draws are validated against the instance the primary opened. It is chained in
        // FRONT of whatever the caller's own chain put in `pNext` (the attachment inheritance above),
        // because a chain is a list and this entry is the backend's, not the caller's. Only when the
        // heap face is ACTIVE: a device without VK_EXT_descriptor_heap leaves both heaps unusable, the
        // bind infos stay zero, and chaining a heap the device never got would be worse than leaving it
        // out - the same "the heap is simply unused" state every other heap path in this file accepts.
        if (this->owner->descriptor_heaps.ready() && (inherits || rhi::has_flag(usage, rhi::command_buffer_usage::render_pass_continue))) {
            this->owner->descriptor_heaps.bind_infos(resource_bind, sampler_bind);
            heap_inheritance = VkCommandBufferInheritanceDescriptorHeapInfoEXT{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_DESCRIPTOR_HEAP_INFO_EXT,
                .pNext = inheritance.pNext,
                .pSamplerHeapBindInfo = &sampler_bind,
                .pResourceHeapBindInfo = &resource_bind,
            };
            inheritance.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
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

        VkCommandBufferBeginInfo const begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = flags,
            // The inheritance info is passed whenever ANY of it was filled: the caller's attachment chain
            // or this backend's own heap entry (see above). When neither applies - a primary, or a
            // secondary begun for inline recording - `pInheritanceInfo` must be NULL.
            .pInheritanceInfo = (inherits || inheritance.pNext != nullptr) ? &inheritance : nullptr,
        };
        return generic_error(vkBeginCommandBuffer(*this->buffer, &begin));
    }

    rhi::error core::owned_command_buffer::end_recording() noexcept {
        if (*this->buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        return generic_error(vkEndCommandBuffer(*this->buffer));
    }

    // ---- THE FRAME'S BORROWED BUFFER: the four lifecycle verbs, refused BY NAME (see the note in
    //      core.declarations.cppm). release() is the one that must not be silent - a caller that wrapped
    //      this view in object_manager<> has a bug - so it says so once, exactly as frame_image_slot does.

    void core::frame_commands::release() noexcept {
        if (!this->borrowed_lifecycle_logged) {
            this->borrowed_lifecycle_logged = true;
            deren::utility::log("core: release() on the FRAME's command buffer - the core owns it and this view carries no reference to drop (logged once)");
        }
    }

    rhi::error core::frame_commands::begin_recording(rhi::command_buffer_begin_info const& declared_info) {
        // THE FRAME'S OWN RECORDING RIDES THE CONTRACT NOW (abi 26), and the two verbs under this comment are
        // why the frame loop stopped calling `vkBeginCommandBuffer`/`vkEndCommandBuffer` itself: the buffer is
        // the CORE's (`frame_command_buffer()`), the API's state machine behind it is exactly what the
        // recording face exists to own, and the engine half must not name an entry point to open a frame.
        //
        // WHAT THE REFUSAL THAT STOOD HERE WAS PROTECTING: "the frame loop begins the frame's recording, not a
        // pass". That rule is still true and is now the CALLER's to keep - the contract's own `begin_recording`
        // doc has always said a buffer's lifecycle is begun once and by its owner - while the pass layer never
        // reaches this verb at all (a pass records into `resolved_io::list`, whose recording the RUNNER opened).
        // A usage bit this backend cannot serve is still refused BY NAME, exactly as the owned form refuses it.
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
        // `render_pass_continue` is a SECONDARY's bit and the frame's buffer is a primary: the owned form refuses
        // the combination it cannot spell rather than passing a flag the API rejects on a primary.
        if (rhi::has_flag(usage, rhi::command_buffer_usage::render_pass_continue)) {
            return rhi::error::unsupported;
        }
        VkCommandBufferBeginInfo const begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = flags,
            .pInheritanceInfo = nullptr, // a primary has no inheritance
        };
        return generic_error(vkBeginCommandBuffer(command_buffer, &begin));
    }

    rhi::error core::frame_commands::end_recording() noexcept {
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        return generic_error(vkEndCommandBuffer(command_buffer));
    }

    rhi::error core::frame_commands::execute(rhi::command_buffer& secondary) {
        // THE FRAME'S OWN RECORDING EXECUTES SECONDARIES TOO, AND THAT IS THE PASS LAYER'S `execute`: the
        // borrowed frame buffer IS a recording buffer, so `io.list->execute(*secondary)` (scene, shadow,
        // transparent) arrives here. Only begin/end/submit stay the frame loop's (the three verbs above);
        // the provenance rule and the one-secondary-per-call unit are the same as
        // `owned_command_buffer::execute` below.
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready; // no frame is in flight: the window `begin_commands()` answers in
        }
        {
            // PROVENANCE FIRST, exactly as the owned form: a buffer this backend did not hand out is a
            // caller bug refused by name, and no pointer is cast before that is known.
            std::lock_guard const lock(this->owner->contract_command_buffers_mutex);
            if (!this->owner->contract_command_buffers.contains(&secondary)) {
                return rhi::error::invalid_argument;
            }
        }
        auto const& other = static_cast<core::owned_command_buffer const&>(secondary);
        VkCommandBuffer const native_secondary = *other.buffer;
        if (native_secondary == VK_NULL_HANDLE) {
            return rhi::error::not_ready; // the secondary was released
        }
        vkCmdExecuteCommands(command_buffer, 1, &native_secondary);
        return rhi::error::ok;
    }
    rhi::error core::owned_command_buffer::execute(rhi::command_buffer& secondary) {
        if (*this->buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        {
            // PROVENANCE FIRST (the registry, so no pointer is cast before it is known to be ours):
            // a command buffer this backend did not hand out is a caller bug, refused by name.
            std::lock_guard const lock(this->owner->contract_command_buffers_mutex);
            if (!this->owner->contract_command_buffers.contains(&secondary)) {
                return rhi::error::invalid_argument;
            }
        }
        auto const& other = static_cast<core::owned_command_buffer const&>(secondary);
        VkCommandBuffer const native = *other.buffer;
        if (native == VK_NULL_HANDLE) {
            return rhi::error::not_ready; // the secondary was released
        }
        // ONE SECONDARY PER CALL: vkCmdExecuteCommands takes an array, and the contract's unit is the
        // single secondary every API in this family executes. This records into THIS buffer, which the
        // caller promised is recording (the state itself is not queryable through Vulkan).
        vkCmdExecuteCommands(*this->buffer, 1, &native);
        return rhi::error::ok;
    }

    rhi::image* core::frame_image() noexcept {
        // THE IMAGE THE LAST ACQUIRE RETURNED, and it stays answerable AFTER the frame is submitted -
        // which is one step longer than the contract's minimum ("valid until that frame is submitted")
        // and is exactly what the read-back needs: its COPY is recorded inside the frame, but its READ
        // runs after the frame has landed and then asks this image for the extent and the format of the
        // frame it captured (runtime.readback.cppm). MEASURED: with the frame-scoped answer the read
        // stage got nullptr and every scenario reported "no screenshot produced". What the pointer may
        // not outlive is the CONTEXT; the frame-scoped window remains the caller-side rule for the
        // RECORDING verbs, and those still refuse when no frame is in flight.
        //
        // BEFORE THE FIRST ACQUIRE THERE IS NO "LAST" IMAGE, and that is a separate state from the index:
        // `acquired_image_index` starts at 0, which names a real swapchain image, so the index alone would
        // answer with image 0 before any frame ever began. `frame_acquired` is what tells the two apart.
        if (!this->frame_acquired || static_cast<std::size_t>(this->acquired_image_index) >= this->swap_chain_images.size()) {
            return nullptr;
        }
        return &this->frame_image_view;
    }

    rhi::buffer* core::frame_readback_buffer() noexcept {
        // THE BACKEND'S HOST-VISIBLE READ-BACK SLOT, at least one texel per frame pixel at four bytes
        // each (the formats the contract can unpack are all 8-bit RGBA/BGRA). Grown on demand: this is
        // the slot the read-back copy is recorded into, and its size follows the swapchain extent.
        rhi::image_extent const extent = this->swap_chain_extent;
        VkDeviceSize const needed = static_cast<VkDeviceSize>(extent.width) * static_cast<VkDeviceSize>(extent.height) * 4u;
        if (needed == 0) {
            return nullptr; // no swapchain extent yet: there is nothing a copy could write into
        }
        bool const fits = this->readback_slot_buffer.valid() && this->readback_slot_size >= needed && this->readback_slot_mapped != nullptr;
        if (!fits) {
            // REPLACING AN ALLOCATION THE GPU MAY STILL BE READING FROM IS THE ONE THING THAT NEEDS A
            // WAIT, and it happens when the needed size GROWS - i.e. after a swapchain recreation,
            // whose extent is this slot's whole size. The wait is on the replacement only; the
            // steady-state call is the three comparisons above and adds nothing to the frame.
            if (this->readback_slot_buffer.valid()) {
                vkDeviceWaitIdle(this->logical_device);
            }
            this->readback_slot_buffer.reset();
            this->readback_slot_handle = VK_NULL_HANDLE;
            this->readback_slot_mapped = nullptr;
            this->readback_slot_size = 0;
            this->readback_slot_buffer = this->vma.create_buffer(nullptr, needed, buffer_type::readback_coherent);
            if (!this->readback_slot_buffer.valid()) {
                deren::utility::log("rhi: read-back slot creation failed ({} bytes)", needed);
                return nullptr;
            }
            auto const* const detail = this->vma.get_buffer_detail(this->readback_slot_buffer.handle());
            if (detail == nullptr) {
                this->readback_slot_buffer.reset();
                return nullptr;
            }
            this->readback_slot_handle = detail->buffer;
            this->readback_slot_mapped = detail->allocation_info.pMappedData;
            this->readback_slot_size = needed;
            deren::utility::log("rhi: read-back slot {}x{} -> {} B (host-visible, coherent, TRANSFER_DST)", extent.width, extent.height, needed);
        }
        return &this->readback_slot_view;
    }

    VkCommandBuffer core::frame_command_buffer() const noexcept {
        // The frame slot in progress: submit() and wait_frame_slot() use `current_frame` for it, and
        // to_next_frame() advances it.
        std::size_t const slot = static_cast<std::size_t>(this->current_frame);
        if (slot >= this->frame_command_buffers.size()) {
            return VK_NULL_HANDLE;
        }
        return this->frame_command_buffers[slot].get();
    }

    // ---- the contract's frame-domain views ---------------------------------------------------------
    // Every one of them answers for the core that owns it (`owner` is set in the constructor, and the
    // view object is a member of that core, so it can never outlive it).

    rhi::image_extent core::frame_image_slot::extent() const noexcept {
        rhi::image_extent const extent = this->owner->swap_chain_extent;
        return rhi::image_extent{.width = extent.width, .height = extent.height, .depth = 1};
    }

    rhi::image_view* core::frame_image_slot::make_view(rhi::image_view_desc const& declared_desc) {
        // A BORROWED IMAGE HANDS OUT OWNED VIEWS (③-D/E item B, abi 16). What is borrowed is the IMAGE -
        // the swapchain image, owned by the presentation path - but a VIEW is a NEW backend object created
        // right here, so `release()` on it is real and the contract's ownership rule applies to it exactly
        // as it does to `create_image()->make_view()`: one reference, dropped once, inside the backend.
        //
        // THE LIFETIME RULE THE CALLER MUST KEEP (and the reason this used to refuse): a view made over a
        // swapchain image dies with that image, so the caller has to release every view it created from a
        // borrowed frame image BEFORE the swapchain is recreated (`swapchain::recreate()`, which the engine
        // drives from `runtime::on_swapchain_recreated`). A view that outlives its image is a stale handle
        // the validation layer reports; this backend does not track the caller's views, so it cannot paper
        // over it. In this renderer the window is exactly one frame's worth: the engine makes the view while
        // a frame is being recorded and releases it after the frame's submission, and a rebuild happens
        // between frames.
        rhi::image_view_desc const desc = sanitize_image_view_desc(declared_desc);
        VkImage const image = this->handle();
        if (image == VK_NULL_HANDLE) {
            deren::utility::log("rhi: make_view on the borrowed frame image refused: no image has been acquired yet");
            return nullptr;
        }
        // THE SWAPCHAIN IMAGE'S SHAPE IS ONE LAYER AND ONE MIP, and that is a fact rather than a limit of
        // this implementation: the presentation image is what it is. A range outside it is refused, not
        // clamped - the same rule `owned_image::make_view` follows, for the same reason (a clamped view
        // samples something the caller did not ask for and says nothing).
        uint32_t const layers = desc.layer_count == 0 ? 1u : desc.layer_count;
        uint32_t const mips = desc.mip_count == 0 ? 1u : desc.mip_count;
        if (desc.base_layer != 0u || layers != 1u || desc.base_mip != 0u || mips != 1u) {
            deren::utility::log("rhi: make_view on the borrowed frame image refused: it is a single-layer, single-mip image "
                                "(asked for layer {} + {}, mip {} + {})",
                                desc.base_layer, layers, desc.base_mip, mips);
            return nullptr;
        }
        VkImageViewCreateInfo const view_info = make_image_view_info(image,
                                                                     this->owner->swap_chain_image_format,
                                                                     VK_IMAGE_VIEW_TYPE_2D,
                                                                     VK_IMAGE_ASPECT_COLOR_BIT,
                                                                     mips,
                                                                     layers);
        VkImageView handle = VK_NULL_HANDLE;
        if (vkCreateImageView(this->owner->logical_device, &view_info, nullptr, &handle) != VK_SUCCESS || handle == VK_NULL_HANDLE) {
            deren::utility::log("rhi: make_view on the borrowed frame image refused: vkCreateImageView failed");
            return nullptr;
        }
        auto* const answer = new owned_image_view();
        answer->native_view = handle;
        answer->device = this->owner->logical_device;
        return answer;
    }

    void core::frame_image_slot::release() noexcept {
        // A BORROWED VIEW CARRIES NO REFERENCE, SO THERE IS NOTHING TO RELEASE - AND SAYING SO IS THE WHOLE
        // ANSWER. This object is a member of the core; the swapchain image behind it belongs to the core and
        // dies with the core's own teardown. A caller that reached `release()` here has wrapped something it
        // never held a reference to in `object_manager` - a bug in the caller rather than a leak - so the
        // answer is a ONE-TIME NAMED LOG, not a panic and not a silent no-op (rhi.api_core.cppm's ownership
        // note; the one-time shape is the one `frame_commands::use` already uses for an image this backend
        // did not hand out).
        if (!this->borrowed_release_logged) {
            this->borrowed_release_logged = true;
            deren::utility::log("rhi: release() on the frame image view - it is BORROWED from the backend and carries no reference, so nothing was released; "
                                "only factory-created handles hold a reference the caller may drop");
        }
    }

    rhi::image_format core::frame_image_slot::format() const noexcept {
        // The four 8-bit shapes a screenshot can be unpacked from; anything else is `unknown`, and the
        // engine's read stage turns that into the one-time "unsupported swapchain format" it already
        // had (runtime.readback.cppm).
        switch (this->owner->swap_chain_image_format) {
        case VK_FORMAT_B8G8R8A8_SRGB:
            return rhi::image_format::bgra8_srgb;
        case VK_FORMAT_B8G8R8A8_UNORM:
            return rhi::image_format::bgra8_unorm;
        case VK_FORMAT_R8G8B8A8_SRGB:
            return rhi::image_format::rgba8_srgb;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return rhi::image_format::rgba8_unorm;
        default:
            return rhi::image_format::unknown;
        }
    }

    VkImage core::frame_image_slot::handle() const noexcept {
        core const& self = *this->owner;
        std::size_t const index = static_cast<std::size_t>(self.acquired_image_index);
        return index < self.swap_chain_images.size() ? self.swap_chain_images[index] : VK_NULL_HANDLE;
    }

    std::uint64_t core::frame_readback_slot::size() const noexcept {
        return static_cast<std::uint64_t>(this->owner->readback_slot_size);
    }

    void core::frame_readback_slot::release() noexcept {
        // BORROWED, same answer as the frame image view's: the read-back slot is the core's own allocation
        // (grown on demand by frame_readback_buffer(), released by the core's teardown), so a caller's
        // `release()` here drops a reference the caller never held and gets one named log.
        if (!this->borrowed_release_logged) {
            this->borrowed_release_logged = true;
            deren::utility::log("rhi: release() on the frame read-back buffer view - it is BORROWED from the backend and carries no reference, so nothing was released; "
                                "only factory-created handles hold a reference the caller may drop");
        }
    }

    std::span<std::byte> core::frame_readback_slot::mapped() noexcept {
        // EMPTY when the buffer cannot be mapped - the contract's spelling of "this one is not
        // host-visible", so a caller decides from the span instead of guessing.
        if (this->owner->readback_slot_mapped == nullptr || this->owner->readback_slot_size == 0) {
            return {};
        }
        return std::span<std::byte>(static_cast<std::byte*>(this->owner->readback_slot_mapped),
                                    static_cast<std::size_t>(this->owner->readback_slot_size));
    }

    VkBuffer core::frame_readback_slot::handle() const noexcept {
        return this->owner->readback_slot_handle;
    }

    rhi::error core::frame_commands::use(rhi::image const& resource, rhi::image_use const from, rhi::image_use const to) noexcept {
        core* const self = this->owner;
        if (self == nullptr || !self->frame_in_flight) {
            // NO FRAME IS BEING RECORDED, so there is nowhere to record the transition - and SAYING SO is
            // the point of the return value: a silently dropped barrier is a frame whose image is in a
            // state nobody declared, which is the failure class task-148 measured.
            return rhi::error::not_ready;
        }
        if (this->target != VK_NULL_HANDLE) {
            // THE FRAME-SCOPED VERBS BELONG TO THE FRAME'S LIST (abi 15): `use()` declares the FRAME
            // image's role pair, and a list that records into a buffer the caller created has no frame
            // image in it. `not_ready` is the contract's own word for it ("or the list is not this
            // frame's" - the window `use()` and `begin_commands()` share).
            return rhi::error::not_ready;
        }
        if (static_cast<void const*>(&resource) != static_cast<void const*>(&self->frame_image_view)) {
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
        barrier.image = self->frame_image_view.handle();

        // ---- GATE A5, THE RUN-TIME HALF: BOTH SIDES OF THE BARRIER, ONCE PER PAIR ------------------
        // The static_asserts above prove the equality at compile time; this dump is what puts the two
        // field lists in the run's own log (spec §6.1 A5 asks for both sides, not for a verdict).
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
        core* const self = this->owner;
        // WHAT CAN SAY NO, in the contract's own vocabulary (batch-2 spec §6.1): `unsupported` = this
        // surface cannot be a copy source at all; `invalid_argument` = the region does not fit, or the
        // destination is too small; `not_ready` = there is no frame to record into.
        if (self == nullptr || !self->frame_in_flight) {
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
        if (!self->swapchain_transfer_src_supported) {
            // The swapchain images lack TRANSFER_SRC, so copying out of one would violate
            // VUID-vkCmdCopyImageToBuffer-srcImage-00186. The ENGINE owns the one-time log and the F12
            // shutdown that go with this answer (runtime.frames.cppm); the judgement itself is the
            // backend's, because the swapchain is.
            return rhi::error::unsupported;
        }
        if (static_cast<void const*>(&destination) != static_cast<void const*>(&self->readback_slot_view) ||
            static_cast<void const*>(&source) != static_cast<void const*>(&self->frame_image_view)) {
            return rhi::error::invalid_argument;
        }
        rhi::image_extent const extent = self->swap_chain_extent;
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
        VkBufferImageCopy copy = {};
        copy.bufferOffset = 0;
        copy.bufferRowLength = 0;
        copy.bufferImageHeight = 0;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, region.mip_level, region.base_array_layer, region.array_layer_count};
        copy.imageOffset = {static_cast<int32_t>(region.offset_x), static_cast<int32_t>(region.offset_y), static_cast<int32_t>(region.offset_z)};
        copy.imageExtent = {region.extent.width, region.extent.height, region.extent.depth};
        vkCmdCopyImageToBuffer(command_buffer, self->frame_image_view.handle(), VK_IMAGE_LAYOUT_GENERAL, self->readback_slot_handle, 1, &copy);
        return rhi::error::ok;
    }

    // ---- the portable record series (abi 20) ----------------------------------------------------
    // THE TRANSLATION IS THE BACKEND'S ALONE (the plan's §1): every Vulkan structure below is built
    // here, from the contract's vocabulary. The handles the descriptors carry are verified by
    // interface identity and, for images, by the same registry the heap writes go through; the cast
    // to `owned_*` is defined only after that check, which is the no-RTTI provenance rule the escape
    // states as a caller premise. The readiness answer is uniform and cheap: a list with no buffer
    // to record into (`native()` null) is `not_ready` - the frame list's answer when no frame is in
    // flight, an owned list's never.

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
                // another build wrote - `vulkan/ray_tracing/ray_tracing.cpp`'s top-level build-ordering barrier,
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
                // access bit is the extension's own ACCELERATION_STRUCTURE_WRITE_KHR.
                stage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
                access = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
                return true;
            case rhi::buffer_use::micromap_write:
                // THE MICROMAP BUILD AS A WRITER (the appended value, and the spec's own pair): the opacity
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
            VkPipelineStageFlags2 const stage = hint == rhi::stage_hint::vertex     ? VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT
                                                : hint == rhi::stage_hint::fragment ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                                : hint == rhi::stage_hint::compute  ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                                                : hint == rhi::stage_hint::mesh     ? (VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT | VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT)
                                                                                    : VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
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
        /// the format is the backend's own creation fact.
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
        // contract's own layout asserts take): the constexpr mappers above are pinned entry by entry,
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
        core* const self = this->owner;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        // The verified image handles: registry membership first (the heap-write rule - the cast is
        // only defined once provenance is known), then the native handle out of the owned object. THE
        // ONE IMAGE THE REGISTRY DOES NOT HOLD is the frame's own swapchain image - a BORROWED view the
        // core itself owns (the same object `use()` accepts by identity) - so the identity check is the
        // second acceptance, with the format the swapchain was created with.
        auto const resolve_image = [&](rhi::image const* const resource, VkImage& native, VkFormat& format) -> rhi::error {
            if (resource == nullptr || resource->type() != rhi::interface_type::image) {
                return rhi::error::invalid_argument;
            }
            if (static_cast<void const*>(resource) == static_cast<void const*>(&self->frame_image_view)) {
                native = self->frame_image_view.handle();
                format = self->swap_chain_image_format;
                return rhi::error::ok;
            }
            std::lock_guard const lock(self->contract_images_mutex);
            if (!self->contract_images.contains(resource)) {
                return rhi::error::invalid_argument;
            }
            auto const* const owned = static_cast<owned_image const*>(resource);
            native = owned->native_handle;
            format = owned->resolved_format;
            return rhi::error::ok;
        };

        std::array<VkImageMemoryBarrier2, 8> image_barriers = {};
        std::array<VkBufferMemoryBarrier2, 8> buffer_barriers = {};
        std::array<VkMemoryBarrier2, 1> memory_barriers = {};
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
            if (one.resource == nullptr || one.resource->type() != rhi::interface_type::buffer) {
                return rhi::error::invalid_argument;
            }
            auto const* const owned = static_cast<owned_buffer const*>(one.resource);
            VkPipelineStageFlags2 source_stage = 0;
            VkAccessFlags2 source_access = 0;
            VkPipelineStageFlags2 destination_stage = 0;
            VkAccessFlags2 destination_access = 0;
            if (!buffer_masks_for(one.from, source_stage, source_access) || !buffer_masks_for(one.to, destination_stage, destination_access)) {
                return rhi::error::unsupported;
            }
            buffer_barriers[buffer_count++] = VkBufferMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = source_stage,
                .srcAccessMask = source_access,
                .dstStageMask = destination_stage,
                .dstAccessMask = destination_access,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = owned->native,
                .offset = one.offset,
                .size = one.size == 0 ? (owned->size_bytes - one.offset) : one.size,
            };
        }
        // THE GLOBAL MEMORY BARRIER OF THE BATCH (abi 21), if the caller declared one: the same role-pair
        // derivation the buffer loop uses, with NO operand - which is the whole reason the field exists. A
        // pair the roles cannot spell is refused by name, exactly as a buffer pair is.
        if (memory_used) {
            VkPipelineStageFlags2 source_stage = 0;
            VkAccessFlags2 source_access = 0;
            VkPipelineStageFlags2 destination_stage = 0;
            VkAccessFlags2 destination_access = 0;
            if (!buffer_masks_for(memory.from, source_stage, source_access) || !buffer_masks_for(memory.to, destination_stage, destination_access)) {
                return rhi::error::unsupported;
            }
            memory_barriers[memory_count++] = VkMemoryBarrier2{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                .pNext = nullptr,
                .srcStageMask = source_stage,
                .srcAccessMask = source_access,
                .dstStageMask = destination_stage,
                .dstAccessMask = destination_access,
            };
        }
        VkDependencyInfo const dependency = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .pNext = nullptr,
            .dependencyFlags = 0,
            .memoryBarrierCount = memory_count,
            .pMemoryBarriers = memory_count != 0 ? memory_barriers.data() : nullptr,
            .bufferMemoryBarrierCount = buffer_count,
            .pBufferMemoryBarriers = buffer_barriers.data(),
            .imageMemoryBarrierCount = image_count,
            .pImageMemoryBarriers = image_barriers.data(),
        };
        vkCmdPipelineBarrier2(command_buffer, &dependency);
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::barrier(rhi::image_barrier const& one) {
        // The single-image form is the group of one - the same derivation, the same refusals.
        std::array<rhi::image_barrier, 1> const one_barriers = {one};
        rhi::barrier_group const group{
            .struct_size = sizeof(rhi::barrier_group),
            .images = one_barriers,
            .buffers = {},
        };
        return this->barrier(group);
    }

    rhi::error core::frame_commands::begin_rendering(rhi::rendering_info const& info) {
        core* const self = this->owner;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (info.struct_size < sizeof(rhi::rendering_info)) {
            return rhi::error::invalid_argument;
        }
        auto const resolve_view = [&](rhi::image_view const* const view) -> VkImageView {
            if (view == nullptr || view->type() != rhi::interface_type::image_view) {
                return VK_NULL_HANDLE;
            }
            return static_cast<owned_image_view const*>(view)->native_view;
        };
        // THE BOUND IS THE PASS FRAMEWORK'S, NOT THIS FUNCTION'S: `pass::max_render_targets` is 8 and the
        // SCENE instance is the widest site (three surface targets + velocity + the scene colour + depth),
        // so a 4-slot array refused the scene's own scope and the frame recorded no geometry. The contract
        // places no bound of its own on `rendering_info::colors`; a scope wider than the widest DECLARED
        // site is refused rather than silently truncated.
        std::array<VkRenderingAttachmentInfo, 8> attachments = {};
        std::uint32_t color_count = 0;
        for (rhi::color_attachment const& one : info.colors) {
            if (color_count >= attachments.size()) {
                return rhi::error::invalid_argument;
            }
            VkImageView const native_view = resolve_view(one.view);
            if (native_view == VK_NULL_HANDLE) {
                return rhi::error::invalid_argument;
            }
            attachments[color_count] = VkRenderingAttachmentInfo{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext = nullptr,
                .imageView = native_view,
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                .resolveMode = VK_RESOLVE_MODE_NONE,
                .resolveImageView = VK_NULL_HANDLE,
                .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .loadOp = one.load == rhi::load_op::clear       ? VK_ATTACHMENT_LOAD_OP_CLEAR
                          : one.load == rhi::load_op::dont_care ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                                                : VK_ATTACHMENT_LOAD_OP_LOAD,
                .storeOp = one.store == rhi::store_op::dont_care ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = {.color = {{one.clear[0], one.clear[1], one.clear[2], one.clear[3]}}},
            };
            ++color_count;
        }
        VkRenderingAttachmentInfo depth_attachment = {};
        bool const depth_used = info.has_depth;
        if (depth_used) {
            VkImageView const native_view = resolve_view(info.depth.view);
            if (native_view == VK_NULL_HANDLE) {
                return rhi::error::invalid_argument;
            }
            depth_attachment = VkRenderingAttachmentInfo{
                .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .pNext = nullptr,
                .imageView = native_view,
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                .resolveMode = VK_RESOLVE_MODE_NONE,
                .resolveImageView = VK_NULL_HANDLE,
                .resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .loadOp = info.depth.load == rhi::load_op::clear       ? VK_ATTACHMENT_LOAD_OP_CLEAR
                          : info.depth.load == rhi::load_op::dont_care ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                                                                       : VK_ATTACHMENT_LOAD_OP_LOAD,
                .storeOp = info.depth.store == rhi::store_op::dont_care ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
                .clearValue = {.depthStencil = {.depth = info.depth.clear_depth, .stencil = info.depth.clear_stencil}},
            };
        }
        VkRenderingInfo const rendering = {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .pNext = nullptr,
            .flags = info.secondary_contents ? VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT : 0u,
            .renderArea = {.offset = {info.area.offset_x, info.area.offset_y}, .extent = {info.area.width, info.area.height}},
            .layerCount = info.layer_count,
            .viewMask = 0u, // no multi-view use anywhere in the renderer (the plan's §4 verdict)
            .colorAttachmentCount = color_count,
            .pColorAttachments = attachments.data(),
            .pDepthAttachment = depth_used ? &depth_attachment : nullptr,
            .pStencilAttachment = nullptr,
        };
        vkCmdBeginRendering(command_buffer, &rendering);
        return rhi::error::ok;
    }

    void core::frame_commands::end_rendering() noexcept {
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer != VK_NULL_HANDLE) {
            vkCmdEndRendering(command_buffer);
        }
    }

    rhi::error core::frame_commands::bind_pipeline(rhi::pipeline const& handle) {
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (handle.type() != rhi::interface_type::pipeline) {
            return rhi::error::invalid_argument;
        }
        auto const* const owned = static_cast<owned_pipeline const*>(&handle);
        vkCmdBindPipeline(command_buffer, owned->bind_point, owned->native_handle);
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::bind_vertex_buffer(rhi::buffer const& handle, std::uint64_t const offset) {
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (handle.type() != rhi::interface_type::buffer) {
            return rhi::error::invalid_argument;
        }
        auto const* const owned = static_cast<owned_buffer const*>(&handle);
        VkBuffer const native = owned->native;
        std::uint64_t const offsets = offset;
        vkCmdBindVertexBuffers(command_buffer, 0u, 1u, &native, &offsets);
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::bind_index_buffer(rhi::buffer const& handle, std::uint64_t const offset, rhi::index_type const type) {
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (handle.type() != rhi::interface_type::buffer) {
            return rhi::error::invalid_argument;
        }
        auto const* const owned = static_cast<owned_buffer const*>(&handle);
        vkCmdBindIndexBuffer(command_buffer, owned->native, offset, type == rhi::index_type::uint32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        return rhi::error::ok;
    }

    void core::frame_commands::draw(std::uint32_t const vertex_count, std::uint32_t const instance_count, std::uint32_t const first_vertex, std::uint32_t const first_instance) noexcept {
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer != VK_NULL_HANDLE) {
            vkCmdDraw(command_buffer, vertex_count, instance_count, first_vertex, first_instance);
        }
    }

    void core::frame_commands::draw_indexed(std::uint32_t const index_count, std::uint32_t const instance_count, std::uint32_t const first_index, std::int32_t const vertex_offset, std::uint32_t const first_instance) noexcept {
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer != VK_NULL_HANDLE) {
            vkCmdDrawIndexed(command_buffer, index_count, instance_count, first_index, vertex_offset, first_instance);
        }
    }

    void core::frame_commands::dispatch(std::uint32_t const groups_x, std::uint32_t const groups_y, std::uint32_t const groups_z) noexcept {
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer != VK_NULL_HANDLE) {
            vkCmdDispatch(command_buffer, groups_x, groups_y, groups_z);
        }
    }

    void core::frame_commands::draw_mesh_tasks(std::uint32_t const groups_x, std::uint32_t const groups_y, std::uint32_t const groups_z) noexcept {
        core* const self = this->owner;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE) {
            return;
        }
        if (self->mesh_dispatch != nullptr) {
            self->mesh_dispatch(command_buffer, groups_x, groups_y, groups_z); // the extension command, resolved once at startup
        }
    }

    void core::frame_commands::trace_rays(rhi::shader_binding_table_region const& raygen, rhi::shader_binding_table_region const& miss,
                                          rhi::shader_binding_table_region const& hit, rhi::shader_binding_table_region const& callable,
                                          std::uint32_t const width, std::uint32_t const height, std::uint32_t const depth) noexcept {
        core* const self = this->owner;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE || self->ray_trace_launch == nullptr) {
            // NOTHING IS RECORDED rather than a launch through an entry point the device did not publish: a pass
            // that launches rays is only built when its ray-tracing pipeline could be created, which needs the
            // extension - so this is the "no device support" answer, not a silent skip of a frame's work that
            // could have run (the extension command is resolved once at startup, see `core::init_device`).
            return;
        }
        // THE ONE CONVERSION FROM THE CONTRACT'S REGION TO THE DRIVER'S STRUCTURE, field for field: the contract
        // type is three numbers (address, size, stride) and Vulkan's is the same three under its own names.
        auto const as_native = [](rhi::shader_binding_table_region const& region) {
            return VkStridedDeviceAddressRegionKHR{.deviceAddress = region.address, .stride = region.stride, .size = region.size};
        };
        VkStridedDeviceAddressRegionKHR const native_raygen = as_native(raygen);
        VkStridedDeviceAddressRegionKHR const native_miss = as_native(miss);
        VkStridedDeviceAddressRegionKHR const native_hit = as_native(hit);
        VkStridedDeviceAddressRegionKHR const native_callable = as_native(callable);
        self->ray_trace_launch(command_buffer, &native_raygen, &native_miss, &native_hit, &native_callable, width, height, depth);
    }

    rhi::micromap* core::create_micromap(rhi::micromap_desc const& declared_desc) {
        // ---- THE CONTRACT'S LAYOUTS ARE THE API'S, AND THIS IS WHERE THAT IS PROVEN --------------------
        static_assert(static_cast<std::uint32_t>(rhi::micromap_format::four_state) == VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT,
                      "the contract's four-state format IS the API's");
        static_assert(static_cast<std::uint32_t>(rhi::micromap_format::two_state) == VK_OPACITY_MICROMAP_FORMAT_2_STATE_EXT,
                      "the contract's two-state format IS the API's");
        static_assert(sizeof(rhi::micromap_triangle) == sizeof(VkMicromapTriangleEXT),
                      "a caller fills an array of the contract's records and the build reads the API's");
        if (this->micromap_create == nullptr || this->micromap_build_sizes == nullptr) {
            if (!this->micromap_refusal_logged) {
                this->micromap_refusal_logged = true;
                deren::utility::log("rhi: create_micromap refused: this device has no VK_EXT_opacity_micromap "
                                    "(its four entry points did not resolve at startup)");
            }
            return nullptr;
        }

        // THE ABI GUARD, then the description's fields.
        std::uint32_t const declared = declared_desc.struct_size;
        std::uint32_t triangle_count = 0;
        std::span<std::byte const> data = {};
        std::uint32_t data_stride = 4u;
        std::span<rhi::micromap_triangle const> triangles = {};
        std::span<std::uint32_t const> indices = {};
        rhi::micromap_format format = rhi::micromap_format::four_state;
        if (covered_by(declared, offsetof(rhi::micromap_desc, triangle_count), sizeof(rhi::micromap_desc::triangle_count))) {
            triangle_count = declared_desc.triangle_count;
        }
        if (covered_by(declared, offsetof(rhi::micromap_desc, data), sizeof(rhi::micromap_desc::data))) {
            data = declared_desc.data;
        }
        if (covered_by(declared, offsetof(rhi::micromap_desc, data_stride), sizeof(rhi::micromap_desc::data_stride))) {
            data_stride = declared_desc.data_stride;
        }
        if (covered_by(declared, offsetof(rhi::micromap_desc, triangles), sizeof(rhi::micromap_desc::triangles))) {
            triangles = declared_desc.triangles;
        }
        if (covered_by(declared, offsetof(rhi::micromap_desc, indices), sizeof(rhi::micromap_desc::indices))) {
            indices = declared_desc.indices;
        }
        if (covered_by(declared, offsetof(rhi::micromap_desc, format), sizeof(rhi::micromap_desc::format))) {
            format = declared_desc.format;
        }
        if (triangle_count == 0 || data.empty() || triangles.size() < triangle_count || indices.size() < triangle_count) {
            deren::utility::log("rhi: create_micromap refused: a micromap needs {}+ attributes, {}+ records and {}+ indices for {} triangles",
                                data.empty() ? 1u : data.size(), triangle_count, triangle_count, triangle_count);
            return nullptr;
        }
        // THE STRIDE IS WHAT SAYS THE ATTRIBUTES ARE ACTUALLY THERE: a span shorter than one record per
        // micro-triangle would build a micromap whose later lookups read past the data (the format decides how
        // many bytes a record is, and `data_stride` is how the caller laid them out).
        if (data_stride == 0u || data.size() < static_cast<std::size_t>(triangle_count) * data_stride) {
            deren::utility::log("rhi: create_micromap refused: {} bytes of attributes do not cover {} micro-triangles at a stride of {}",
                                data.size(), triangle_count, data_stride);
            return nullptr;
        }

        auto* const out = new owned_micromap{};
        out->owner = this;
        out->triangle_count = triangle_count;
        out->triangle_array_stride = sizeof(VkMicromapTriangleEXT);
        out->index_stride = static_cast<std::uint32_t>(sizeof(std::uint32_t));
        out->usage = VkMicromapUsageEXT{.count = triangle_count, .subdivisionLevel = 0u, .format = static_cast<std::uint32_t>(format)};

        // ---- THE SETUP BUFFERS, AND THE ALIGNMENT THE API REQUIRES OF THEIR ADDRESSES --------------------
        // 256 BYTES ON THE DEVICE ADDRESS, not on the buffer: the allocator gives no such promise and the address
        // does not exist before the allocation, so each buffer carries one alignment worth of slack and the
        // payload is written at the first aligned address inside it. THIS IS THE ENGINE'S OLD JOB, and it is the
        // backend's now - which is the whole reason `micromap` is an object rather than a caller-managed pair.
        constexpr VkDeviceSize address_alignment = 256u;
        constexpr rhi::buffer_flags setup_flags =
            rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::micromap_build_input);
        auto const setup = [this](std::span<std::byte const> const bytes, std::uintptr_t& address) -> rhi::buffer* {
            rhi::buffer* const buffer = this->create_buffer(rhi::buffer_desc{
                .size = static_cast<std::uint64_t>(bytes.size()) + address_alignment,
                .usage = rhi::buffer_usage::storage_coherent,
                .flags = setup_flags});
            if (buffer == nullptr) {
                return nullptr;
            }
            std::span<std::byte> const mapped = buffer->mapped();
            std::uint64_t const base = this->address_view.buffer_address(*buffer, 0);
            if (mapped.data() == nullptr || base == 0) {
                buffer->release();
                return nullptr;
            }
            std::uint64_t const offset = (address_alignment - (base % address_alignment)) % address_alignment;
            std::memcpy(mapped.data() + offset, bytes.data(), bytes.size());
            address = base + offset;
            return buffer;
        };
        out->data = setup(data, out->data_address);
        std::span<std::byte const> const triangle_bytes{reinterpret_cast<std::byte const*>(triangles.data()),
                                                        static_cast<std::size_t>(triangle_count) * sizeof(rhi::micromap_triangle)};
        out->triangles = setup(triangle_bytes, out->triangles_address);
        std::span<std::byte const> const index_bytes{reinterpret_cast<std::byte const*>(indices.data()),
                                                     static_cast<std::size_t>(triangle_count) * sizeof(std::uint32_t)};
        out->indices = setup(index_bytes, out->indices_address);
        if (out->data == nullptr || out->triangles == nullptr || out->indices == nullptr) {
            deren::utility::log("rhi: create_micromap refused: a setup buffer could not be allocated or has no device address");
            delete out;
            return nullptr;
        }

        VkMicromapBuildInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT;
        info.pNext = nullptr;
        info.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
        info.flags = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT;
        info.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
        info.usageCountsCount = 1u;
        info.pUsageCounts = &out->usage;
        info.data.deviceAddress = out->data_address;
        info.triangleArray.deviceAddress = out->triangles_address;
        info.triangleArrayStride = out->triangle_array_stride;

        VkMicromapBuildSizesInfoEXT sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT;
        this->micromap_build_sizes(this->logical_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &sizes);
        if (sizes.micromapSize == 0) {
            deren::utility::log("rhi: create_micromap refused: the build-size query answered zero bytes");
            delete out;
            return nullptr;
        }
        out->storage = this->create_buffer(rhi::buffer_desc{
            .size = sizes.micromapSize,
            .usage = rhi::buffer_usage::storage_gpu_only,
            .flags = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::micromap_storage)});
        if (out->storage == nullptr) {
            deren::utility::log("rhi: create_micromap refused: the micromap storage could not be allocated");
            delete out;
            return nullptr;
        }
        if (sizes.buildScratchSize != 0) {
            // only its ADDRESS is read (by the build), so the device-address flag is the whole requirement
            out->scratch = this->create_buffer(rhi::buffer_desc{
                .size = sizes.buildScratchSize,
                .usage = rhi::buffer_usage::storage_gpu_only,
                .flags = rhi::to_bits(rhi::buffer_flag::device_address)});
            if (out->scratch == nullptr) {
                deren::utility::log("rhi: create_micromap refused: the micromap scratch could not be allocated");
                delete out;
                return nullptr;
            }
            out->scratch_address = this->address_view.buffer_address(*out->scratch, 0);
        }

        auto const* const storage_buffer = static_cast<owned_buffer const*>(out->storage);
        VkMicromapCreateInfoEXT create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT;
        create_info.pNext = nullptr;
        create_info.createFlags = 0;
        create_info.buffer = storage_buffer->native;
        create_info.offset = 0;
        create_info.size = sizes.micromapSize;
        create_info.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
        create_info.deviceAddress = 0;
        if (this->micromap_create(this->logical_device, &create_info, nullptr, &out->native) != VK_SUCCESS || out->native == VK_NULL_HANDLE) {
            deren::utility::log("rhi: create_micromap refused: vkCreateMicromapEXT failed");
            delete out;
            return nullptr;
        }
        deren::utility::log("rhi: create_micromap: {} micro-triangles, {} bytes of storage, {} bytes of scratch",
                            out->triangle_count, sizes.micromapSize, sizes.buildScratchSize);
        return out;
    }

    rhi::acceleration_structure* core::create_acceleration_structure(rhi::acceleration_structure_desc const& declared_desc) {
        // ---- THE ABI GUARD, THEN WHAT A CREATE ACTUALLY IS (tier-1 since abi 26) ------------------------
        //
        //   1. THE GEOMETRY, in the driver's words, built ONCE: the caller's records are PODs of the CONTRACT's
        //      layout, and this is the ONE place the two spellings meet - the backend owns the conversion, the
        //      same rule `mesh_task_command` follows.
        //   2. THE SIZES (`vkGetAccelerationStructureBuildSizesKHR`) and the memory: the storage the structure
        //      lives in, allocated through this backend's own factory. The caller never sees the number.
        //   3. THE HANDLE (`vkCreateAccelerationStructureKHR`) and its ADDRESS
        //      (`vkGetAccelerationStructureDeviceAddressKHR`), which is what a shader and an instance record use.
        //
        // THE SCRATCH IS ALLOCATED HERE TOO, not at a later first build: both sizes are known at creation (a
        // bottom level's geometry is fixed; a top level is sized for its capacity), so recording a build never
        // allocates - which is what keeps the recording verbs free of a failure mode in the middle of a frame.
        if (this->acceleration_structure_create == nullptr || this->acceleration_structure_build_sizes == nullptr ||
            this->acceleration_structure_address == nullptr || this->acceleration_structure_build == nullptr) {
            if (!this->acceleration_structure_refusal_logged) {
                this->acceleration_structure_refusal_logged = true;
                deren::utility::log("rhi: create_acceleration_structure refused: this device has no "
                                    "VK_KHR_acceleration_structure (its five entry points did not resolve at startup)");
            }
            return nullptr;
        }

        // THE ABI GUARD: only the prefix the caller declared is read (the rule every descriptor here follows).
        std::uint32_t const declared = declared_desc.struct_size;
        rhi::acceleration_structure_type type = rhi::acceleration_structure_type::bottom_level;
        rhi::acceleration_structure_flags flags = rhi::no_acceleration_structure_flags;
        rhi::acceleration_structure_geometry const* geometries = nullptr;
        std::uint32_t geometry_count = 0;
        std::uint32_t instance_capacity = 0;
        if (covered_by(declared, offsetof(rhi::acceleration_structure_desc, type), sizeof(rhi::acceleration_structure_desc::type))) {
            type = declared_desc.type;
        }
        if (covered_by(declared, offsetof(rhi::acceleration_structure_desc, flags), sizeof(rhi::acceleration_structure_desc::flags))) {
            flags = declared_desc.flags;
        }
        if (covered_by(declared, offsetof(rhi::acceleration_structure_desc, geometries), sizeof(rhi::acceleration_structure_desc::geometries))) {
            geometries = declared_desc.geometries;
        }
        if (covered_by(declared, offsetof(rhi::acceleration_structure_desc, geometry_count), sizeof(rhi::acceleration_structure_desc::geometry_count))) {
            geometry_count = declared_desc.geometry_count;
        }
        if (covered_by(declared, offsetof(rhi::acceleration_structure_desc, instance_capacity), sizeof(rhi::acceleration_structure_desc::instance_capacity))) {
            instance_capacity = declared_desc.instance_capacity;
        }

        bool const top_level = type == rhi::acceleration_structure_type::top_level;
        if (!top_level && (geometries == nullptr || geometry_count == 0)) {
            deren::utility::log("rhi: create_acceleration_structure refused: a bottom-level structure needs at least one geometry");
            return nullptr; // a structure with nothing to trace is a caller bug, refused by name
        }
        if (top_level && instance_capacity == 0) {
            deren::utility::log("rhi: create_acceleration_structure refused: a top-level structure needs an instance capacity");
            return nullptr;
        }

        auto* const structure = new owned_acceleration_structure{};
        structure->owner = this;
        structure->top_level = top_level;
        structure->refittable = rhi::has_flag(flags, rhi::acceleration_structure_flag::allow_update);
        structure->instance_capacity = instance_capacity;

        // ---- 1. THE GEOMETRY ---------------------------------------------------------------------------
        if (top_level) {
            rhi::buffer* const records = this->create_buffer(rhi::buffer_desc{
                .size = static_cast<std::uint64_t>(instance_capacity) * sizeof(rhi::acceleration_structure_instance),
                .usage = rhi::buffer_usage::storage_coherent,
                .flags = rhi::to_bits(rhi::buffer_flag::device_address) | rhi::to_bits(rhi::buffer_flag::acceleration_structure_input)});
            if (records == nullptr || records->mapped().data() == nullptr) {
                deren::utility::log("rhi: create_acceleration_structure refused: the instance buffer could not be allocated");
                delete structure;
                return nullptr;
            }
            structure->instances = records;
            structure->instances_mapped = records->mapped().data();
            std::uint64_t const records_address = this->address_view.buffer_address(*records, 0);
            if (records_address == 0) {
                deren::utility::log("rhi: create_acceleration_structure refused: the instance buffer has no device address");
                delete structure;
                return nullptr;
            }
            VkAccelerationStructureGeometryInstancesDataKHR instances_data{};
            instances_data.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
            instances_data.pNext = nullptr;
            instances_data.arrayOfPointers = VK_FALSE;
            instances_data.data.deviceAddress = records_address;
            structure->geometries.push_back(VkAccelerationStructureGeometryKHR{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                .pNext = nullptr,
                .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
                .geometry = {.instances = instances_data},
                .flags = 0,
            });
            structure->range = VkAccelerationStructureBuildRangeInfoKHR{.primitiveCount = 0u, .primitiveOffset = 0u, .firstVertex = 0u, .transformOffset = 0u};
        } else {
            structure->geometries.reserve(geometry_count);
            structure->micromap_attachments.reserve(geometry_count);
            for (std::uint32_t index = 0; index < geometry_count; ++index) {
                rhi::acceleration_structure_geometry const& source = geometries[index];
                VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
                triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
                triangles.pNext = nullptr;
                triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT; // the contract's geometry is positions
                triangles.vertexData.deviceAddress = source.vertex_address;
                triangles.vertexStride = source.vertex_stride;
                triangles.maxVertex = source.vertex_count == 0 ? 0u : source.vertex_count - 1u;
                // Zero index address denotes triangle soup (for example the MASK bake's output).
                triangles.indexType = source.index_address == 0 ? VK_INDEX_TYPE_NONE_KHR : static_cast<VkIndexType>(source.index_format);
                triangles.indexData.deviceAddress = source.index_address;
                triangles.transformData.deviceAddress = 0;
                // THE OPACITY MICROMAP, CHAINED INTO THE TRIANGLES DATA (not into the geometry: the geometry's own
                // pNext accepts only the micromap-DATA struct, and validation named exactly that when the engine
                // first attached it in the wrong place). THE INDEX ARRAY IS THIS BACKEND'S - it built the micromap
                // and owns the buffer - which is what makes the contract's attachment one handle and a usage record.
                if (source.opacity_micromap != nullptr) {
                    if (source.opacity_micromap->type() != rhi::interface_type::micromap) {
                        deren::utility::log("rhi: create_acceleration_structure refused: a geometry's opacity micromap is not a handle this backend handed out");
                        delete structure;
                        return nullptr;
                    }
                    auto& micromap = static_cast<owned_micromap&>(*source.opacity_micromap);
                    if (micromap.owner != this) {
                        deren::utility::log("rhi: create_acceleration_structure refused: a geometry's opacity micromap belongs to another device");
                        delete structure;
                        return nullptr;
                    }
                    structure->micromap_attachments.push_back(VkAccelerationStructureTrianglesOpacityMicromapEXT{
                        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT,
                        .pNext = nullptr,
                        .indexType = VK_INDEX_TYPE_UINT32,
                        .indexBuffer = {.deviceAddress = micromap.indices_address},
                        .indexStride = micromap.index_stride,
                        .baseTriangle = 0u,
                        .usageCountsCount = 1u,
                        .pUsageCounts = nullptr,  // filled below: it has to point INSIDE this vector
                        .ppUsageCounts = nullptr, // ... and this backend uses the single-record form
                        .micromap = micromap.native,
                    });
                    // `pUsageCounts` points at the MICROMAP's own usage record, which lives as long as the caller
                    // holds the micromap (the engine's structure set keeps every micromap it built alive for the
                    // whole session, which is why that is a safe anchor rather than a local).
                    structure->micromap_attachments.back().pUsageCounts = &micromap.usage;
                    triangles.pNext = &structure->micromap_attachments.back();
                }
                structure->geometries.push_back(VkAccelerationStructureGeometryKHR{
                    .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                    .pNext = nullptr,
                    .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                    .geometry = {.triangles = triangles},
                    // Preserve the engine's any-hit alpha test, including geometries without a micromap.
                    .flags = 0,
                });
                structure->range = VkAccelerationStructureBuildRangeInfoKHR{
                    .primitiveCount = (source.index_address == 0 ? source.vertex_count : source.index_count) / 3u,
                    .primitiveOffset = 0u,
                    .firstVertex = 0u,
                    .transformOffset = 0u,
                };
            }
        }

        // ---- 2. THE SIZES, THEN THE MEMORY --------------------------------------------------------------
        VkAccelerationStructureBuildGeometryInfoKHR size_info{};
        size_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        size_info.pNext = nullptr;
        size_info.type = top_level ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        size_info.flags = (top_level ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR) |
                          (structure->refittable ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0u);
        size_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        size_info.geometryCount = static_cast<std::uint32_t>(structure->geometries.size());
        size_info.pGeometries = structure->geometries.data();
        std::uint32_t const primitive_count = top_level ? instance_capacity : structure->range.primitiveCount;
        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        this->acceleration_structure_build_sizes(this->logical_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &size_info, &primitive_count, &sizes);
        if (sizes.accelerationStructureSize == 0) {
            deren::utility::log("rhi: create_acceleration_structure refused: the build-size query answered zero bytes");
            delete structure;
            return nullptr;
        }

        structure->storage = this->create_buffer(
            rhi::buffer_desc{.size = sizes.accelerationStructureSize, .usage = rhi::buffer_usage::acceleration_structure_storage});
        if (structure->storage == nullptr) {
            deren::utility::log("rhi: create_acceleration_structure refused: the structure's storage could not be allocated ({} bytes)", sizes.accelerationStructureSize);
            delete structure;
            return nullptr;
        }
        structure->structure_size = sizes.accelerationStructureSize;

        std::uint64_t const scratch_size = std::max(sizes.buildScratchSize, structure->refittable ? sizes.updateScratchSize : 0u);
        if (scratch_size != 0) {
            std::uint64_t const alignment = std::max<std::uint64_t>(this->acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment, 1u);
            structure->scratch = this->create_buffer(rhi::buffer_desc{.size = scratch_size + alignment - 1u,
                                                                      .usage = rhi::buffer_usage::acceleration_structure_scratch,
                                                                      .flags = rhi::to_bits(rhi::buffer_flag::device_address)});
            if (structure->scratch == nullptr) {
                deren::utility::log("rhi: create_acceleration_structure refused: the build scratch could not be allocated ({} bytes)", scratch_size);
                delete structure;
                return nullptr;
            }
            structure->scratch_size = scratch_size;
            std::uint64_t const base = this->address_view.buffer_address(*structure->scratch, 0);
            if (base == 0) {
                delete structure;
                return nullptr;
            }
            // The requirement applies to the device address, rather than merely an offset in the buffer.
            structure->scratch_address = base + (alignment - base % alignment) % alignment;
        }

        // ---- 3. THE HANDLE AND ITS ADDRESS --------------------------------------------------------------
        auto const* const storage_buffer = static_cast<owned_buffer const*>(structure->storage);
        VkAccelerationStructureCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        create_info.pNext = nullptr;
        create_info.createFlags = 0;
        create_info.buffer = storage_buffer->native;
        create_info.offset = 0;
        create_info.size = sizes.accelerationStructureSize;
        create_info.type = size_info.type;
        create_info.deviceAddress = 0;
        if (this->acceleration_structure_create(this->logical_device, &create_info, nullptr, &structure->native) != VK_SUCCESS) {
            deren::utility::log("rhi: create_acceleration_structure refused: vkCreateAccelerationStructureKHR failed");
            delete structure;
            return nullptr;
        }
        VkAccelerationStructureDeviceAddressInfoKHR address_info{};
        address_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        address_info.pNext = nullptr;
        address_info.accelerationStructure = structure->native;
        structure->address = this->acceleration_structure_address(this->logical_device, &address_info);
        deren::utility::log("rhi: create_acceleration_structure: {} level, {} geometries, {} bytes of storage, {} bytes of scratch, address {:#x}",
                            top_level ? "top" : "bottom", structure->geometries.size(), structure->structure_size,
                            structure->scratch_size, static_cast<std::uint64_t>(structure->address));
        return structure;
    }

    rhi::error core::frame_commands::build_micromap(rhi::micromap& target) {
        core* const self = this->owner;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (self->micromap_build == nullptr) {
            return rhi::error::unsupported; // no VK_EXT_opacity_micromap on this device (see the constructor)
        }
        if (target.type() != rhi::interface_type::micromap) {
            return rhi::error::invalid_argument; // a handle this backend did not hand out
        }
        auto& micromap = static_cast<owned_micromap&>(target);
        if (micromap.owner != self || micromap.native == VK_NULL_HANDLE) {
            return rhi::error::invalid_argument;
        }
        // THE BARRIER IS THE BACKEND'S, as it is for an acceleration structure build: the setup buffers were
        // written by the HOST through their mappings, and the micromap itself is written.
        VkMemoryBarrier2 const barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_MICROMAP_BUILD_BIT_EXT,
            .dstAccessMask = VK_ACCESS_2_MICROMAP_READ_BIT_EXT | VK_ACCESS_2_MICROMAP_WRITE_BIT_EXT | VK_ACCESS_2_SHADER_READ_BIT,
        };
        VkDependencyInfo const dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .pNext = nullptr,
                                          .dependencyFlags = 0,
                                          .memoryBarrierCount = 1u,
                                          .pMemoryBarriers = &barrier,
                                          .bufferMemoryBarrierCount = 0u,
                                          .pBufferMemoryBarriers = nullptr,
                                          .imageMemoryBarrierCount = 0u,
                                          .pImageMemoryBarriers = nullptr};
        vkCmdPipelineBarrier2(command_buffer, &dependency);

        VkMicromapBuildInfoEXT info{};
        info.sType = VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT;
        info.pNext = nullptr;
        info.type = VK_MICROMAP_TYPE_OPACITY_MICROMAP_EXT;
        info.flags = VK_BUILD_MICROMAP_PREFER_FAST_TRACE_BIT_EXT;
        info.mode = VK_BUILD_MICROMAP_MODE_BUILD_EXT;
        info.dstMicromap = micromap.native;
        info.usageCountsCount = 1u;
        info.pUsageCounts = &micromap.usage;
        info.data.deviceAddress = micromap.data_address;
        info.triangleArray.deviceAddress = micromap.triangles_address;
        info.triangleArrayStride = micromap.triangle_array_stride;
        info.scratchData.deviceAddress = micromap.scratch_address;
        self->micromap_build(command_buffer, 1u, &info);
        return rhi::error::ok;
    }

    void core::owned_micromap::release() noexcept {
        delete this;
    }

    core::owned_micromap::~owned_micromap() noexcept {
        // THE ORDER IS THE POINT, as it is for an acceleration structure: the micromap first, then the memory it
        // lives in and the setup buffers the build read.
        if (this->native != VK_NULL_HANDLE && this->owner != nullptr && this->owner->micromap_destroy != nullptr) {
            this->owner->micromap_destroy(this->owner->logical_device, this->native, nullptr);
        }
        for (deren::promise::rhi::buffer* const buffer : {this->indices, this->triangles, this->data, this->scratch, this->storage}) {
            if (buffer != nullptr) {
                buffer->release();
            }
        }
    }

    rhi::error core::frame_commands::build_acceleration_structure(rhi::acceleration_structure& target) {
        core* const self = this->owner;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (self->acceleration_structure_build == nullptr) {
            return rhi::error::unsupported; // no VK_KHR_acceleration_structure on this device (see the constructor)
        }
        if (target.type() != rhi::interface_type::acceleration_structure) {
            return rhi::error::invalid_argument; // a handle this backend did not hand out (the provenance rule)
        }
        auto& structure = static_cast<owned_acceleration_structure&>(target);
        if (structure.owner != self) {
            return rhi::error::invalid_argument;
        }
        if (structure.top_level && structure.instance_count == 0) {
            // An empty top level is a legal but useless object; recording a build for it would make the driver
            // read zero instances while `ok` claimed a build happened. Refused by name instead.
            return rhi::error::invalid_argument;
        }
        // THE BARRIER IS THE BACKEND'S NOW (the engine's module used to record its own): everything the build
        // READS was written by the host (the caller's vertex/index/instance writes, and this frame's transfers)
        // and the structure itself is written - one conservative memory barrier covers both.
        VkMemoryBarrier2 const barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        };
        VkDependencyInfo const dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .pNext = nullptr,
                                          .dependencyFlags = 0,
                                          .memoryBarrierCount = 1u,
                                          .pMemoryBarriers = &barrier,
                                          .bufferMemoryBarrierCount = 0u,
                                          .pBufferMemoryBarriers = nullptr,
                                          .imageMemoryBarrierCount = 0u,
                                          .pImageMemoryBarriers = nullptr};
        vkCmdPipelineBarrier2(command_buffer, &dependency);

        VkAccelerationStructureBuildGeometryInfoKHR build_info{};
        build_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        build_info.pNext = nullptr;
        build_info.type = structure.top_level ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        build_info.flags = (structure.top_level ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR) |
                           (structure.refittable ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0u);
        build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build_info.srcAccelerationStructure = VK_NULL_HANDLE;
        build_info.dstAccelerationStructure = structure.native;
        build_info.geometryCount = static_cast<std::uint32_t>(structure.geometries.size());
        build_info.pGeometries = structure.geometries.data();
        build_info.ppGeometries = nullptr;
        build_info.scratchData.deviceAddress = structure.scratch_address;
        VkAccelerationStructureBuildRangeInfoKHR range = structure.range;
        if (structure.top_level) {
            range.primitiveCount = structure.instance_count; // the build reads what the caller last wrote
        }
        VkAccelerationStructureBuildRangeInfoKHR const* ranges[1] = {&range};
        self->acceleration_structure_build(command_buffer, 1u, &build_info, ranges);
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::refit_acceleration_structure(rhi::acceleration_structure& target) {
        core* const self = this->owner;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (self->acceleration_structure_build == nullptr) {
            return rhi::error::unsupported;
        }
        if (target.type() != rhi::interface_type::acceleration_structure) {
            return rhi::error::invalid_argument;
        }
        auto& structure = static_cast<owned_acceleration_structure&>(target);
        if (structure.owner != self) {
            return rhi::error::invalid_argument;
        }
        if (!structure.refittable) {
            // THE ONE REFUSAL THE FLAG EXISTS FOR: a structure built without ALLOW_UPDATE has no in-place mode,
            // and a "refit" that silently rebuilt it would be a different operation than the caller asked for.
            return rhi::error::unsupported;
        }
        VkMemoryBarrier2 const barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
            .pNext = nullptr,
            .srcStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .srcAccessMask = VK_ACCESS_2_HOST_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            .dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            .dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
        };
        VkDependencyInfo const dependency{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                          .pNext = nullptr,
                                          .dependencyFlags = 0,
                                          .memoryBarrierCount = 1u,
                                          .pMemoryBarriers = &barrier,
                                          .bufferMemoryBarrierCount = 0u,
                                          .pBufferMemoryBarriers = nullptr,
                                          .imageMemoryBarrierCount = 0u,
                                          .pImageMemoryBarriers = nullptr};
        vkCmdPipelineBarrier2(command_buffer, &dependency);

        VkAccelerationStructureBuildGeometryInfoKHR build_info{};
        build_info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        build_info.pNext = nullptr;
        build_info.type = structure.top_level ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        build_info.flags = (structure.top_level ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR) |
                           VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR; // the whole difference from a build
        build_info.srcAccelerationStructure = structure.native;
        build_info.dstAccelerationStructure = structure.native;
        build_info.geometryCount = static_cast<std::uint32_t>(structure.geometries.size());
        build_info.pGeometries = structure.geometries.data();
        build_info.ppGeometries = nullptr;
        build_info.scratchData.deviceAddress = structure.scratch_address;
        VkAccelerationStructureBuildRangeInfoKHR range = structure.range;
        if (structure.top_level) {
            range.primitiveCount = structure.instance_count;
        }
        VkAccelerationStructureBuildRangeInfoKHR const* ranges[1] = {&range};
        self->acceleration_structure_build(command_buffer, 1u, &build_info, ranges);
        return rhi::error::ok;
    }

    // ---- the owned acceleration structure's own verbs -----------------------------------------------------

    std::uint64_t core::owned_acceleration_structure::device_address() const noexcept {
        return this->address;
    }

    std::uint64_t core::owned_acceleration_structure::size_bytes() const noexcept {
        return this->structure_size;
    }

    rhi::error core::owned_acceleration_structure::write_instances(std::span<rhi::acceleration_structure_instance const> records) {
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

    void core::owned_acceleration_structure::release() noexcept {
        delete this;
    }

    core::owned_acceleration_structure::~owned_acceleration_structure() noexcept {
        // THE ORDER IS THE POINT (see the type's note): the structure first, then the memory it lives in.
        if (this->native != VK_NULL_HANDLE && this->owner != nullptr && this->owner->acceleration_structure_destroy != nullptr) {
            this->owner->acceleration_structure_destroy(this->owner->logical_device, this->native, nullptr);
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
    rhi::error core::frame_commands::draw_mesh_tasks_indirect(rhi::buffer const& argument_buffer, std::uint64_t const offset, std::uint32_t const count, std::uint32_t const stride) {
        // THE CONTRACT'S RECORD LAYOUT IS THE API'S, AND THIS IS WHERE THAT IS PROVEN: callers write
        // `rhi::mesh_task_command` records into their argument buffer and pass `mesh_task_command_size` as the
        // stride, so the two structures must be the same bytes. The backend is the only side that can assert it
        // (it is the only one that names both).
        static_assert(sizeof(VkDrawMeshTasksIndirectCommandEXT) == rhi::mesh_task_command_size,
                      "the argument buffer's record IS the contract's layout, and the stride its size");
        static_assert(sizeof(VkDrawMeshTasksIndirectCommandEXT) == sizeof(rhi::mesh_task_command),
                      "the contract's record and the API's structure must agree");
        core* const self = this->owner;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (argument_buffer.type() != rhi::interface_type::buffer) {
            return rhi::error::invalid_argument;
        }
        if (self->mesh_dispatch_indirect == nullptr) {
            return rhi::error::unsupported; // no mesh-shader extension entry point on this device
        }
        auto const* const owned = static_cast<owned_buffer const*>(&argument_buffer);
        self->mesh_dispatch_indirect(command_buffer, owned->native, offset, count, stride);
        return rhi::error::ok;
    }

    void core::frame_commands::set_viewport(rhi::viewport const& vp) noexcept {
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return;
        }
        VkViewport const native = {.x = vp.x, .y = vp.y, .width = vp.width, .height = vp.height, .minDepth = vp.min_depth, .maxDepth = vp.max_depth};
        vkCmdSetViewport(command_buffer, 0u, 1u, &native);
    }

    void core::frame_commands::set_scissor(rhi::rect const& scissor) noexcept {
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return;
        }
        VkRect2D const native = {.offset = {scissor.offset_x, scissor.offset_y}, .extent = {scissor.width, scissor.height}};
        vkCmdSetScissor(command_buffer, 0u, 1u, &native);
    }

    void core::frame_commands::set_cull_mode(rhi::cull_mode const mode) noexcept {
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer != VK_NULL_HANDLE) {
            VkCullModeFlags const native = mode == rhi::cull_mode::front  ? VK_CULL_MODE_FRONT_BIT
                                           : mode == rhi::cull_mode::back ? VK_CULL_MODE_BACK_BIT
                                           : mode == rhi::cull_mode::none ? VK_CULL_MODE_NONE
                                                                          : VK_CULL_MODE_FRONT_AND_BACK;
            vkCmdSetCullMode(command_buffer, native);
        }
    }

    void core::frame_commands::set_depth_write(bool const enable) noexcept {
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer != VK_NULL_HANDLE) {
            vkCmdSetDepthWriteEnable(command_buffer, enable ? VK_TRUE : VK_FALSE);
        }
    }

    void core::frame_commands::set_depth_bias(float const constant_factor, float const slope_factor, float const clamp) noexcept {
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer != VK_NULL_HANDLE) {
            // THE TWO ORDERS ARE NOT THE SAME, and this line is where that is spelled once: the CONTRACT states
            // `set_depth_bias(constant, slope, clamp)` (the order a reader of the record series expects, and the
            // order `rhi::command_buffer::set_depth_bias` documents), while VULKAN states
            // `vkCmdSetDepthBias(cb, depthBiasConstantFactor, depthBiasClamp, depthBiasSlopeFactor)`. This call
            // had them as (clamp, slope, constant) - ALL THREE WRONG - and the comment claimed to be Vulkan's own
            // order, so nothing but a frame that reads the number back could catch it. The shadow map's live depth
            // bias is what does: the 14-hash render gate took 12 mismatches the first time the shadow cascade
            // recorded its bias through this verb instead of through vkCmdSetDepthBias directly.
            vkCmdSetDepthBias(command_buffer, constant_factor, clamp, slope_factor);
        }
    }

    rhi::error core::frame_commands::copy_image(rhi::image_copy const& copy) {
        core* const self = this->owner;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        VkImage source_native = VK_NULL_HANDLE;
        VkFormat source_format = VK_FORMAT_UNDEFINED;
        VkImage destination_native = VK_NULL_HANDLE;
        VkFormat destination_format = VK_FORMAT_UNDEFINED;
        {
            std::lock_guard const lock(self->contract_images_mutex);
            if (copy.source == nullptr || copy.destination == nullptr || copy.source->type() != rhi::interface_type::image ||
                copy.destination->type() != rhi::interface_type::image || !self->contract_images.contains(copy.source) ||
                !self->contract_images.contains(copy.destination)) {
                return rhi::error::invalid_argument;
            }
            auto const* const source_owned = static_cast<owned_image const*>(copy.source);
            auto const* const destination_owned = static_cast<owned_image const*>(copy.destination);
            source_native = source_owned->native_handle;
            source_format = source_owned->resolved_format;
            destination_native = destination_owned->native_handle;
            destination_format = destination_owned->resolved_format;
        }
        auto const layer_of = [](rhi::image_copy_region const& region) {
            return VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, region.mip_level, region.base_array_layer,
                                            region.array_layer_count == 0 ? VK_REMAINING_ARRAY_LAYERS : region.array_layer_count};
        };
        VkImageCopy2 const regions = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
            .pNext = nullptr,
            .srcSubresource = layer_of(copy.source_region),
            .srcOffset = {static_cast<std::int32_t>(copy.source_region.offset_x), static_cast<std::int32_t>(copy.source_region.offset_y),
                          static_cast<std::int32_t>(copy.source_region.offset_z)},
            .dstSubresource = layer_of(copy.destination_region),
            .dstOffset = {static_cast<std::int32_t>(copy.destination_region.offset_x), static_cast<std::int32_t>(copy.destination_region.offset_y),
                          static_cast<std::int32_t>(copy.destination_region.offset_z)},
            .extent = {copy.source_region.extent.width, copy.source_region.extent.height, copy.source_region.extent.depth},
        };
        (void)source_format; // both sides live in GENERAL; the aspects are colour (the copy sites' shape)
        (void)destination_format;
        VkCopyImageInfo2 const copy_info = {
            .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
            .pNext = nullptr,
            .srcImage = source_native,
            .srcImageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .dstImage = destination_native,
            .dstImageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .regionCount = 1,
            .pRegions = &regions,
        };
        vkCmdCopyImage2(command_buffer, &copy_info);
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::copy_buffer(rhi::buffer& destination, rhi::buffer const& source, std::uint64_t const size, std::uint64_t const source_offset,
                                                 std::uint64_t const destination_offset) {
        VkCommandBuffer const command_buffer = this->native();
        if (command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (destination.type() != rhi::interface_type::buffer || source.type() != rhi::interface_type::buffer) {
            return rhi::error::invalid_argument;
        }
        auto const* const destination_owned = static_cast<owned_buffer const*>(&destination);
        auto const* const source_owned = static_cast<owned_buffer const*>(&source);
        VkBufferCopy2 const regions = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
            .pNext = nullptr,
            .srcOffset = source_offset,
            .dstOffset = destination_offset,
            .size = size,
        };
        VkCopyBufferInfo2 const copy_info = {
            .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
            .pNext = nullptr,
            .srcBuffer = source_owned->native,
            .dstBuffer = destination_owned->native,
            .regionCount = 1,
            .pRegions = &regions,
        };
        vkCmdCopyBuffer2(command_buffer, &copy_info);
        return rhi::error::ok;
    }

    rhi::error core::frame_commands::clear_color_image(rhi::image const& target, std::array<float, 4> const& color, rhi::subresource_range const& range) {
        core* const self = this->owner;
        VkCommandBuffer const command_buffer = this->native();
        if (self == nullptr || command_buffer == VK_NULL_HANDLE) {
            return rhi::error::not_ready;
        }
        if (target.type() != rhi::interface_type::image) {
            return rhi::error::invalid_argument;
        }
        std::lock_guard const lock(self->contract_images_mutex);
        if (!self->contract_images.contains(&target)) {
            return rhi::error::invalid_argument;
        }
        auto const* const owned = static_cast<owned_image const*>(&target);
        VkClearColorValue const clear = {.float32 = {color[0], color[1], color[2], color[3]}};
        VkImageSubresourceRange const native_range = subresource_of(VK_IMAGE_ASPECT_COLOR_BIT, range);
        vkCmdClearColorImage(command_buffer, owned->native_handle, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &native_range);
        return rhi::error::ok;
    }

    // ---- tier-2: device_address --------------------------------------------------------------------

    std::uint64_t core::buffer_address_view::buffer_address(rhi::buffer const& resource, std::uint64_t const offset) const noexcept {
        // THE PRECONDITION IS THE CONTRACT'S OWN RULE, STATED RATHER THAN GUESSED AT: a caller may only
        // ask about a buffer THIS backend handed out (rhi.api_core.cppm says the same about every handle
        // it gives back). `frame_commands::use()` can afford a stricter check because its views are
        // singletons it compares addresses against; factory-created buffers are heap objects, so
        // recognising "one of mine" without RTTI would mean keeping a registry of them - and this path
        // does not need one, because the answer it computes (`vkGetBufferDeviceAddress`) is only defined
        // for a buffer this device created in the first place. So: a `static_cast` on a documented
        // precondition, not a hopeful assumption.
        auto const* const owned = static_cast<owned_buffer const*>(&resource);
        if (this->owner == nullptr || !owned->addressable) {
            // ASKED FOR NOTHING, GET NOTHING: a buffer created without `buffer_flag::device_address` has
            // no address to report (Vulkan only allows the call for a buffer created with that usage),
            // and 0 is the contract's spelling of "none" - the module's own guard, not a failure.
            return 0ull;
        }
        VkBufferDeviceAddressInfo const query = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .pNext = nullptr, .buffer = owned->native};
        std::uint64_t const address = static_cast<std::uint64_t>(vkGetBufferDeviceAddress(this->owner->logical_device, &query));
        if (address == 0u) {
            deren::utility::log("rhi: buffer_address() answered 0 for native {:#x} (usage flags {:#x}) - the address is only defined when the device enabled "
                                "bufferDeviceAddress, which is what the flag promised",
                                reinterpret_cast<std::uintptr_t>(owned->native), owned->declared_flags);
        }
        return address + offset;
    }

    // ---- tier-2: vulkan_escape ---------------------------------------------------------------------

    void* core::frame_escape::native_instance() const noexcept {
        return reinterpret_cast<void*>(this->owner->instance);
    }

    void* core::frame_escape::native_physical_device() const noexcept {
        return reinterpret_cast<void*>(this->owner->physical_device);
    }

    void* core::frame_escape::native_device() const noexcept {
        return reinterpret_cast<void*>(this->owner->logical_device);
    }

    void* core::frame_escape::native_queue() const noexcept {
        return reinterpret_cast<void*>(this->owner->graphics_queue_handle);
    }

    void* core::frame_escape::native_command_buffer(rhi::command_buffer& commands) const noexcept {
        // THE SAME WINDOW `begin_commands()` ANSWERS IN for the FRAME's list: outside the frame there is
        // no command buffer to name, and answering with "the slot that would be next" would be a lie an
        // escaping pass could record into.
        core& self = *this->owner;
        if (static_cast<void const*>(&commands) == static_cast<void const*>(&self.commands_view)) {
            if (!self.frame_in_flight) {
                return nullptr;
            }
            return reinterpret_cast<void*>(self.frame_command_buffer());
        }
        // ... AND AN OWNED BUFFER'S LIST (abi 15): resolved through the SAME provenance registry
        // `execute()` uses, so no caller pointer is cast before it is known to be one of ours. Outside a
        // frame is legitimate here - a buffer the caller owns exists on its own (the read-back's
        // one-shot buffer is created and recorded after the frame has landed), and it names itself
        // rather than "the slot that would be next".
        std::lock_guard const lock(self.contract_command_buffers_mutex);
        for (deren::promise::rhi::command_buffer const* const candidate : self.contract_command_buffers) {
            auto const* const owned = static_cast<owned_command_buffer const*>(candidate);
            // THE BUFFER **IS** THE LIST NOW (the recording face absorbed `command_list`), so this is an
            // object-identity question rather than the member-address comparison the borrowed view needed:
            // one inheritance chain, so the two pointers name the same object.
            if (static_cast<void const*>(owned) == static_cast<void const*>(&commands)) {
                return reinterpret_cast<void*>(*owned->buffer);
            }
        }
        return nullptr; // not a list this backend handed out
    }

    std::span<char const* const> core::frame_escape::enabled_instance_extensions() const noexcept {
        return std::span<char const* const>(this->owner->instance_extension_names);
    }

    std::span<char const* const> core::frame_escape::enabled_device_extensions() const noexcept {
        return std::span<char const* const>(this->owner->device_extension_names);
    }

    void* core::frame_escape::native_buffer(rhi::buffer const& resource) const noexcept {
        // BORROWED, AND THE SAME PRECONDITION `buffer_address()` STATES: a caller may only ask about a
        // buffer THIS backend handed out (the contract's own rule for every handle it gives back).
        // Without RTTI there is no honest way to check that a heap-allocated factory object is one of
        // ours, so the cast rests on the documented precondition rather than on a hopeful assumption -
        // and a RELEASED buffer must not reach here at all (touching a released handle is a caller bug
        // the contract names; this would turn it into a crash instead of a wrong answer).
        auto const* const owned = static_cast<owned_buffer const*>(&resource);
        return reinterpret_cast<void*>(owned->native);
    }

    void* core::frame_escape::native_image(rhi::image const& resource) const noexcept {
        // THE SAME BORROWED-HANDLE RULE native_buffer states (abi 7's image face): the precondition is
        // the caller's - only images this backend's create_image handed out reach here, and a released
        // one must not. The VkImage was copied into the owned object at creation, so no detail lookup
        // runs per call.
        //
        // THE FRAME IMAGE IS THE ONE EXCEPTION AND IT IS HANDLED BY IDENTITY (③-D/E step 2): the
        // borrowed presentation image is not an `owned_image`, so reinterpreting it as one would read a
        // wrong field - the same pointer-identity test `frame_commands::use` and `fill_heap_bindings`
        // already use for this object. It answers the image the last acquire returned.
        if (static_cast<void const*>(&resource) == static_cast<void const*>(&this->owner->frame_image_view)) {
            return reinterpret_cast<void*>(this->owner->frame_image_view.handle());
        }
        auto const* const owned = static_cast<owned_image const*>(&resource);
        return reinterpret_cast<void*>(owned->native_handle);
    }

    void* core::frame_escape::native_image_view(rhi::image_view const& resource) const noexcept {
        auto const* const owned = static_cast<owned_image_view const*>(&resource);
        return reinterpret_cast<void*>(owned->native_view);
    }

    void* core::frame_escape::native_sampler(rhi::sampler const& resource) const noexcept {
        auto const* const owned = static_cast<owned_sampler const*>(&resource);
        return reinterpret_cast<void*>(owned->native_sampler_handle);
    }

    std::uint32_t core::frame_escape::native_image_format(rhi::image const& resource) const noexcept {
        // THE FRAME IMAGE IS THE ONE BORROWED IMAGE, and it is not an `owned_image` (it is the swapchain's
        // presentation image, which the core owns; abi 16's borrowed view). Asking for its format must
        // answer the SWAPCHAIN's format rather than reinterpret the borrowed object as an owned one - the
        // same pointer-identity test `fill_heap_bindings` and `frame_commands::use` already use for this
        // object. `owned_image::resolved_format` and the swapchain's format are the same value for it by
        // construction (the backend created the images from that format).
        if (static_cast<void const*>(&resource) == static_cast<void const*>(&this->owner->frame_image_view)) {
            return static_cast<std::uint32_t>(this->owner->swap_chain_image_format);
        }
        auto const* const owned = static_cast<owned_image const*>(&resource);
        return static_cast<std::uint32_t>(owned->resolved_format);
    }

    std::uint32_t core::frame_escape::native_swapchain_image_format() const noexcept {
        // abi 17 (③-D/E step 2): the SESSION-STABLE surface format, answerable before any frame exists -
        // which is the whole reason it is on the escape rather than on `native_image_format`, whose only
        // swapchain-image operand (`frame_image()`) is nullptr until an acquire. UNDEFINED before the
        // swapchain is built; the engine's debug overlay and its presentation-drawing pipelines are
        // created after construction, so they see the real value.
        return static_cast<std::uint32_t>(this->owner->swap_chain_image_format);
    }

    void* core::frame_escape::native_pipeline(rhi::pipeline const& resource) const noexcept {
        auto const* const owned = static_cast<owned_pipeline const*>(&resource);
        return reinterpret_cast<void*>(owned->native_handle);
    }

    void* core::frame_escape::native_shader_module(rhi::shader const& resource) const noexcept {
        auto const* const owned = static_cast<owned_shader const*>(&resource);
        return reinterpret_cast<void*>(owned->native_handle);
    }

    // ---- abi 22: the BASIC-HANDLE basis ------------------------------------------------------------
    //
    // THE TOKEN CARRIES THE TAG AND NOTHING ELSE (see `api_basis`): what a caller passes is "the device this
    // work belongs to", and the two escape methods below take it back and read the device from THIS core's own
    // state. That is the whole reason the base has no interface - the handle never travels in it, so nothing
    // about it can go stale, and an implementer of the contract owes the type nothing at all.
    rhi::api_basis* core::get_basis() noexcept {
        return &this->basis_object;
    }

    bool core::owns_basis(rhi::api_basis const& basis) const noexcept {
        // THE CHECK IS THE TAG, NOT A dynamic_cast: this build is `-fno-rtti`, and the tag is the stronger
        // question anyway (it says "a Vulkan device basis", which is exactly what the methods below need to
        // know). A token from a NON-Vulkan backend is refused here; a token from another Vulkan core in the same
        // process is indistinguishable by tag and is resolved against THIS core's device - the engine obtains
        // the token from the face it is recording with, so a mixed-core call would be a caller bug, and the
        // failure it can produce is bounded: a device proc resolved on the wrong device, not a mis-cast.
        return basis.s_type == rhi::structure_type::vulkan_device_basis;
    }

    rhi::api_basis* core::frame_escape::get_basis() const noexcept {
        return this->owner != nullptr ? this->owner->get_basis() : nullptr;
    }

    bool core::frame_escape::shader_group_handles(rhi::api_basis& basis, rhi::pipeline const& resource, std::uint32_t const first_group, std::uint32_t const group_count,
                                                  std::span<std::uint8_t> const out) const noexcept {
        core* const self = this->owner;
        if (self == nullptr || !self->owns_basis(basis) || self->logical_device == VK_NULL_HANDLE) {
            return false;
        }
        void* const native = this->native_pipeline(resource);
        if (native == nullptr || out.empty()) {
            return false;
        }
        auto const query = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(vkGetDeviceProcAddr(self->logical_device, "vkGetRayTracingShaderGroupHandlesKHR"));
        if (query == nullptr) {
            return false;
        }
        // `out.size()` IS THE QUERY'S dataSize, exactly as Vulkan wants it: the CALLER sized the destination from
        // the device's own handle size (the pass reads it off `ray_tracing_properties`), so this side needs no
        // second opinion about the layout - and a caller that sized it wrong gets a failed call rather than a
        // truncated write.
        return query(self->logical_device, static_cast<VkPipeline>(native), first_group, group_count, out.size(), out.data()) == VK_SUCCESS;
    }

    // ---- tier-2 host_image_copy (③-D/E step 2) ------------------------------------------------------
    //
    // THE ONE MECHANISM, IN ONE PLACE (abi 25): the ability's span-shaped verb and `image::get_content()`
    // are two SPELLINGS of the same host copy - `vkCopyImageToMemoryEXT`, with no staging buffer, no copy
    // command and no submission - so both build the same `VkImageToMemoryCopy` through this helper. A
    // device fact (`host_image_copy_available`) gates both, which is what keeps them from disagreeing.
    namespace {
        [[nodiscard]] rhi::error host_copy_image_out(core& self, VkImage const source, std::span<std::byte> const destination,
                                                     rhi::image_copy_region const& region) noexcept {
            if (!self.host_image_copy_available || source == VK_NULL_HANDLE) {
                return rhi::error::unsupported;
            }
            // THE REGION IS THE CALLER'S, TIGHTLY PACKED: the contract's `image_copy_region` says WHERE in
            // the image (texels, subresource, offsets) and this maps it onto the API's own structure with the
            // row/image strides left zero - which is the API's own spelling of "the region is tightly
            // packed", i.e. exactly extent.width texels per row and extent.height rows deep. The aspect is
            // COLOUR: the contract's region carries no aspect, and every image this renderer copies out of
            // is a colour one.
            VkImageToMemoryCopy memory_copy = {};
            memory_copy.sType = VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY_EXT;
            memory_copy.pNext = nullptr;
            memory_copy.pHostPointer = destination.data();
            memory_copy.memoryRowLength = 0;
            memory_copy.memoryImageHeight = 0;
            // THE CONVERSIONS ARE EXPLICIT because the API's own structure is less wide here than the
            // contract's region: `mipLevel` and the offsets are `int32_t` in Vulkan and `uint32_t` in the
            // region. A braced initializer would refuse the narrowing outright (-Wc++11-narrowing), which is
            // the compiler asking for the cast - and a region whose values do not fit is a caller bug the
            // backend cannot repair.
            memory_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            memory_copy.imageSubresource.mipLevel = static_cast<std::int32_t>(region.mip_level);
            memory_copy.imageSubresource.baseArrayLayer = region.base_array_layer;
            memory_copy.imageSubresource.layerCount = region.array_layer_count;
            memory_copy.imageOffset = {static_cast<std::int32_t>(region.offset_x),
                                       static_cast<std::int32_t>(region.offset_y),
                                       static_cast<std::int32_t>(region.offset_z)};
            memory_copy.imageExtent = {region.extent.width, region.extent.height, region.extent.depth};
            VkCopyImageToMemoryInfo copy_info = {};
            copy_info.sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO_EXT;
            copy_info.pNext = nullptr;
            copy_info.flags = 0;
            copy_info.srcImage = source;
            // GENERAL, AND THAT IS THIS RENDERER'S OWN CONVENTION rather than a choice made here: every image
            // it creates is kept in GENERAL (the heap-native shaders index the grid directly), and the
            // device's host-copy source layouts list GENERAL among what it can read from - which is exactly
            // what `host_image_copy_available` verifies at startup.
            copy_info.srcImageLayout = VK_IMAGE_LAYOUT_GENERAL;
            copy_info.regionCount = 1;
            copy_info.pRegions = &memory_copy;
            return generic_error(self.copy_image_to_memory(self.logical_device, &copy_info));
        }
    } // namespace

    // ---- tier-2 device_capabilities: THE DEVICE'S OWN FACTS, ANSWERED FROM WHAT THE CONSTRUCTOR QUERIED ----
    //
    // NOT ONE OF THESE METHODS ASKS THE DEVICE ANYTHING. Every value is a member the constructor filled while it
    // decided what to enable (`gate G2` in init_utils.cppm verified the four-part capability sets), so the
    // engine's questions are answered from the record of the decision rather than by repeating its queries -
    // which is also what makes the answers UNABLE to disagree with the enabling they describe.
    //
    // A MISSING DEVICE (`owner == nullptr`, a released core) answers the same defaults an unqueried device
    // struct would have: false for the features, zero for the numbers - the callers' own guard rails treat a
    // zero limit as "visibly not a device" (the note `physical_properties_of` used to carry).
    bool core::frame_device_capabilities::mesh_shader() const noexcept {
        return this->owner != nullptr && this->owner->mesh_shader_available;
    }

    bool core::frame_device_capabilities::ray_query() const noexcept {
        return this->owner != nullptr && this->owner->ray_query_available;
    }

    std::uint32_t core::frame_device_capabilities::max_push_constants_size() const noexcept {
        return this->owner == nullptr ? 0u : this->owner->device_properties.limits.maxPushConstantsSize;
    }

    std::uint32_t core::frame_device_capabilities::graphics_queue_family() const noexcept {
        return this->owner == nullptr ? 0u : this->owner->graphics_queue_family_index;
    }

    rhi::shader_binding_table_properties core::frame_device_capabilities::shader_binding_table() const noexcept {
        if (this->owner == nullptr) {
            return {};
        }
        auto const& properties = this->owner->ray_tracing_pipeline_properties;
        return rhi::shader_binding_table_properties{.handle_size = properties.shaderGroupHandleSize,
                                                    .handle_alignment = properties.shaderGroupHandleAlignment,
                                                    .base_alignment = properties.shaderGroupBaseAlignment};
    }

    std::uint64_t core::frame_device_capabilities::acceleration_structure_scratch_alignment() const noexcept {
        return this->owner == nullptr ? 0u : this->owner->acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment;
    }

    std::uint64_t core::frame_device_capabilities::max_acceleration_structure_instances() const noexcept {
        return this->owner == nullptr ? 0u : this->owner->acceleration_structure_properties.maxInstanceCount;
    }

    rhi::error core::frame_host_copy::copy_image_to_memory(rhi::image const& source, std::span<std::byte> destination,
                                                           rhi::image_copy_region const& region) noexcept {
        core* const self = this->owner;
        if (self == nullptr || !self->host_image_copy_available) {
            // The ability is announced ONLY when this holds, so a caller that checked `abilities()` never
            // sees this; one that did not gets the contract's named refusal rather than a call through a
            // null function pointer.
            return rhi::error::unsupported;
        }
        // ONLY AN IMAGE THIS BACKEND CREATED, which is the contract's precondition for every borrowed
        // native handle. The frame image (a BORROWED view) is NOT served HERE, and that is deliberate: this
        // ability's contract is "an image a caller made", and reinterpreting the borrowed object as an owned
        // one is the bug the escape's pointer-identity tests exist to avoid. The FRAME image has its own
        // spelling now (`frame_image_slot::get_content()`), so nothing is lost by keeping this refusal.
        if (static_cast<void const*>(&source) == static_cast<void const*>(&self->frame_image_view)) {
            return rhi::error::invalid_argument;
        }
        auto const* const owned = static_cast<owned_image const*>(&source);
        if (owned->native_handle == VK_NULL_HANDLE) {
            return rhi::error::invalid_argument;
        }
        return host_copy_image_out(*self, owned->native_handle, destination, region);
    }

    // ---- image::get_content (abi 25): THE CONTENT, NOT A HANDLE --------------------------------------
    std::expected<rhi::image_content, rhi::error> core::owned_image::get_content(rhi::image_copy_region const& region) const {
        core* const self = this->owner;
        if (self == nullptr || this->native_handle == VK_NULL_HANDLE) {
            return std::unexpected(rhi::error::invalid_argument);
        }
        // THE IMAGE MUST HAVE BEEN MADE FOR HOST TRANSFER: the contract's `image_flag::host_transfer`, which
        // the descriptor declared and this backend kept in `declared_flags`. An image without it is refused by
        // nature (VUID-vkCopyImageToMemoryEXT-srcImage-09460), so the answer is a NAMED error, not the call.
        if (!rhi::has_flag(this->declared_flags, rhi::image_flag::host_transfer) || !self->host_image_copy_available) {
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
        if (this->owner == nullptr) {
            return std::unexpected(rhi::error::invalid_argument);
        }
        return std::unexpected(rhi::error::unsupported);
    }

    VkResult core::acquire_next_image(uint32_t& image_index) {
        // The frame slot in progress: submit() and wait_frame_slot() use `current_frame` for it, and
        // to_next_frame() advances it, so the acquire semaphore below is the one that slot's submission
        // will wait on (VUID-vkAcquireNextImageKHR-semaphore-01779 needs the slot to be idle - the
        // caller's wait_frame_slot() is what guarantees that).
        uint32_t const slot = static_cast<uint32_t>(this->current_frame);
        VkResult const result = vkAcquireNextImageKHR(this->logical_device, this->swap_chain, UINT64_MAX,
                                                      this->image_available_semaphores[slot], VK_NULL_HANDLE, &image_index);
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
            // THE FRAME IN FLIGHT IS NOW THIS IMAGE, and that is all `frame_in_flight` means: from here
            // until submit() hands the frame over, `begin_commands()` and `frame_image()` answer for it.
            // Both spellings go through this one primitive - the contract's frame_begin() and the
            // runtime's own pacing path - so neither can be "in flight" without the other knowing.
            this->acquired_image_index = image_index;
            this->frame_in_flight = true;
            // and this is what makes `frame_image()` answer at all: before the first successful acquire
            // there is no last-acquired image, only a default index that happens to name one.
            this->frame_acquired = true;
        }
        return result;
    }

    rhi::submit_info core::frame_begin() {
        uint32_t image_index = 0;
        VkResult const acquired = this->acquire_next_image(image_index);
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
            // tier-1's frame_begin() has no error channel (plan §3.3): a zeroed answer is the contract's
            // way of saying "no frame started", and the caller's own pacing path owns the diagnosis.
            return rhi::submit_info{};
        }
        // The contract's "index in the frame-in-flight ring" IS this core's frame slot.
        return rhi::submit_info{.frame_index = static_cast<std::uint32_t>(this->current_frame), .image_index = image_index};
    }

    rhi::error core::present() {
        // The contract's present() has no argument: it presents the image its own acquire took, and
        // the PRESENT call site's translation answers (out_of_date => the caller rebuilds; a failed
        // presentation never reports silence - the abi 14 note on the verb).
        //
        // NO ACQUIRED FRAME IS A NAMED REFUSAL, not a queue call: `acquired_image_index` starts at 0,
        // which NAMES a real image, so forwarding it before any acquire would reach
        // vkQueuePresentKHR waiting on a present-ready semaphore nothing has signalled (and present
        // image 0, which this frame never wrote). `frame_acquired` is what tells "the last acquire"
        // from "there has never been one" - the same flag `frame_image()` answers on - so a caller
        // that presents before opening a frame is told `not_ready` instead of being run into the WSI.
        if (!this->frame_acquired) {
            return rhi::error::not_ready;
        }
        return this->present(this->acquired_image_index);
    }

    void core::wait_idle() {
        // The const facade call (vulkan/core/core.cpp) is the implementation; the contract's virtual is
        // not const, so this overload exists to forward into it.
        core const& self = *this;
        self.wait_idle();
    }

    rhi::swapchain* core::frame_swapchain() noexcept {
        return &this->swapchain_view_;
    }

    rhi::error core::submit(rhi::command_buffer& commands) {
        // THE FRAME'S OWN LIST first, which is the shape this verb had from the start.
        if (&commands != static_cast<rhi::command_buffer*>(&this->commands_view)) {
            // ---- AND THE CALLER'S OWN LIST (plan X4) ------------------------------------------------------
            //
            // WHAT THIS WIDENING IS FOR, and it is a WIDENING rather than a new slot (the signature does not
            // move, so abi 27 stands): the probes record their read-back into a buffer THEY created
            // (`create_command_buffer()`), and they handed it to the queue through the escape with a raw
            // `vkQueueSubmit` - the last Vulkan reference the engine half had. A one-shot list the caller owns is
            // the same operation the frame's list is (record -> submit to the graphics queue); what differs is
            // that NOTHING IS PRESENTED, so the acquire state, the swapchain image and the semaphores are not
            // involved. THE ORDERING STAYS THE CALLER'S (`wait_idle()`), exactly as it was when it submitted by
            // hand - the contract has no fence for an owned buffer, and inventing one is not this batch's call.
            {
                std::lock_guard const lock(this->contract_command_buffers_mutex);
                if (!this->contract_command_buffers.contains(&commands)) {
                    return rhi::error::invalid_argument; // a list this backend did not hand out (provenance)
                }
            }
            auto const& owned = static_cast<core::owned_command_buffer const&>(commands);
            VkCommandBuffer const native = *owned.buffer;
            if (native == VK_NULL_HANDLE || this->graphics_queue_handle == VK_NULL_HANDLE) {
                return rhi::error::not_ready;
            }
            VkSubmitInfo const one_shot = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                           .pNext = nullptr,
                                           .waitSemaphoreCount = 0,
                                           .pWaitSemaphores = nullptr,
                                           .pWaitDstStageMask = nullptr,
                                           .commandBufferCount = 1,
                                           .pCommandBuffers = &native,
                                           .signalSemaphoreCount = 0,
                                           .pSignalSemaphores = nullptr};
            return generic_error(vkQueueSubmit(this->graphics_queue_handle, 1, &one_shot, VK_NULL_HANDLE));
        }
        if (!this->frame_in_flight) {
            return rhi::error::not_ready;
        }
        // The image and the present-ready semaphore are this backend's own acquire state - never
        // caller data (the contract's note). The raw failure becomes the generic translation; the
        // device-level codes travel as themselves.
        return generic_error(this->submit_frame(this->frame_command_buffer(), this->acquired_image_index));
    }

    // ---- the frame face (abi 13) ----------------------------------------------------------------
    //
    // THE ACCESSORS hand out this core's two borrowed views - the same object every call, the
    // constructor set their `owner`. The views' methods compose the machinery this class already
    // owned: the walker is today's frame prologue fused (wait -> latch timings -> acquire -> report),
    // the profiler reads the timing snapshot the walker's latch produced.

    rhi::frame_walker* core::walk_frames() noexcept {
        return &this->frames_view;
    }

    rhi::gpu_profiler* core::profiler() noexcept {
        return &this->profiler_view;
    }

    std::uint32_t core::frame_walker_view::slot_count() const noexcept {
        return static_cast<std::uint32_t>(this->owner->MAX_FRAMES_IN_FLIGHT);
    }

    std::uint32_t core::frame_walker_view::position() const noexcept {
        // THE one authority: this is the backend's own cursor, read without advancing.
        return static_cast<std::uint32_t>(this->owner->current_frame);
    }

    rhi::frame_open_info core::frame_walker_view::wait_and_acquire() {
        // THE FIVE STEPS in the order the contract's note spells out - each position is load-bearing.
        uint32_t const slot = static_cast<uint32_t>(this->owner->current_frame);

        // 1 + 2. the cursor (read, not advanced) and its timeline. A never-submitted slot (value 0)
        // has nothing to wait for - VK_SUCCESS by definition, the same early return as today.
        VkResult const waited = this->owner->wait_frame_slot_result(slot);
        if (waited != VK_SUCCESS) {
            // The wait's VkResult, REPORTED instead of dropped (today `wait_frame_slot` voids it):
            // the code says what class of failure, the message names the step, native_code carries
            // the raw VkResult for whoever needs the exact number.
            return {.frame = {}, .result = deren::vulkan::failed(deren::vulkan::generic_error(waited), waited, "waiting the frame slot's timeline failed")};
        }

        // 3. LATCH this slot's previous frame's GPU timings, here between the wait and the acquire -
        // exactly where the engine's collect_gpu_timings sits today, so the wait already guarantees
        // the timestamps are readable AND a frame the acquire kills on OUT_OF_DATE is still
        // collected. Behaviour unchanged; the position is the backend's now.
        this->owner->profiler_view.latch(slot);

        // 4. the acquire - the same primitive `frame_begin()` goes through (one mechanism, two
        // spellings), translated AT THIS CALL SITE: SUBOPTIMAL is ok here (the acquired image
        // renders), OUT_OF_DATE is the rebuild signal the caller's classifier acts on.
        uint32_t image_index = 0;
        VkResult const acquired = this->owner->acquire_next_image(image_index);
        rhi::error const opened = deren::vulkan::acquire_error(acquired);
        if (opened != rhi::error::ok) {
            // The zero-frame rule: `frame` is zeroed and unusable, and the caller now knows WHY
            // (out_of_date => rebuild and skip; device_lost => fatal; ...) - the information
            // `frame_begin()`'s zeroed submit_info could not carry.
            return {.frame = {}, .result = deren::vulkan::failed(opened, acquired, "acquiring the next swapchain image failed")};
        }

        // 5. the frame is open: this slot, the acquired image, and a zeroed diagnostic saying ok.
        return {.frame = {.frame_index = slot, .image_index = image_index}, .result = {}};
    }

    void core::frame_walker_view::walk_to_next() noexcept {
        // The present-side close: advance ONE slot. It does not promise "the frame is over" - only
        // that the ring moved (the engine calls this after its present recipe).
        this->owner->to_next_frame();
    }

    void core::gpu_profiler_view::latch(uint32_t const slot) noexcept {
        // Non-const on purpose: the latch writes the read-once guard (a slot's timings are fetched
        // at most once per submission), the same bookkeeping read_gpu_timings keeps.
        core& self = *this->owner;
        this->latch_failed = false;
        if (!self.gpu_timing_supported) {
            this->latched_mark_count = 0; // the device cannot timestamp: the report is honestly empty
            return;
        }
        uint64_t const submitted = self.frame_done_values[slot];
        if (submitted == 0 || submitted <= self.gpu_timing_read_value[slot]) {
            return; // nothing new since the previous latch: the snapshot stays as it was
        }
        self.gpu_timing_read_value[slot] = submitted; // this submission is now accounted for (once)
        uint32_t const marks = self.gpu_timing_marks[slot];
        this->latched_mark_count = marks;
        if (marks == 0) {
            return; // a submission with no marks has nothing to fetch
        }
        std::array<uint64_t, gpu_timing_mark_capacity> ticks = {};
        VkResult const status = vkGetQueryPoolResults(self.logical_device, self.timestamp_query_pool, slot * gpu_timing_mark_capacity, marks,
                                                      sizeof(uint64_t) * marks, ticks.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        if (status == VK_NOT_READY) {
            // The timeline wait already said the submission completed, so this is the same defensive
            // reading read_gpu_timings has: report no measurement rather than waiting or stalling.
            this->latched_mark_count = 0;
            return;
        }
        if (status != VK_SUCCESS) {
            // A real read-back failure: the mark count is host-known, the ticks are not - the
            // getters answer `device_lost` for it instead of reporting silence.
            this->latch_failed = true;
            return;
        }
        this->latched_ticks = ticks;
        for (uint32_t mark = 0; mark < marks; ++mark) {
            // The names are by-value string_views of the engine's static text (the `window_title`
            // rule), copied out at latch time - the array below may be overwritten by the next
            // recording, the snapshot must not move with it.
            this->latched_names[mark] = self.gpu_timing_names[slot][mark];
        }
    }

    std::uint32_t core::gpu_profiler_view::stage_count() const noexcept {
        if (!this->owner->gpu_timing_supported) {
            return 0; // "no timing => always 0": the caller reads this as unsupported via get_stage_info
        }
        return this->latched_mark_count;
    }

    rhi::error core::gpu_profiler_view::get_stage_info(uint32_t const index, std::string_view* const name,
                                                       uint64_t* const duration_ns) const noexcept {
        if (!this->owner->gpu_timing_supported) {
            return rhi::error::unsupported; // the device or configuration has no timing to report
        }
        if (index >= this->latched_mark_count) {
            return rhi::error::invalid_argument;
        }
        if (this->latch_failed) {
            return rhi::error::device_lost; // the timestamp read-back failed; the count is all we know
        }
        if (index + 1 >= this->latched_mark_count) {
            return rhi::error::not_ready; // the last mark closes the frame and opens no stage
        }
        if (name != nullptr) {
            *name = this->latched_names[index];
        }
        if (duration_ns != nullptr) {
            // The same masking the milliseconds reader does: the counter is a modulo-2^valid_bits
            // ring, so the delta is taken inside the width (a wrap inside the span comes out right),
            // and the device's timestampPeriod converts ticks to nanoseconds (1 ns/tick with 64
            // valid bits on the device this backend ships on - the integer is lossless there).
            uint64_t const mask = this->owner->timestamp_valid_bits >= 64 ? ~uint64_t{0} : ((uint64_t{1} << this->owner->timestamp_valid_bits) - 1);
            uint64_t const delta = (this->latched_ticks[index + 1] - this->latched_ticks[index]) & mask;
            *duration_ns = static_cast<uint64_t>(static_cast<double>(delta) * static_cast<double>(this->owner->timestamp_period_ns));
        }
        return rhi::error::ok;
    }

} // namespace deren::vulkan
