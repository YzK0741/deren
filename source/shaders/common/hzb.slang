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

#endif // HZB_SLANG
