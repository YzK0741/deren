// shaders/scene/software_merge.slang - THE SOFTWARE RASTER'S DEPTH-ONLY MERGE (M4 Task 2 scaffolding).
//
// PRODUCES `software_merge.vert.spv` + `software_merge.frag.spv`. The hybrid visibility design's LAST step before the
// HZB and the lighting (docs/superpowers/specs/2026-10-22-software-raster-design.md §3.2/§5.2): one fullscreen
// depth-only draw over the frame's own G-buffer depth, with the pipeline's LESS_OR_EQUAL compare and depth write
// enabled. The merge's effect is then `min(hardware, software)` for free - including the design's tie rule (an equal
// value writes the same value, so a tie changes nothing) - and the depth image every later stage samples stays the
// ONE depth the frame has, rather than a second image someone must resolve later.
//
// THE ORDER THE DESIGN REQUIRES is the graph's, not this file's: the resolve runs BEFORE this pass (it reads the
// UNMERGED hardware depth and the software candidates), and this merge runs after it. `render_graph/visibility.cppm`'s
// `append_software_merge` declares both edges.
//
// WHAT THIS FILE IS TODAY: the fullscreen triangle and a fragment stage that writes 1.0 into SV_Depth, i.e. the depth
// test can never pass (a scene depth is < 1 except where nothing was drawn, and there the hardware value is already
// the far plane). That makes a recorded merge a NO-OP rather than a wrong picture until Task 3 fills the body: it
// samples the two software images, reconstructs the software depth from the R32_UINT bit pattern
// (`asfloat(sw_depth_bits)`, the same spelling as the CPU reference's `software_pixel_usable`), and writes it - a
// value <= the stored depth passes the test and becomes the new depth, which is exactly min(hw, sw).

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
// LESS_OR_EQUAL compare against the existing depth turns a returned value into min(hw, sw) with no arithmetic here;
// 1.0 is the far plane, so the scaffolding merge is a no-op. Task 3 samples `software_tiles`' owner's two heap images
// (the depth bit pattern and the id) and returns the software depth, with the id's EMPTY value (0) returning 1.0 so an
// empty software pixel never competes with the hardware one (spec §3.2's winner rule).
[shader("fragment")]
float frag_main() : SV_Depth
{
    return 1.0;
}
