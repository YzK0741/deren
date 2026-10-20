// THE HZB BUILD STAGE (M3 phase A2): builds the max pyramid of this frame's G-buffer depth, and writes the
// header that records WHICH camera/extent built it.
//
// SPEC: docs/superpowers/specs/2026-10-20-hzb-occlusion-design.md §4.1 (the representation), §4.2 (this pass),
// §7.1 (the pseudo-code this file implements) and §10.2 (the readback that proves it). The design's phase A is
// BUILD + VALIDATE ONLY: nothing culls against this pyramid yet, so the stage cannot change a pixel - it writes
// `heap_slots_hzb_levels` and `heap_slots_hzb_header` and nothing else reads them this frame.
//
// ONE DISPATCH PER LEVEL, and that is a correctness shape rather than a cost: level L's 2x2 max reads level L-1
// WHOLE, and a single dispatch would have to guess which of its neighbours another workgroup has already
// written. The host records one dispatch per level and one buffer barrier `{shader_write -> shader_read}`
// between adjacent dispatches (source/engine/pass/hzb_build.cpp), so each level reads a COMPLETE previous level.
//
// THE LAYOUT IS THE HOST'S: `common/hzb.slang` is the shader's copy of `deren.engine.visibility.hzb`'s
// recurrence, and the two are ONE formula (§4.1) - the host sizes the buffer with it, this stage writes each
// level at that offset, and the phase-B cull stage will read level L's tile (tx, ty) at the same offset. Do not
// "simplify" either side: a drift is not caught by validation, it shows up as a pyramid built into the wrong
// words.
//
// THE DEPTH READ IS AN EXPLICIT TEXEL FETCH, NOT A SAMPLED LOOKUP (`Texture2D.Load`, §4.2): a compute stage must
// not imply derivatives, and this stage wants the depth EXACTLY (the readback probe compares its level 0 with
// the raw `DEREN_DEPTH_PROBE_CAPTURE` dump word for word, §10.2). The sampled descriptor it fetches through is
// the frame's existing per-image one (`heap_slots_gbuffer_depth + image_index`), so no new heap slot is needed.
//
// THE HEADER IS WRITTEN BY THE SAME DISPATCH (§4.1, spec C21): level 0's thread 0 snapshots the frame camera's
// view/proj/camera_pos into it, which is the only place those matrices exist AFTER this frame's camera UBO lane
// is overwritten by the next frame's recording. The pyramid and the matrices that built it therefore cannot
// disagree - and the probe's metadata check ("header view/proj == this frame's camera") is that invariant's
// machine-checkable form.
#include "common/heap_access.slang"
#include "common/hzb.slang"

struct HzbBuildPush
{
    uint level; // the pyramid level THIS dispatch writes (the host records one dispatch per level)
    uint frame_slot; // appended by the pass framework: which lane of the HZB tables this frame owns
    uint image_index; // appended by the pass framework: which per-image depth descriptor to fetch
};
[[vk::push_constant]] ConstantBuffer<HzbBuildPush> push;
#define heap_frame_slot (push.frame_slot)
#define heap_image_index (push.image_index)

// the shader's [numthreads(8, 8, 1)]; the host's per-level dispatch divides the level's size by it
static const uint hzb_build_group_size = 8u;

/// @brief snapshot the frame camera into this frame slot's header lane
/// @param slot the HZB frame lane (the header table is ONE descriptor with an internal per-frame lane)
/// @note the camera block is read as a `StructuredBuffer<float4>` of its OWN bytes rather than through
///       `camera_at(...)`: a header copy must be BIT-EXACT (the probe compares it byte for byte with the host's
///       snapshot), and element-wise float4 copies through the same storage-class descriptor cannot round.
///       `camera_ubo`'s layout is view[16], proj[16], camera_pos(3)+pad - exactly nine float4 lanes
///       (source/engine/primitive/primitive.cppm's static_asserts pin it).
void hzb_write_header(uint slot, uint width, uint height, uint levels)
{
    StructuredBuffer<float4> camera = heap_at<StructuredBuffer<float4>>(heap_camera_slot);
    RWStructuredBuffer<uint> header = heap_at<RWStructuredBuffer<uint>>(heap_slots_hzb_header);
    const uint base = hzb_header_lane_base(slot);

    header[base + hzb_header_word_magic] = hzb_magic;
    header[base + hzb_header_word_width] = width;
    header[base + hzb_header_word_height] = height;
    header[base + hzb_header_word_levels] = levels;

    for (uint row = 0u; row < 4u; ++row)
    {
        const float4 view_row = camera[row];
        header[base + hzb_header_word_view + row * 4u + 0u] = asuint(view_row.x);
        header[base + hzb_header_word_view + row * 4u + 1u] = asuint(view_row.y);
        header[base + hzb_header_word_view + row * 4u + 2u] = asuint(view_row.z);
        header[base + hzb_header_word_view + row * 4u + 3u] = asuint(view_row.w);
        const float4 proj_row = camera[4u + row];
        header[base + hzb_header_word_proj + row * 4u + 0u] = asuint(proj_row.x);
        header[base + hzb_header_word_proj + row * 4u + 1u] = asuint(proj_row.y);
        header[base + hzb_header_word_proj + row * 4u + 2u] = asuint(proj_row.z);
        header[base + hzb_header_word_proj + row * 4u + 3u] = asuint(proj_row.w);
    }

    const float4 position = camera[8u];
    header[base + hzb_header_word_camera_pos + 0u] = asuint(position.x);
    header[base + hzb_header_word_camera_pos + 1u] = asuint(position.y);
    header[base + hzb_header_word_camera_pos + 2u] = asuint(position.z);
    header[base + hzb_header_word_camera_pos + 3u] = 0u; // the vec4 pad lane, written so the lane is deterministic

    // THE RESERVED TAIL IS ZEROED rather than left as allocation garbage: the readback probe dumps the whole
    // lane, and a dump that differs between two runs for a reason no consumer reads would make the A/B useless.
    for (uint word = hzb_header_word_reserved_begin; word < hzb_header_words; ++word)
    {
        header[base + word] = 0u;
    }
}

/// @brief one 2x2 max over level @p previous, with the odd boundary REPLICATED
/// @note REPLICATION IS THE CONSERVATIVE CHOICE (§4.2): the last column/row of an odd-width level has no
///       right/bottom neighbour, and pulling the edge value in can only raise a max that is already taken over a
///       real texel - a larger max makes the occlusion comparison keep MORE, which is the only direction this
///       design allows a bound to err in.
float hzb_reduce_2x2(RWStructuredBuffer<float> pyramid, uint previous_base, uint previous_width, uint previous_height, uint x, uint y)
{
    const uint x0 = min(2u * x, previous_width - 1u);
    const uint x1 = min(2u * x + 1u, previous_width - 1u);
    const uint y0 = min(2u * y, previous_height - 1u);
    const uint y1 = min(2u * y + 1u, previous_height - 1u);
    const float top_left = pyramid[previous_base + y0 * previous_width + x0];
    const float top_right = pyramid[previous_base + y0 * previous_width + x1];
    const float bottom_left = pyramid[previous_base + y1 * previous_width + x0];
    const float bottom_right = pyramid[previous_base + y1 * previous_width + x1];
    return max(max(top_left, top_right), max(bottom_left, bottom_right));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    const uint slot = push.frame_slot;
    // the depth image's own extent IS this frame's render extent (the G-buffer depth is the frame's), so the
    // dimensions come from the texture rather than from a push lane that a resize could leave stale (§4.2)
    Texture2D<float> depth = heap_at<Texture2D<float>>(heap_slots_gbuffer_depth + heap_image_index);
    uint width = 0u;
    uint height = 0u;
    uint depth_levels = 0u;
    depth.GetDimensions(0u, width, height, depth_levels);

    const uint levels = hzb_level_count(width, height);
    if (levels == 0u || push.level >= levels)
    {
        return; // a degenerate extent, or a dispatch the host should not have recorded
    }
    uint level_width = 0u;
    uint level_height = 0u;
    hzb_level_size(width, height, push.level, level_width, level_height);
    if (dispatch_thread_id.x >= level_width || dispatch_thread_id.y >= level_height)
    {
        return; // the last row/column of an odd level has no work here (the host rounds the group count up)
    }

    RWStructuredBuffer<float> pyramid = heap_at<RWStructuredBuffer<float>>(heap_slots_hzb_levels);
    // THE LANE START: `frame_slot * pyramid_words(extent)`, the internal per-frame lane of ONE descriptor -
    // never `+ heap_frame_slot` (see common/hzb.slang's header note)
    const uint lane_base = slot * hzb_pyramid_words(width, height);
    const uint level_base = lane_base + hzb_level_offset(width, height, push.level);

    float value = 0.0;
    if (push.level == 0u)
    {
        // level 0 IS the depth image (§4.1): no max, no filtering - an explicit texel fetch at the pixel centre
        value = depth.Load(int3(int(dispatch_thread_id.x), int(dispatch_thread_id.y), 0));
    }
    else
    {
        uint previous_width = 0u;
        uint previous_height = 0u;
        hzb_level_size(width, height, push.level - 1u, previous_width, previous_height);
        const uint previous_base = lane_base + hzb_level_offset(width, height, push.level - 1u);
        value = hzb_reduce_2x2(pyramid, previous_base, previous_width, previous_height, dispatch_thread_id.x, dispatch_thread_id.y);
    }
    pyramid[level_base + dispatch_thread_id.y * level_width + dispatch_thread_id.x] = value;

    // THE HEADER, FROM THREAD 0 OF LEVEL 0 only (§4.1): the same dispatch that writes the pyramid's first level
    // writes the matrices/extent that describe it, so "the HZB"' and "the camera that built it" are one write.
    if (push.level == 0u && dispatch_thread_id.x == 0u && dispatch_thread_id.y == 0u)
    {
        hzb_write_header(slot, width, height, levels);
    }
}
