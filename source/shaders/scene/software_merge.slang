// shaders/scene/software_merge.slang - THE SOFTWARE RASTER'S DEPTH-ONLY MERGE.
//
// PRODUCES `software_merge.vert.spv` + `software_merge.frag.spv`. The hybrid visibility design's LAST step before the
// HZB and the lighting (historical design record §3.2/§5.2): one fullscreen
// depth-only draw over the frame's own G-buffer depth, with the pipeline's LESS_OR_EQUAL compare and depth write
// enabled. The merge's effect is then `min(hardware, software)` for free - including the design's tie rule (an equal
// value writes the same value, so a tie changes nothing) - and the depth image every later stage samples stays the
// ONE depth the frame has, rather than a second image someone must resolve later.
//
// THE ORDER THE DESIGN REQUIRES is the graph's, not this file's: the resolve runs BEFORE this pass (it reads the
// UNMERGED hardware depth and the software candidates), and this merge runs after it. `render_graph/visibility.cppm`'s
// `append_software_merge` declares both edges.
//
// The fragment stage samples the software images, reconstructs depth from the R32_UINT bit pattern, and lets the
// pipeline's LESS_OR_EQUAL depth test write min(hardware, software) into the frame depth attachment.

#include "common/heap_access.slang"
#include "common/software_candidate.slang"

struct SoftwareMergePush
{
    uint frame_slot;
    uint image_index;
    uint spare_lane;
};
[[vk::push_constant]] ConstantBuffer<SoftwareMergePush> push;
#define heap_frame_slot (push.frame_slot)
#define heap_image_index (push.image_index)

struct software_merge_vertex_output {
    float4 position : SV_Position;
};

// THE FULLSCREEN TRIANGLE, synthesized from the vertex index alone (the same spelling as `post.slang` and
// `debug/heap_probe.slang`): three vertices, one triangle covering the whole render extent, no vertex buffer.
[shader("vertex")]
software_merge_vertex_output vert_main(uint vertex_index : SV_VertexID)
{
    const float2 positions[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    software_merge_vertex_output out;
    out.position = float4(positions[vertex_index], 0.0, 1.0);
    return out;
}

// THE DEPTH-ONLY FRAGMENT STAGE: with no colour attachment this stage's ONLY output is SV_Depth. The pipeline's
// LESS_OR_EQUAL compare against the existing depth turns a returned value into min(hw, sw); an empty software pixel
// returns 1.0, so it never competes with the hardware candidate (spec §3.2's winner rule).
[shader("fragment")]
float frag_main(float4 position : SV_Position) : SV_Depth
{
    const uint2 pixel = uint2(position.xy);
    const uint id = heap_at<Texture2D<uint>>(heap_image_slot(heap_slots_software_id)).Load(int3(pixel, 0));
    const uint depth_bits = heap_at<Texture2D<uint>>(heap_image_slot(heap_slots_software_depth)).Load(int3(pixel, 0));
    return software_candidate_usable(id, depth_bits) ? asfloat(depth_bits) : 1.0;
}
