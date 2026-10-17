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
//   bits 30:21: item            (which per-frame draw item; 10 bits)
//   bits 20:7 : meshlet_in_item (the item's meshlet order; 14 bits)
//   bits  6:0 : triangle        (meshlet-local, 7 bits; the 85-triangle workgroup limit fits)
//
// v0 SCOPE: static, non-instanced leaves only. `push.model` is the draw's model matrix (no instance table,
// no skin, no morph), which is exactly the subset the runtime will admit to this path; the id's `item` is
// the runtime's index into its per-frame draw-item table, which is what the resolve pass reads back.
//
// THE PUSH BLOCK IS THIS STAGE'S OWN SHAPE, not the material block: the visibility raster pass composes it
// from the draw item it is recording (model + item index + frame slot + the geometry lanes), so no field has
// to be kept in step with the material push constants or the shadow stage's extra lanes.

#include "common/heap_access.slang"
#include "geometry/mesh_geometry.slang"

struct VisibilityPush
{
    // the model matrix FIRST, so the two scalar lanes and the geometry lanes need no padding: 64 + 4 + 4 + 32
    // = 104 bytes, and the host pushes exactly that many (every declared byte, the heap-native rule).
    VR_MAT4 model;            // the draw item's model matrix (static draws only in v0)
    uint item_index;          // the per-frame draw item this dispatch belongs to (10 bits in the id)
    uint frame_slot;          // which per-frame lane the camera reads through
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
static const uint visibility_id_item_shift = visibility_id_meshlet_shift + 14u;
static const uint visibility_id_valid = 1u << 31u;

// One triangle's three vertices, plus the flat id every one of them carries: the fragment stage receives it
// UNINTERPOLATED, so all three corners of a triangle name the same triangle.
struct VisibilityVertexOutput
{
    float4 position : SV_Position;
    nointerpolation uint vis_id : TEXCOORD0;
};

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void mesh_main(out vertices VisibilityVertexOutput verts[mesh_vertices_per_workgroup], out indices uint3 tris[mesh_triangles_per_workgroup], uint3 group_id : SV_GroupID)
{
    const MeshGeometryLanes geometry = push.geometry;
    const uint triangle_count = geometry.index_count / mesh_indices_per_triangle;
    const uint first_triangle = group_id.x * mesh_triangles_per_workgroup;
    const uint remaining = first_triangle < triangle_count ? triangle_count - first_triangle : 0u;
    const uint emit = min(remaining, mesh_triangles_per_workgroup);
    SetMeshOutputCounts(emit * mesh_indices_per_triangle, emit);

    // THE SAME CONSERVATIVE CULL pbr's meshlet entry does: one workgroup per meshlet, its object-space sphere
    // pushed through the draw's model and the camera's view*proj, the radius scaled by an upper bound on both
    // matrices' axis scales. A culled meshlet emits NOTHING, which leaves those pixels to a nearer triangle -
    // and the test is deliberately conservative, because a visible meshlet culled here is a hole.
    // a descriptor HANDLE converts to its block only through a member access (the pattern every other stage
    // uses as `camera_at(...).view`), so the two matrices are read member-wise rather than copied as a struct.
    const VR_MAT4 view_proj = mul(visibility_camera.view, visibility_camera.proj);
    const MeshletRecord meshlet = meshlet_at(geometry.first_index + group_id.x);
    const float4 clip_center = mul(mul(float4(meshlet.center, 1.0), push.model), view_proj);
    const float radius = meshlet.radius * matrix_max_axis_scale(push.model) * matrix_max_axis_scale(view_proj);
    if (!clip_sphere_visible(clip_center, radius))
    {
        SetMeshOutputCounts(0u, 0u);
        return;
    }

    // THE ID IS THE MESHLET'S ORDER INSIDE THE ITEM, not its table index: the resolve pass reads the item's
    // record to find the run's base, so the two halves agree on the order the dispatch was built with.
    const uint meshlet_in_item = group_id.x;
    for (uint t = 0u; t < emit; ++t)
    {
        const uint vis_id = visibility_id_valid | (push.item_index << visibility_id_item_shift) |
                            (meshlet_in_item << visibility_id_meshlet_shift) | t;
        for (uint corner = 0u; corner < mesh_indices_per_triangle; ++corner)
        {
            const uint raw_index = mesh_index_at(geometry, geometry.first_index + (first_triangle + t) * mesh_indices_per_triangle + corner);
            const uint vertex_index = uint(int(raw_index) + geometry.base_vertex);
            const MeshVertex v = mesh_vertex_at(geometry, vertex_index);
            VisibilityVertexOutput o;
            o.position = mul(mul(float4(v.position, 1.0), push.model), view_proj);
            o.vis_id = vis_id;
            verts[t * mesh_indices_per_triangle + corner] = o;
        }
        tris[t] = uint3(t * mesh_indices_per_triangle + 0u, t * mesh_indices_per_triangle + 1u, t * mesh_indices_per_triangle + 2u);
    }
}

[shader("fragment")]
void main([[vk::location(0)]] nointerpolation uint vis_id : TEXCOORD0, out uint target : SV_Target0)
{
    // ONE STORE, and it is the whole fragment stage: depth was decided by the rasterizer, and the resolve pass
    // turns this word back into the triangle it names.
    target = vis_id;
}
