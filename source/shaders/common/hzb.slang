// THE HZB LAYOUT, shared by the build stage (and, from phase B on, the cull stage).
//
// THE TWO FORMULAS IN THIS FILE ARE ONE CONTRACT WITH THE HOST, and the contract has a name:
// `deren.engine.visibility.hzb` (source/engine/visibility/hzb.cppm). The host SIZES the
// `heap_slots_hzb_levels` buffer with `pyramid_words(extent)` and the headless test pins that formula; this
// file is the shader's copy of the same recurrence, and a drift between the two is not a validation error -
// it is a build pass writing level L to words the cull pass reads as level L-1, i.e. a wrong picture. The
// spec (docs/superpowers/specs/2026-10-20-hzb-occlusion-design.md §4.1) asks for exactly this note:
//
//   level 0  = the depth image verbatim (one NDC depth per pixel),
//   level L  = 2x2 MAX of level L-1, width/height = ceil(previous / 2); the odd boundary REPLICATES its
//              edge sample (conservative: a larger max can only make the occlusion test keep more),
//   lane s   = [level 0][level 1][...][level Lmax] packed contiguously inside ONE storage buffer.
//
// PACKED AS A BUFFER, NOT A MIP CHAIN, and §4.1 says why: under VK_EXT_descriptor_heap an image descriptor
// IS one view, a storage view covers a mip RANGE and RWTexture2D cannot name a mip, so N levels would need N
// descriptors (and N heap slots). A buffer needs ONE descriptor and the offsets below.
//
// THE PER-FRAME LANE IS INTERNAL, NOT `+ heap_frame_slot`: the host writes ONE descriptor per table whose
// bytes hold `frame_slots` lanes, so a shader reads `heap_at<...>(heap_slots_hzb_levels)` and indexes
// `frame_slot * hzb_pyramid_words(width, height) + ...`. Adding `heap_frame_slot` to the SLOT would read
// another table's descriptor - a wrong picture rather than an error (render_layout.cppm's slot comments
// repeat this warning on the host side).
#ifndef HZB_SLANG
#define HZB_SLANG

// how many levels ONE dispatch run can build: 16 halvings reach 1x1 from any width/height up to 32768, i.e.
// every swapchain this renderer can create. Mirrors `hzb_max_levels` in deren.engine.visibility.hzb.
static const uint hzb_max_levels = 16u;
// a pyramid word is one float32 NDC depth (the D32 format's own bits)
static const uint hzb_word_bytes = 4u;

// ---- THE HEADER (spec §4.1): 256 B per frame lane, written by the build pass ---------------------------------
//
// ONE HZBHeader PER FRAME LANE, written by the SAME dispatch that writes level 0 - the pyramid and the
// matrices that built it can therefore never disagree (§4.1). The matrices are a SNAPSHOT of the camera UBO
// lane as this frame's build read it: the host's per-frame camera UBO is overwritten before the NEXT frame
// records (spec C21), so "the matrices that built this HZB" have to be stored beside it rather than re-read
// later. It is plain u32 words rather than a struct because every consumer here wants either integers
// (magic/extent/levels) or exact copies of float bits (view/proj), and a struct with matrices inside a
// structured buffer would put the shader's matrix-majorness rules between the header and the C++ side.
static const uint hzb_header_bytes = 256u;
static const uint hzb_header_words = hzb_header_bytes / hzb_word_bytes;
// "HZB", version 1 - the probe script reads it back and refuses a header that is not this build's
static const uint hzb_magic = 0x485A4201u;
// the word offsets inside ONE header lane (each of them multiplied from the constants above so the table
// reads as one layout rather than sixteen magic numbers)
static const uint hzb_header_word_magic = 0u;
static const uint hzb_header_word_width = 1u;
static const uint hzb_header_word_height = 2u;
static const uint hzb_header_word_levels = 3u;
static const uint hzb_header_word_view = 4u;          // 16 words: the glm::mat4's own bytes
static const uint hzb_header_word_proj = 20u;         // 16 words
static const uint hzb_header_word_camera_pos = 36u;   // 3 words; word 39 is the vec4 pad slot
static const uint hzb_header_word_reserved_begin = 40u; // .. 63: zeroed by the writer

/// the first word of @p slot's header lane (the buffer is ONE descriptor holding `frame_slots` lanes)
uint hzb_header_lane_base(uint frame_slot)
{
    return frame_slot * hzb_header_words;
}

// ---- THE PYRAMID LAYOUT: the mirror of deren.engine.visibility.hzb's level_width/height/count/offset -------

/// level @p level's width: `ceil(w / 2)` applied @p level times (integer `(w + 1) / 2`)
uint hzb_level_width(uint width, uint level)
{
    uint result = width;
    for (uint step = 0u; step < level; ++step)
    {
        result = (result + 1u) / 2u;
    }
    return result;
}

/// level @p level's height (same recurrence as @ref hzb_level_width)
uint hzb_level_height(uint height, uint level)
{
    uint result = height;
    for (uint step = 0u; step < level; ++step)
    {
        result = (result + 1u) / 2u;
    }
    return result;
}

/// the pyramid's level count: level 0 plus one per halving until 1x1 (0 for a degenerate extent)
uint hzb_level_count(uint width, uint height)
{
    if (width == 0u || height == 0u)
    {
        return 0u;
    }
    uint remaining_width = width;
    uint remaining_height = height;
    uint levels = 1u;
    while (remaining_width > 1u || remaining_height > 1u)
    {
        remaining_width = (remaining_width + 1u) / 2u;
        remaining_height = (remaining_height + 1u) / 2u;
        ++levels;
    }
    return levels;
}

/// the WORD offset of level @p level inside one frame's lane (the value hzb_pyramid_words is built from)
uint hzb_level_offset(uint width, uint height, uint level)
{
    const uint levels = hzb_level_count(width, height);
    const uint last = level < levels ? level : levels;
    uint offset = 0u;
    for (uint each = 0u; each < last; ++each)
    {
        offset += hzb_level_width(width, each) * hzb_level_height(height, each);
    }
    return offset;
}

/// how many floats ONE frame lane holds: the same number the host writes the descriptor's length with
/// @note the product stays inside 32 bits for every extent this renderer creates (3840x2160 = ~11.06M
///       words), which is what lets the host and both shaders do this arithmetic in `uint`.
uint hzb_pyramid_words(uint width, uint height)
{
    return hzb_level_offset(width, height, hzb_level_count(width, height));
}

/// both dimensions of @p level, in one call (the dispatch strides and the tile loop both need the pair)
void hzb_level_size(uint width, uint height, uint level, out uint out_width, out uint out_height)
{
    out_width = hzb_level_width(width, level);
    out_height = hzb_level_height(height, level);
}

// =============================================================================================================
// THE PHASE-B CULL CONTRACT (M3 phase B1, spec §4.3/§6/§7.2): the two table layouts the cull stage reads and
// writes, and THE GPU MIRROR OF THE CPU REFERENCE'S CONSERVATIVE TEST.
//
// ONE CONTRACT, TWO SPELLINGS, AND THEY MUST AGREE LINE FOR LINE. `deren::engine::visibility::hzb` (hzb.cppm) is
// the CPU REFERENCE: the headless test pins it, and the in-engine probe (DEREN_HZB_CULL_PROBE_CAPTURE) compares
// every GPU decision against it on the real scene. Everything below is that reference's §6 formula in float, with
// the same fail-open early-outs and the same bias direction - a disagreement between the two is not noise, it is
// one of the two spellings being non-conservative. The shader keeps the reference's MATRIX STORAGE AND ACCESSOR
// SHAPES deliberately (a 16-float lane plus `element(row, column) = lane[column * 4 + row]`, and `transform` /
// `multiply` written out): reading a `row_major float4x4` with `mul` would be the same arithmetic, but the
// reference is the contract and this spelling can be compared with it line by line.
//
// WHERE FLOAT VS DOUBLE CAN DIFFER, and why the design accepts it: the reference evaluates in double, this file in
// float. The differences are confined to (a) the NDC-depth comparison against `hzb_ndc_epsilon`, (b) floor/ceil on
// the screen rectangle when a projected edge lands exactly on a pixel boundary, and (c) the level select at an
// exact power of two - i.e. only TIES, where the exact value sits within one float rounding of a decision
// boundary. The epsilon is only ever on the conservative side (a larger `farthest` keeps the candidate), and the
// probe's acceptance is 100% agreement: a mismatch is investigated, not tolerated.
// =============================================================================================================

// ---- capacities and thresholds: mirrors of hzb.cppm's exported constants ----
static const uint hzb_item_candidate_capacity = 4096u;  // how many candidates ONE item may offer (the id's meshlet field)
static const uint hzb_mesh_command_capacity = 4u * 65536u;
static const uint hzb_visibility_command_run = 2u * 65536u;
static const uint hzb_recovery_command_run = 3u * 65536u;
static const uint hzb_count_fallback = 0xFFFFFFFFu;
uint hzb_command_word_offset(uint slot, uint item)
{
    return (slot * hzb_mesh_command_capacity + hzb_visibility_command_run + item) * 3u;
}
static const uint hzb_candidate_capacity = 65536u;      // records one frame lane of a meshlet table holds
static const float hzb_ndc_epsilon = 1e-5;              // the NDC-depth comparison's epsilon (§6.6)
static const float hzb_near_plane_scale = 1.001;        // the cross-near-plane early-out's factor (§6.3)
static const float hzb_w_epsilon = 1e-4;                // clip w at or below which a position is "behind the eye"

/// the per-item candidate description the HOST writes into heap_slots_hzb_items (§4.3), one 16-byte entry per item
/// per frame lane. `candidate_count == 0` means "this item offers the cull nothing this frame" (it is not a
/// host-cut run, so its candidates are the shared import-time meshlet table the cull does not walk in phase B1).
struct HzbItem
{
    uint meshlet_base;    // the run's first candidate index inside the source table
    uint candidate_count; // how many candidates the run holds (0 = none)
    uint source;          // hzb_item_source_*: 0 = not a host-cut run, 1 = this frame's host-culled lane
    uint pad;             // original host dispatch count (probe contract), keeps the entry 16 B
};

/// `HzbItem.source`: the candidate records are NOT in the host-culled lane, so this phase culls nothing for the item
static const uint hzb_item_source_unculled = 0u;
/// `HzbItem.source`: the candidate records are in `heap_slots_hzb_culled` at `meshlet_base + i` for candidate i
static const uint hzb_item_source_culled = 1u;

// ---- THE heap_slots_hzb_results LAYOUT (one descriptor, one INTERNAL lane per frame slot) ----
//
// flags[65536] + pass1_count[4096] + pass2_count[4096] + stats[16] words, packed per frame lane. The flags are
// addressed by the ABSOLUTE candidate index (meshlet_base + i), one u32 per candidate - written exactly once, so
// no clear and no atomics; the per-item count has ONE writer (the item's own workgroup), and only the cross-item
// stats block is atomically accumulated. pass2_count is phase C's recovered suffix count (reserved in phase B so the shape
// and the byte offsets do not move when recovery lands).
static const uint hzb_results_flags = 0u;                           // [65536] bit0=history rejected, bit1=recovery kept; 0/1 in single-pass mode
static const uint hzb_results_pass1_count = hzb_candidate_capacity; // [4096] per item
static const uint hzb_results_pass2_count = hzb_results_pass1_count + hzb_item_candidate_capacity;
static const uint hzb_results_stats = hzb_results_pass2_count + hzb_item_candidate_capacity;
static const uint hzb_results_stats_count = 16u;
static const uint hzb_results_words = hzb_results_stats + hzb_results_stats_count;

/// the first WORD of @p frame_slot's results lane (the buffer is ONE descriptor holding `frame_slots` lanes)
uint hzb_results_lane_base(uint frame_slot)
{
    return frame_slot * hzb_results_words;
}

// A rejected result keeps the host command and the original host-culled records.
bool hzb_survivors_usable(uint slot, uint item)
{
    return heap_at<StructuredBuffer<uint>>(heap_slots_hzb_results)[hzb_results_lane_base(slot) + hzb_results_pass1_count + item] != hzb_count_fallback;
}

/// the first WORD of @p frame_slot's item lane (heap_slots_hzb_items holds 4096 16-byte entries per frame)
uint hzb_items_lane_base(uint frame_slot)
{
    return frame_slot * hzb_item_candidate_capacity;
}

/// the stats block's rows (§4.3/§10.3): the four numbers this phase accumulates per frame
static const uint hzb_stat_candidates = 0u;     // candidates the cull walked
static const uint hzb_stat_culled = 1u;         // candidates it conservatively dismissed
static const uint hzb_stat_survivors = 2u;      // candidates it kept
static const uint hzb_stat_retested = 4u;
static const uint hzb_stat_recovered = 5u;
static const uint hzb_stat_still_occluded = 6u;
static const uint hzb_stat_overflow_items = 3u; // items whose run could not be described: fallback sentinel preserves the original run

// ---- THE MATRIX SPELLING: the CPU reference's storage and its three accessors, copied §6.2 verbatim ----
//
// A matrix is the 16 floats the renderer already puts in its buffers: glm's COLUMN-MAJOR layout, element
// (row, column) = lane[column * 4 + row]. The header stores the camera UBO's own bytes, and the draw item stores
// the model matrix's, so loading a matrix is `asfloat(word)` in memory order - no transpose, no `mul`, no
// layout-majorness rule between the two sides (hzb.cppm's `detail::matrix_element` is the same one line).
struct HzbMatrix
{
    float lane[16];
};

/// the element at (@p row, @p column): THE CPU REFERENCE'S ACCESSOR, unchanged (hzb.cppm's detail::matrix_element)
float hzb_matrix_element(const HzbMatrix matrix, uint row, uint column)
{
    return matrix.lane[column * 4u + row];
}

/// `matrix * vector`: hzb.cppm's detail::transform, one column-vector product
float4 hzb_matrix_transform(const HzbMatrix matrix, const float4 vector)
{
    float4 result;
    for (uint row = 0u; row < 4u; ++row)
    {
        float sum = 0.0;
        for (uint column = 0u; column < 4u; ++column)
        {
            sum += hzb_matrix_element(matrix, row, column) * vector[column];
        }
        result[row] = sum;
    }
    return result;
}

/// `a * b` in the same column-vector sense (hzb.cppm's detail::multiply): `proj * view` is the view-projection
HzbMatrix hzb_matrix_multiply(const HzbMatrix a, const HzbMatrix b)
{
    HzbMatrix result;
    for (uint row = 0u; row < 4u; ++row)
    {
        for (uint column = 0u; column < 4u; ++column)
        {
            float sum = 0.0;
            for (uint inner = 0u; inner < 4u; ++inner)
            {
                sum += hzb_matrix_element(a, row, inner) * hzb_matrix_element(b, inner, column);
            }
            result.lane[column * 4u + row] = sum;
        }
    }
    return result;
}

/// @brief the largest ROW sum of the matrix's linear part (§6.2's operator-norm upper bound on the model's scale)
/// @note ROW SUMS, and the two words are load-bearing: the reference takes `sum_j |M(r, j)|` for r in 0..2, so a
///       model with non-uniform scale or shear gets a radius that is never too small. `matrix_max_axis_scale` in
///       mesh_geometry.slang is a DIFFERENT sum (columns of the host matrix, because it is fed the stored rows) -
///       reusing it here would disagree with the reference on any scaled scene.
float hzb_model_axis_scale(const HzbMatrix model)
{
    float largest = 0.0;
    for (uint row = 0u; row < 3u; ++row)
    {
        const float sum = abs(hzb_matrix_element(model, row, 0u)) + abs(hzb_matrix_element(model, row, 1u)) + abs(hzb_matrix_element(model, row, 2u));
        largest = max(largest, sum);
    }
    return largest;
}

/// @brief the NDC z of an eye-space depth (§6.4): the monotone function the near-depth bound is derived from
/// @note THE ALGEBRA IS REARRANGED FOR FLOAT PRECISION, and that is worth a measurement: the reference's
///       `(z22 * -d + z23) / d` in double is one contract, but evaluating it in float rounds a ~164-scaled
///       numerator before dividing and loses ~4e-8 NDC - which is the same order as the cull/keep margin of the
///       closest real case (measured: 5.4e-8 on Bistro, and that one candidate flipped). `-z22 - z23 / d` is the
///       same function evaluated where the result lives (one subtraction of a small quotient from ~1.0), so the
///       only rounding left is the final representation, which no arithmetic can remove in float.
float hzb_z_ndc_at_depth(const HzbMatrix projection, const float depth)
{
    // (z22 * -d + z23) / d, rearranged as -z22 + z23 / d - the same function, one rounding instead of three
    return -hzb_matrix_element(projection, 2u, 2u) + hzb_matrix_element(projection, 2u, 3u) / depth;
}

/// one matrix loaded from @p words at @p word_base (memory order, casts only - see HzbMatrix's note)
HzbMatrix hzb_load_matrix(RWStructuredBuffer<uint> words, uint word_base)
{
    HzbMatrix matrix;
    for (uint index = 0u; index < 16u; ++index)
    {
        matrix.lane[index] = asfloat(words[word_base + index]);
    }
    return matrix;
}

/// whether every component of @p matrix is finite (the reference's detail::all_finite, which fails open)
bool hzb_matrix_finite(const HzbMatrix matrix)
{
    for (uint index = 0u; index < 16u; ++index)
    {
        if (!isfinite(matrix.lane[index]))
        {
            return false;
        }
    }
    return true;
}

/// whether every component of @p vector is finite (the reference's detail::all_finite3)
bool hzb_vector_finite(const float3 vector)
{
    return isfinite(vector.x) && isfinite(vector.y) && isfinite(vector.z);
}

// ---- THE HISTORY CAMERA + THE DERIVED BOUNDS (§6.1/§6.3/§6.4), read from heap_slots_hzb_header ----
//
// The header is the ONE place the matrices that BUILT the pyramid exist after the frame that built it recorded
// (spec C21: the camera UBO lane is overwritten before the next frame records), so the cull reads the camera from
// here and never from the UBO. `usable` is the reference's input validation, hoisted: same header for every
// candidate of the item, same answer.
struct HzbHistory
{
    uint width;
    uint height;
    uint levels;
    HzbMatrix projection;      // the derivation's own projection (§6.4's z_ndc), kept beside the product
    HzbMatrix view_projection; // proj * view, the matrix every world position in the test goes through
    float3 camera_position;    // the eye, for the "eye inside the ball" early-out
    float near_plane;          // derived from proj (§6.3), used by the cross-near-plane early-out
    float far_plane;           // derived from proj (§6.4), used by the past-far-plane early-out
    bool usable;               // magic/extent/level count/finiteness: false -> the caller keeps every candidate
    bool projection_ok;        // the proj is the perspectiveRH_ZO shape §6.4 derives its bounds from
};

/// @brief read @p frame_slot's header lane and derive everything the test needs, once per item
/// @param expected_width/expected_height the extent the cull pass expects (push lanes, from the host) - a header
///        from another extent is a resize race and must not index this frame's pyramid stride
/// @note the mutability of `header` is only the heap's descriptor type (a `RWStructuredBuffer` read); nothing is
///       written here.
HzbHistory hzb_read_history(RWStructuredBuffer<uint> header, uint frame_slot, uint expected_width, uint expected_height)
{
    HzbHistory history;
    history.usable = false;
    history.projection_ok = false;
    const uint base = hzb_header_lane_base(frame_slot);
    history.width = header[base + hzb_header_word_width];
    history.height = header[base + hzb_header_word_height];
    history.levels = header[base + hzb_header_word_levels];
    history.usable = header[base + hzb_header_word_magic] == hzb_magic && history.width == expected_width && history.height == expected_height &&
                     history.levels == hzb_level_count(history.width, history.height) && history.levels != 0u && history.levels <= hzb_max_levels;
    if (!history.usable)
    {
        return history;
    }
    const HzbMatrix view = hzb_load_matrix(header, base + hzb_header_word_view);
    const HzbMatrix projection = hzb_load_matrix(header, base + hzb_header_word_proj);
    history.camera_position = float3(asfloat(header[base + hzb_header_word_camera_pos + 0u]), asfloat(header[base + hzb_header_word_camera_pos + 1u]),
                                     asfloat(header[base + hzb_header_word_camera_pos + 2u]));
    history.projection = projection;
    history.view_projection = hzb_matrix_multiply(projection, view);
    if (!hzb_matrix_finite(view) || !hzb_matrix_finite(projection) || !hzb_vector_finite(history.camera_position))
    {
        history.usable = false;
        return history;
    }
    // ---- §6.3/§6.4: the projection's SHAPE IS VALIDATED, not assumed (hzb.cppm's detail::projection_of) ----
    // A projection of any other shape (an orthographic one, an infinite far plane, a flipped z) gets no derived
    // bounds: a bound derived from the wrong formula is not a conservative bound, so the whole test fails open.
    history.projection_ok = false;
    if (hzb_matrix_element(projection, 3u, 0u) != 0.0 || hzb_matrix_element(projection, 3u, 1u) != 0.0 || hzb_matrix_element(projection, 3u, 2u) != -1.0 ||
        hzb_matrix_element(projection, 3u, 3u) != 0.0)
    {
        return history;
    }
    // XY projection offsets (including TAA jitter) are covered by the eight-corner footprint.
    // Only mixing XY into clip Z invalidates the monotone near-depth bound.
    if (hzb_matrix_element(projection, 2u, 0u) != 0.0 || hzb_matrix_element(projection, 2u, 1u) != 0.0)
    {
        return history;
    }
    const float z_row = hzb_matrix_element(projection, 2u, 2u);
    const float z_constant = hzb_matrix_element(projection, 2u, 3u);
    if (!(z_row < 0.0) || !(z_constant < 0.0))
    {
        return history;
    }
    history.near_plane = z_constant / z_row;
    const float denominator = 1.0 + z_row;
    if (denominator == 0.0)
    {
        return history; // an infinite far plane: §6.4's "entirely past the far plane" test has no answer
    }
    history.far_plane = z_constant / denominator;
    history.projection_ok = isfinite(history.near_plane) && isfinite(history.far_plane) && history.near_plane > 0.0 && history.far_plane > history.near_plane;
    return history;
}

// ---- §6.5: THE SCREEN RECTANGLE, from the world AABB's eight projected corners (NOT the prototype's circle) ----
struct HzbScreenRect
{
    uint x0;
    uint y0;
    uint x1;
    uint y1;
};

/// @brief the framebuffer rectangle a world ball covers (§6.5), or false when the test must fail open
/// @note THE CORNER PROJECTION IS THE CONSERVATIVE PART: the ball is inside the world AABB `center +/- radius`,
///       perspective maps a convex set in front of the eye to the convex hull of its projected corners, and the
///       bbox of those corners therefore CONTAINS the ball's footprint. A corner at/behind the eye breaks that,
///       so such a ball is preserved. This is the function that replaces the host prototype's circle of
///       `radius * focal / clip.w`, whose underestimation cost the prototype its 0.314% pixel difference.
bool hzb_screen_rect(const HzbMatrix view_projection, const float3 world_center, const float world_radius, const uint width, const uint height,
                     out HzbScreenRect rect)
{
    // the out parameter is initialized FIRST so every early return below leaves a defined value (Slang warns on an
    // out that a return path does not write, and a defined-but-unused value is the truth this function means).
    rect.x0 = 0u;
    rect.y0 = 0u;
    rect.x1 = 0u;
    rect.y1 = 0u;
    float min_x = 3.402823466e+38; // FLT_MAX: finite, and every finite NDC candidate below replaces it
    float min_y = 3.402823466e+38;
    float max_x = -3.402823466e+38;
    float max_y = -3.402823466e+38;
    for (uint corner = 0u; corner < 8u; ++corner)
    {
        const float3 position = world_center + float3((corner & 1u) != 0u ? world_radius : -world_radius,
                                                      (corner & 2u) != 0u ? world_radius : -world_radius,
                                                      (corner & 4u) != 0u ? world_radius : -world_radius);
        const float4 clip = hzb_matrix_transform(view_projection, float4(position, 1.0));
        if (clip.w <= hzb_w_epsilon)
        {
            return false; // a corner behind the eye: no finite rectangle bounds this projection
        }
        const float ndc_x = clip.x / clip.w;
        const float ndc_y = clip.y / clip.w;
        if (!isfinite(ndc_x) || !isfinite(ndc_y))
        {
            return false;
        }
        min_x = min(min_x, ndc_x);
        min_y = min(min_y, ndc_y);
        max_x = max(max_x, ndc_x);
        max_y = max(max_y, ndc_y);
    }
    // §6.5's pixel mapping, with Vulkan's y-down convention already inside `proj` (proj[1][1] < 0). floor/ceil
    // rather than round: the rasterizer covers a pixel when its CENTRE is inside the triangle, so flooring the
    // left edge and ceiling the right one covers every pixel the geometry could touch.
    const float left = floor((min_x * 0.5 + 0.5) * float(width));
    const float top = floor((min_y * 0.5 + 0.5) * float(height));
    const float right = ceil((max_x * 0.5 + 0.5) * float(width));
    const float bottom = ceil((max_y * 0.5 + 0.5) * float(height));
    if (!isfinite(left) || !isfinite(top) || !isfinite(right) || !isfinite(bottom))
    {
        return false;
    }
    // THE CLAMP IS ONE-SIDED PER EDGE (§6.5): the left edge only grows to 0 and the right one only shrinks to
    // width-1, so a rectangle entirely off an edge keeps x1 < x0 and is preserved instead of collapsing onto the
    // border column and sampling it.
    const float clamped_left = max(left, 0.0);
    const float clamped_top = max(top, 0.0);
    const float clamped_right = min(right, float(width) - 1.0);
    const float clamped_bottom = min(bottom, float(height) - 1.0);
    if (clamped_right < clamped_left || clamped_bottom < clamped_top)
    {
        return false; // entirely outside the OLD frustum: it could not have been in the old image
    }
    rect.x0 = uint(clamped_left);
    rect.y0 = uint(clamped_top);
    rect.x1 = uint(clamped_right);
    rect.y1 = uint(clamped_bottom);
    return true;
}

/// @brief THE CONSERVATIVE SPHERE-vs-HZB TEST (§6), the GPU mirror of hzb.cppm's hzb_test_sphere
/// @return true only when every depth sample under the ball's footprint is strictly nearer than the ball's nearest
///         possible point; false in EVERY other case, including every one where a bound is invalid
/// @note EVERY RETURN OF `false` BELOW IS A FAIL-OPEN, and the list is the reference's exhaustive set: unusable
///       header/projection; non-finite sphere/model; centre behind the eye (w <= 1e-4); eye inside the sphere; the
///       ball crossing or hugging the near plane (1.001 * near); the ball entirely past the far plane; a projected
///       AABB corner behind the eye; the footprint entirely outside the old frustum; the sampled tiles holding the
///       clear value (1.0, "no geometry"). The only way this function can be wrong in the picture is by keeping a
///       candidate that would have been hidden - never by hiding one. The reference's test_occlusion pins the same
///       direction case by case.
bool hzb_occluded(const HzbHistory history, RWStructuredBuffer<float> pyramid, const uint pyramid_lane_base, const HzbMatrix model,
                  const float3 sphere_center, const float sphere_radius)
{
    if (!history.usable || !history.projection_ok)
    {
        return false;
    }
    // ---- §6.2: the world-space ball. The radius uses the row-sum bound of the MODEL matrix only; the bounds that
    // need the view/projection's scale are derived analytically below instead of scaling a radius by them. ----
    if (!(sphere_radius >= 0.0) || !isfinite(sphere_radius) || !hzb_vector_finite(sphere_center) || !hzb_matrix_finite(model))
    {
        return false;
    }
    const float4 center = hzb_matrix_transform(model, float4(sphere_center, 1.0));
    const float3 world_center = float3(center.x, center.y, center.z);
    const float world_radius = sphere_radius * hzb_model_axis_scale(model);
    if (!isfinite(world_center.x) || !isfinite(world_center.y) || !isfinite(world_center.z) || !isfinite(world_radius))
    {
        return false;
    }
    // ---- §6.3: the cheapest early-outs. `w` is the centre's clip-space w, which for the validated projection is
    // the centre's distance down the view axis, so the ball's nearest point is `w - r_w`. ----
    const float4 center_clip = hzb_matrix_transform(history.view_projection, float4(world_center, 1.0));
    const float center_w = center_clip.w;
    if (!isfinite(center_w) || center_w <= hzb_w_epsilon)
    {
        return false; // centre behind the eye plane
    }
    const float3 eye_offset = world_center - history.camera_position;
    const float eye_distance = length(eye_offset);
    if (eye_distance <= world_radius)
    {
        return false; // the eye is inside the ball: no projection bound means anything
    }
    const float nearest_depth = center_w - world_radius; // = d_near
    if (nearest_depth <= history.near_plane * hzb_near_plane_scale)
    {
        return false; // crosses (or hugs) the near plane: §6.4's bound is not defined here
    }
    if (nearest_depth >= history.far_plane)
    {
        return false; // entirely past the far plane: it was not in the old image
    }
    // ---- §6.4: the near-depth bound, the monotone formula (NOT the prototype's ndc_z - radius/w) ----
    // The bound's VALUE is `hzb_z_ndc_at_depth(projection, nearest_depth)`; the comparison below is evaluated in
    // the algebraically equal but float-accurate form. See the margin's note there - the rearrangement is what
    // turned the measured float/double disagreement from ~1e-7 into ~1e-10, i.e. below every real decision gap.
    // ---- §6.5: the screen rectangle, from the world AABB's projected corners (NOT the prototype's circle) ----
    HzbScreenRect rect;
    if (!hzb_screen_rect(history.view_projection, world_center, world_radius, history.width, history.height, rect))
    {
        return false;
    }
    // ---- §6.6: pick a level where the rectangle covers at most ~4x4 tiles, then take the max of exactly the
    // tiles covering [x0..x1] - a CLOSED range, because a `ceil` pyramid puts pixel x in tile x >> L. ----
    const uint horizontal = rect.x1 - rect.x0;
    const uint vertical = rect.y1 - rect.y0;
    const uint widest = max(horizontal, vertical);
    const uint span = max(widest, 1u);
    uint level = 0u;
    if (span > 1u)
    {
        const uint floor_log2 = uint(firstbithigh(span)); // std::bit_width(span) - 1 in the reference
        level = floor_log2 > 0u ? floor_log2 - 1u : 0u;
    }
    level = min(level, history.levels - 1u);
    uint level_width = 0u;
    uint level_height = 0u;
    hzb_level_size(history.width, history.height, level, level_width, level_height);
    const uint tile_x0 = rect.x0 >> level;
    const uint tile_y0 = rect.y0 >> level;
    const uint tile_x1 = min(rect.x1 >> level, level_width - 1u);
    const uint tile_y1 = min(rect.y1 >> level, level_height - 1u);
    const uint level_base = pyramid_lane_base + hzb_level_offset(history.width, history.height, level);
    float farthest = 0.0;
    for (uint tile_y = tile_y0; tile_y <= tile_y1; ++tile_y)
    {
        for (uint tile_x = tile_x0; tile_x <= tile_x1; ++tile_x)
        {
            farthest = max(farthest, pyramid[level_base + tile_y * level_width + tile_x]);
        }
    }
    // The clear value is 1.0 (the far plane), so a tile that still holds it means "no geometry here": preserving
    // that ball is what makes an empty sky the exact boundary case it should be, with no sentinel in the pyramid.
    if (!(farthest < 1.0))
    {
        return false;
    }
    // ---- THE DECISION, AS ONE FUSED MARGIN: `near_ndc - farthest - epsilon > 0` ----
    //
    // THIS GROUPING IS THE FLOAT/DOUBLE CONTRACT'S ENTIRE MECHANISM, and it was measured rather than guessed. The
    // reference's formula in double is exact; in float the natural spellings lose 1-2 NDC ulps (~6e-8), which is
    // the same order as the closest real decision gap on Bistro (measured: 5.4e-8 and -9.8e-9 on two candidates,
    // one decision flipped each way). The rearrangement below loses ~1e-10 instead:
    //   * `-z_row - farthest` subtracts two near-equal ~0.9995 floats, which Sterbenz's lemma makes EXACT (and the
    //     values are only within a factor of two of each other exactly when they are close, i.e. only when the
    //     cancellation matters);
    //   * `- epsilon` is then a small correction to a ~1e-5 quantity, not an addition to a ~1.0 one;
    //   * `z_constant / nearest_depth` is one division of two floats into a ~6e-4 quotient (one rounding, ~1e-11).
    // Every operand is the reference's own value; no bias was added, so the direction of every decision is the
    // reference's - only its float rounding is bounded far below a real gap. A NaN (an invalid projection that got
    // this far) makes the margin NaN and the comparison false, i.e. it keeps the candidate: the fail-open direction.
    const float margin = (-hzb_matrix_element(history.projection, 2u, 2u) - farthest - hzb_ndc_epsilon) +
                         hzb_matrix_element(history.projection, 2u, 3u) / nearest_depth;
    return margin > 0.0;
}

#endif // HZB_SLANG
