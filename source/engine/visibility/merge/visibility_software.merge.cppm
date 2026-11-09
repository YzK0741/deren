// ============================================================================
// module: deren.engine.visibility.software:merge  (the resolve/merge contract:
//         one software pixel, the winner rule, and the merged depth)
//
// One of the domains of deren.engine.visibility.software; the top-level
// facade re-exports it, so consumers keep importing the same module.
// ============================================================================

module;
#include <cstdint>

export module deren.engine.visibility.software:merge;
export import deren.vstd;

import deren.engine.visibility;

export namespace deren::engine::visibility::software {
    // ================================================================================================
    // THE RESOLVE/MERGE CONTRACT (spec §3.2): one software pixel, the winner rule BEFORE the merge, and the
    // merged depth the merge pass leaves behind
    //
    // THE ORDER IS THE CORRECTNESS ARGUMENT. The resolve runs on the UNMERGED hardware depth. Its winner rule is
    //   software wins iff sw_depth < hw_depth                       (strictly nearer)
    //                  or sw_depth == hw_depth and !hw_id.valid()   (the hardware pixel is background/clear)
    // and it uses the WINNING candidate's depth for the reconstruction/tolerance check, so a software pixel's
    // z-check compares the software id's reconstructed z against the software raster's depth. If the merge ran
    // FIRST, software-won pixels would have their depth overwritten by sw_depth and the strict `<` could no longer
    // see the old hardware depth (the design review's blocking defect, spec §3.2/§5.1). The merge is a depth-only
    // pass with LESS_OR_EQUAL + depth write whose final value is therefore min(hw_depth, sw_depth).
    //
    // COMPARISON IS EXACT (no epsilon), per spec: the two producers can disagree by one ULP at a nominally equal
    // depth, which the strict `<` resolves deterministically; the merge writes the smaller bit pattern, and for
    // positive floats the bit order IS the float order.
    // ================================================================================================
    /// the software images' "no software geometry wrote this pixel" values: depth = a bit pattern larger than any
    /// legal positive float (so integer min selection keeps it last), id = the cleared id (0)
    inline constexpr std::uint32_t software_depth_empty = 0xFFFFFFFFu;
    inline constexpr std::uint32_t software_id_empty = 0u;

    /// one software pixel: NDC depth as an INTEGER (float bit pattern) + the packed 32-bit visibility id
    struct software_pixel {
        std::uint32_t depth_bits = software_depth_empty;
        std::uint32_t id = software_id_empty;
    };

    /// @brief whether a software pixel names geometry with a usable depth (empty id / non-finite / out-of-range fail)
    /// @note spec §5.4: a non-finite or out-of-[0,1] software depth is treated as EMPTY (skipped) - the design never
    ///       writes a NaN into the depth attachment
    [[nodiscard]] inline bool software_pixel_usable(software_pixel const& pixel) noexcept {
        if (pixel.id == software_id_empty) {
            return false;
        }
        float const depth = std::bit_cast<float>(pixel.depth_bits);
        return std::isfinite(depth) && depth >= 0.0f && depth <= 1.0f;
    }

    enum class candidate_source : std::uint8_t { hardware,
                                                 software };

    /// the resolve's choice for one pixel: which producer's id it decodes and which depth it reconstructs with
    struct resolved_candidate {
        candidate_source source = candidate_source::hardware;
        std::uint32_t id = 0u; ///< the packed id to decode (hardware's or software's)
        float depth = 0.0f;    ///< the depth the reconstruction/tolerance check must use (the WINNER's depth)
    };

    /**
     * @brief THE WINNER RULE (spec §3.2), evaluated BEFORE the merge on the unmerged hardware depth
     * @param hardware_id the hardware visibility id (its valid bit distinguishes "background" from "geometry")
     * @param hardware_depth the unmerged G-buffer depth the hardware raster wrote for this pixel
     * @param software the software producer's pixel (id + depth bits)
     * @note software wins on a STRICTLY smaller depth, or on an exact tie when the hardware pixel is background
     *       (invalid id). Every other case - including a software pixel that is empty, non-finite or out of [0,1] -
     *       keeps hardware. The returned depth is the chosen candidate's depth (spec §5.3).
     */
    [[nodiscard]] inline resolved_candidate resolve_candidate(visibility::id const hardware_id, float const hardware_depth,
                                                              software_pixel const software) noexcept {
        resolved_candidate chosen{.source = candidate_source::hardware, .id = hardware_id.value, .depth = hardware_depth};
        if (!software_pixel_usable(software)) {
            return chosen;
        }
        float const software_depth = std::bit_cast<float>(software.depth_bits);
        bool const strictly_nearer = software_depth < hardware_depth;
        bool const background_tie = software_depth == hardware_depth && !hardware_id.valid();
        if (strictly_nearer || background_tie) {
            chosen = resolved_candidate{.source = candidate_source::software, .id = software.id, .depth = software_depth};
        }
        return chosen;
    }

    /**
     * @brief the depth the MERGE pass leaves in the depth attachment: `min(hardware, software)`
     * @param hardware_depth_bits the depth attachment's current value (the hardware producer's, bit pattern)
     * @param software the software pixel
     * @note the merge pipeline is LESS_OR_EQUAL + depth write on `clamp(asfloat(sw_depth_bits),0,1)`, so it writes
     *       only when `sw_depth <= hw_depth`; the resulting value is `min`. Equal writes the same value (the spec's
     *       "tie writes same value, no change"); for positive floats the numeric min is the bit-pattern min, which is
     *       what makes this CPU reference exact.
     */
    [[nodiscard]] inline std::uint32_t merged_depth_bits(std::uint32_t const hardware_depth_bits, software_pixel const software) noexcept {
        if (!software_pixel_usable(software)) {
            return hardware_depth_bits;
        }
        return software.depth_bits < hardware_depth_bits ? software.depth_bits : hardware_depth_bits;
    }

} // namespace deren::engine::visibility::software
