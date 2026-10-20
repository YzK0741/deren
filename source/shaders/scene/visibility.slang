// shaders/scene/visibility.slang - the VISIBILITY BUFFER's two stages (visibility-buffer v0.3).
//
// PRODUCES TWO SPIR-V MODULES from one file, because they share the push block and the geometry fetch:
//   * `visibility.mesh.spv`  - one workgroup per meshlet, the SAME conservative per-meshlet frustum cull the
//                              pbr meshlet entry does, emitting each triangle's three vertices plus ONE
//                              FLAT PACKED ID per triangle (`nointerpolation`);
//   * `visibility.frag.spv`  - writes that id into the R32_UINT visibility target. No colour, no material,
//                              no attributes: the depth test is the whole visibility decision, and the
//                              resolve pass (a later step) reconstructs everything else from the id.
//
// WHY THIS SHELL IS SEPARATE FROM pbr.slang: the id does not need the material, the skin, the morph or the
// vertex attributes the G-buffer pass interpolates, and adding a second output to that file's vertex record
// would make every existing fragment stage pay for it. This stage reads the same 64-byte interleaved vertex -
// through the SAME `mesh_vertex_at`/`mesh_index_at` the pbr meshlet entry uses - so a triangle lands on the
// same pixels with the same depth by construction rather than by a copied expression.
//
// THE PACKING IS `deren.engine.visibility`'s C++ layout, mirrored here by hand (a shader cannot include a C++
// enum; the heap slot grid and the material lanes already follow that rule):
//   bit 31    : valid (always 1 for a written id; the target's clear value is 0 = "no geometry")
//   bits 30:19: item            (which per-frame draw item; 12 bits)
//   bits 18:7 : meshlet_in_item (the item's meshlet order; 12 bits)
//   bits  6:0 : triangle        (meshlet-local, 7 bits; the 85-triangle workgroup limit fits)
//
// v0 SCOPE: static, non-instanced leaves only. `push.model` is the draw's model matrix (no instance table,
// no skin, no morph), which is exactly the subset the runtime will admit to this path; the id's `item` is
// the runtime's index into its per-frame draw-item table, which is what the resolve pass reads back.
//
// ---- v0.4b CORRECTION: THE TRIANGLES COME FROM THE MESHLET RECORD, NOT FROM THE GEOMETRY WINDOW ----
// The first version of this entry (v0.3a) was `pbr.slang`'s *mesh* entry with a per-meshlet cull bolted on:
// it read the triangle count from `geometry.index_count` and the indices from `geometry.first_index + t*3`.
// The runtime's item recorder (v0.3b-3a) fills those two lanes with the MESHLET RUN's base and count - the
// values the per-meshlet dispatch needs - not with the draw's index window, so for every meshlet past the
// first few the stage indexed the index buffer at the wrong place (and could read beyond it). Nothing caught
// it because until v0.4 there was no consumer of the ids: the capture lane wrote its own two targets and the
// frame's picture came from the old geometry path. The emission below is now `pbr.slang`'s MESHLET entry's
// emission - the record's own `first_index`/`index_count`/`base_vertex` window - so a triangle's id, depth and
// the indices the resolve reads back are three views of ONE record.
//
// THE PUSH BLOCK IS THIS STAGE'S OWN SHAPE, not the material block: the visibility raster pass composes it
// from the draw item it is recording (model + item index + frame slot + the geometry lanes), so no field has
// to be kept in step with the material push constants or the shadow stage's extra lanes. In a meshlet run
// `geometry.first_index` is the item's MESHLET RUN BASE and `geometry.index_count` its MESHLET COUNT (the
// dispatch's group count) - not an index window; `geometry.base_vertex` is unused and zero.

#include "common/heap_access.slang"
// the material record: the raster fragment reads one record per pixel for the alphaMode MASK test (item 2)
#include "common/material_record.slang"
#include "geometry/mesh_geometry.slang"

struct VisibilityPush
{
    // the model matrix FIRST, so the two scalar lanes and the geometry lanes need no padding: 64 + 4 + 4 + 32
    // = 104 bytes, and the host pushes exactly that many (every declared byte, the heap-native rule).
    VR_MAT4 model;            // the draw item's model matrix (static draws only in v0)
    uint item_index;          // the per-frame draw item this dispatch belongs to (12 bits in the id)
    uint frame_slot;          // which per-frame lane the camera reads through
    uint material_index;      // the material record (alpha test source); the explicit pad keeps geometry aligned
    uint pad;
    MeshGeometryLanes geometry; // where this item's vertices/indexes are, and which window the dispatch covers
};
[[vk::push_constant]] ConstantBuffer<VisibilityPush> push;
#define heap_frame_slot (push.frame_slot)

// The only three members of the camera block this stage needs, in the host record's order (`camera_ubo`:
// view, proj, ...; proj is the JITTERED projection the G-buffer pass also rasterizes with, so depth agrees).
struct VisibilityCamera
{
    VR_MAT4 view;
    VR_MAT4 proj;
};
#define visibility_camera heap_at<ConstantBuffer<VisibilityCamera>>(heap_camera_slot)

// The id's field shifts, the C++ layout's constants spelled out (see deren.engine.visibility's static_asserts).
static const uint visibility_id_meshlet_shift = 7u;
static const uint visibility_id_item_shift = visibility_id_meshlet_shift + 12u;
static const uint visibility_id_valid = 1u << 31u;

// One triangle's three vertices, plus the flat id every one of them carries: the fragment stage receives it
// UNINTERPOLATED, so all three corners of a triangle name the same triangle.
struct VisibilityVertexOutput
{
    float4 position : SV_Position;
    nointerpolation uint vis_id : TEXCOORD0;
    float2 uv : TEXCOORD1; // the alpha test's UV (item 2)
    nointerpolation uint material_index : TEXCOORD2; // the alpha test's material (item 2) // INTERPOLATED FOR THE ALPHA TEST, item 2: the raster fragment needs the material UV to
                           // decide an alphaMode MASK cut-out BEFORE the depth write (see the plan review).
                           // Carried now that the decision is priced: the mesh raster measures 0.01 ms.
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void mesh_main(out vertices VisibilityVertexOutput verts[mesh_vertices_per_workgroup], out indices uint3 tris[mesh_triangles_per_workgroup], uint3 group_id : SV_GroupID)
{
    const MeshGeometryLanes geometry = push.geometry;
    // ONE WORKGROUP PER MESHLET: the lane's first_index is the item's MESHLET RUN BASE and the group id is the
    // meshlet's order inside that run, so this is the same record the pbr meshlet entry reads (and the resolve
    // reads back through the id).
    const bool host_culled = (geometry.base_vertex & 1u) != 0u;
    const MeshletRecord meshlet = host_culled ? culled_meshlet_at(push.frame_slot, geometry.first_index + group_id.x)
                                            : meshlet_at(geometry.first_index + group_id.x);

    // THE SAME CONSERVATIVE CULL pbr's meshlet entry does: the record's object-space sphere pushed through the
    // draw's model and the camera's view*proj, the radius scaled by an upper bound on both matrices' axis scales.
    // A culled meshlet emits NOTHING, which leaves those pixels to a nearer triangle - and the test is
    // deliberately conservative, because a visible meshlet culled here is a hole.
    // a descriptor HANDLE converts to its block only through a member access (the pattern every other stage
    // uses as `camera_at(...).view`), so the two matrices are read member-wise rather than copied as a struct.
    const VR_MAT4 view_proj = mul(visibility_camera.view, visibility_camera.proj);
    const float4 clip_center = mul(mul(float4(meshlet.center, 1.0), push.model), view_proj);
    const float radius = meshlet.radius * matrix_max_axis_scale(push.model) * matrix_max_axis_scale(view_proj);
    if (!clip_sphere_visible(clip_center, radius))
    {
        SetMeshOutputCounts(0u, 0u);
        return;
    }

    // THE MESHLET'S OWN WINDOW, and no more than one mesh workgroup can emit: `index_count` is the record's
    // (a multiple of three), capped at the 85-triangle / 255-vertex workgroup limit - the same `min` the pbr
    // meshlet entry applies, so a meshlet that fits a workgroup emits exactly the triangles the old path
    // rasterized for it.
    const uint triangle_count = min(meshlet.index_count, mesh_triangles_per_workgroup * mesh_indices_per_triangle) / mesh_indices_per_triangle;
    SetMeshOutputCounts(triangle_count * mesh_indices_per_triangle, triangle_count);

    // THE ID IS THE MESHLET'S ORDER INSIDE THE ITEM, not its table index: the resolve pass reads the item's
    // record to find the run's base, so the two halves agree on the order the dispatch was built with.
    const uint meshlet_in_item = group_id.x;
    for (uint t = 0u; t < triangle_count; ++t)
    {
        const uint vis_id = visibility_id_valid | (push.item_index << visibility_id_item_shift) |
                            (meshlet_in_item << visibility_id_meshlet_shift) | t;
        for (uint corner = 0u; corner < mesh_indices_per_triangle; ++corner)
        {
            // THE RECORD'S WINDOW AND THE RECORD'S base_vertex, exactly as the pbr meshlet entry fetches them:
            // `geometry.base_vertex` is the run's flag lane in a meshlet session, never a vertex offset.
            const uint raw_index = mesh_index_at(geometry, meshlet.first_index + t * mesh_indices_per_triangle + corner);
            const uint vertex_index = uint(int(raw_index) + meshlet.base_vertex);
            const MeshVertex v = mesh_vertex_at(geometry, vertex_index);
            VisibilityVertexOutput o;
            // THE CLIP POSITION MUST BE SPELLED LIKE pbr.slang SPELLS IT, and that is a MEASURED requirement, not
            // style: with a precomputed `view_proj` local (`mul(mul(pos, model), view_proj)`) this stage's depth came
            // out 1-2 ULP NEARER than the toon re-shading pipeline's on 184,782 pixels (17.8% of the frame, every one
            // nearer, u32-bit delta mostly 1), so the stage's EXACT depth-EQUAL test rejected every character
            // fragment and the whole-frame fallback was invented for it. With this nested form both stages' depth
            // dumps are BIT-IDENTICAL (0 differing pixels) - see docs/reviews/2026-10-18-visibility-buffer-ab.md.
            // The difference is codegen: the precomputed-product form lets the compiler reassociate the matrix
            // chain `(world * view) * proj` differently than the nested form's evaluation order.
            o.position = mul(mul(float4(v.position, 1.0), push.model), mul(visibility_camera.view, visibility_camera.proj));
            o.vis_id = vis_id;
            o.uv = v.uv;
            o.material_index = push.material_index;
            verts[t * mesh_indices_per_triangle + corner] = o;
        }
        tris[t] = uint3(t * mesh_indices_per_triangle + 0u, t * mesh_indices_per_triangle + 1u, t * mesh_indices_per_triangle + 2u);
    }
}

[shader("fragment")]
void main([[vk::location(0)]] nointerpolation uint vis_id : TEXCOORD0, [[vk::location(1)]] float2 uv : TEXCOORD1,
          [[vk::location(2)]] nointerpolation uint material_index : TEXCOORD2, out uint target : SV_Target0)
{
    // ---- THE alphaMode MASK TEST, IN THE RASTER FRAGMENT (visibility-buffer item 2) ----
    // It has to run HERE and BEFORE the depth write: a `discard` kills the fragment, the rasterizer keeps testing
    // the triangles behind it, and the surface that is actually visible wins the id AND the depth. The compute
    // resolve cannot reproduce this - it cannot reach the surface behind a masked pixel (measured: 957,719 pixels
    // with no surface behind on sponza before this test existed), which is why MASK frames used to fall back whole.
    // The cost is bounded by measurement: the mesh-task raster is 0.01 ms against the resolve's 0.92 ms (the
    // review record's lane split), and only a MASK material fetches the albedo here.
    const Material material = material_at(heap_slots_materials, material_index);
    if ((material.flags & 16u) != 0u && heap_sample(material.tex_indices.x, uv).a < material.alpha_cutoff)
        discard;
    // the store remains the whole of the surviving fragment: depth was decided by the rasterizer, and the resolve
    // turns this word back into the triangle it names.
    target = vis_id;
}
