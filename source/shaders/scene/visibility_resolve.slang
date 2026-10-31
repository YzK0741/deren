// shaders/scene/visibility_resolve.slang - THE VISIBILITY BUFFER'S SECOND HALF (visibility-buffer v0.4b).
//
// PRODUCES `visibility_resolve.comp.spv`, the compute stage that turns a pixel's packed id back into the five
// G-buffer targets. The raster lane (`visibility.slang`) wrote ONE `r32_uint` word per pixel - the triangle that won
// the depth test - and the frame's own G-buffer depth (v0.5: the raster lane is the opaque producer, so the depth it
// wrote is the one the lighting stage samples); this stage reads both, rebuilds the triangle's world-space attributes
// from the per-frame draw-item and meshlet tables, and writes the same five values `scene/gbuffer.slang` writes,
// through the same shared bodies (`scene/gbuffer_body.slang` + `common/surface_body.slang`). Downstream pixel
// processing is unchanged: deferred lighting / TAA / post read the same G-buffer semantics they always did.
//
// WHY A COMPUTE STAGE: one thread per pixel over the whole extent, and the camera the raster used. It reads no
// interpolators and has no derivatives, so everything a fragment stage gets from the rasterizer is reconstructed
// here: the barycentric weights from the pixel centre and the triangle's projected vertices (THE MATH IS THE CPU
// REFERENCE's, `deren.engine.visibility` section 4, mirrored by hand - see the notes on each function), the
// perspective correction from the vertices' clip w, and the tangent frame from the TRIANGLE'S OWN EDGES rather
// than ddx/ddy (`surface_tbn`'s frame and this one describe the same plane - see `gather_surface_finish`).
//
// WHAT IT DOES NOT DO, deliberately:
//  * it does not write DEPTH. The raster lane owns the depth target: the depth test is the visibility decision,
//    and a compute pass has no load/store semantics over it. The depth is READ back here for a correctness check
//    of the reconstructed triangle (see `visibility_depth_tolerance`): the pixel's stored depth must belong to
//    the triangle the id names, and the counter is how a wrong-id resolve would be caught.
//  * it does not handle alphaMode MASK: a rasterized fragment's depth is already written, so a resolve "discard"
//    cannot cut the pixel out of the picture. v0 therefore excludes MASK materials from the visibility path
//    entirely (the frame falls back to the old geometry path - V0.5's eligibility rule), and this stage treats a
//    masked surface as "no surface" rather than shading a cut-out that the old path would not have drawn.
//  * it does not CLIP a triangle against the near plane. A triangle with a vertex behind the eye cannot be
//    rebuilt from three projected vertices; the pixel is left empty (and counted in
//    `mesh_stat_visibility_unresolved`) rather than projected through a mirrored point. v0 accepts that hole as a
//    known limitation and V0.6 measures it; a proper resolve clips the triangle.
//  * it does not choose a TEXTURE MIP exactly the way a fragment does. The material fetch is the shared decode's
//    EXPLICIT-LOD spelling (a compute stage cannot use an implicit `Sample`: Slang would declare the
//    `ComputeDerivativeGroupQuadsKHR` capability, which this renderer's device does not enable), and the LOD it
//    passes is rebuilt from the TRIANGLE's UV area against its screen area (see `visibility_texture_lod`). That
//    reproduces the mip RANGE the fragment sampler lands in, not its per-pixel gradient or its anisotropy - the
//    residue is one of the two known v0 difference sources (V0.6 measures it; see the leaf's helper note).
//
// THE ID, THE RECORD AND THE TABLES ARE READ THROUGH THE HEAP, like every stage: the id and the depth are per-image
// slots, the item table is a per-frame lane, and the meshlet table is one descriptor written at import. The graph
// declares those accesses (render_graph/visibility.cppm's resolve lane) - this stage only indexes them.
//
// THE PACKING IS `deren.engine.visibility`'s C++ layout, mirrored here by hand (a shader cannot include a C++
// header; this is the third mirror, after `visibility.slang` and the CPU reference, and `test_visibility_buffer`
// parses all three). The DRAW ITEM RECORD is mirrored for the same reason, 112 bytes exactly.

#include "common/heap_access.slang"
#include "geometry/mesh_geometry.slang"
#include "common/hzb.slang"

// ---- THE FRAGMENT SAMPLER'S MIP, REBUILT FROM THE TRIANGLE (visibility-buffer v0.6 quality alignment) --------
// A compute thread has no derivatives, so the shared decode's fetches are EXPLICIT SampleLevel calls. v0 used
// LOD 0 for all of them, which made a minified surface sample a sharper mip than the fragment path's
// derivative-driven sampler - one of the two measured sources of the ON/OFF difference (see
// docs/reviews/2026-10-18-visibility-buffer-ab.md). The LOD is now derived from the SAME quantity that
// approximate derivative is: this triangle's UV-per-pixel scale, `sqrt(|uv area| / |screen area in pixels|)`.
// A per-pixel gradient and a per-triangle scale differ on a heavily foreshortened triangle, and the hardware's
// anisotropic choice is not reproduced; what IS reproduced is the mip RANGE the fragment sampler lands in, which
// is what a mip miss changes. Set by resolve_pixel before the decode; the helpers live in THIS leaf (not in
// heap_access.slang) so adding them does not recompile every other shader.
uint scene_texture_base();
static float visibility_uv_footprint = 0.0; // uv units per render pixel at the current pixel's surface

float2 visibility_texture_size(uint texture_index)
{
    uint width, height, levels;
    heap_at<Texture2D<float4>>(scene_texture_base() + texture_index).GetDimensions(0u, width, height, levels);
    return float2(float(width), float(height));
}

float visibility_texture_lod(uint texture_index)
{
    const float2 size = visibility_texture_size(texture_index);
    return max(0.0, log2(max(size.x, size.y) * visibility_uv_footprint)); // a magnified surface keeps mip 0
}

float4 visibility_sample_lod(uint texture_index, float2 uv)
{
    return combined_at(scene_texture_base() + texture_index, heap_sampler_texture).SampleLevel(uv, visibility_texture_lod(texture_index));
}

// THE COMPUTE STAGE'S TEXTURE FETCH IS STILL EXPLICIT-LOD (the capability reason stays: an implicit `Sample` in
// compute declares `ComputeDerivativeGroupQuadsKHR`, which vkCreateShaderModule refuses unless the device enabled
// it). DEFINED BEFORE THE SHARED BODY IS INCLUDED, because the macro is expanded where the body's function is
// defined; `gbuffer_body.slang` includes `surface_body.slang`, so the override must arrive first.
#define DEREN_SURFACE_SAMPLE_TEXTURE(texture_index, uv) visibility_sample_lod(texture_index, uv)
#include "scene/gbuffer_body.slang"

// The endpoint supplies frame/image indices and the existing software-visibility flag. The pass appends
// an optional feedback slot as a fourth word, keeping those three lanes intact. Image dimensions come
// from GetDimensions, so resizing does not require another copy of the render extent.
struct VisibilityResolvePush
{
    uint frame_slot;  // which per-frame lane the item table and the camera read through
    uint image_index; // which per-swapchain-image lane the id/depth/output slots read through
    uint spare_lane;  // existing software/hybrid visibility flag
    uint mip_feedback_slot; // optional extent-feedback descriptor; zero disables requests
};
[[vk::push_constant]] ConstantBuffer<VisibilityResolvePush> push;
#define heap_frame_slot (push.frame_slot)
#define heap_image_index (push.image_index)
uint scene_texture_base() { return heap_slots_textures + push.frame_slot * heap_texture_bank_stride; }

#include "common/mesh_fetch.slang"
#include "common/software_candidate.slang"

/// @brief the three world-space positions / normals / uvs / previous positions of the id's triangle
struct visibility_triangle {
    float3 world[3];      // this frame's world-space vertex positions
    float3 normal[3];     // this frame's world-space vertex normals
    float2 uv[3];         // texture coordinates
    float3 prev_world[3]; // LAST frame's world-space vertex positions (the velocity half)
    float4 clip[3];       // clip-space positions under the JITTERED view*proj
};

/**
 * @brief resolve one pixel's id into the five G-buffer values
 * @param pixel the pixel index (the value the dispatch thread id names)
 * @param packed the raw visibility word the raster wrote for this pixel (valid bit included)
 * @param depth_value the frame's G-buffer depth for the pixel (NDC z, 0..1), read by the caller
 * @param extent the render extent in pixels
 * @param depth_mismatch set when the reconstructed triangle's z disagrees with @p depth_value
 * @param unresolved set when a VALID id could not be resolved at all (degenerate / behind the eye)
 * @return the five targets' values, or `gbuffer_empty_output()` when the pixel has no surface to write
 * @note the failure outputs are DIAGNOSTICS, not culling decisions: the picture is the same either way (the pixel
 *       stays empty), and the counters are what V0.4c/V0.6 reads to prove the lane ran and to quantify the known
 *       near-plane limitation.
 */
gbuffer_output resolve_pixel(const uint2 pixel,
                             const uint packed,
                             const float depth_value,
                             const float2 extent,
                             const bool software_selected,
                             out bool depth_mismatch,
                             out bool unresolved) {
    depth_mismatch = false;
    unresolved = false;
    if ((packed & visibility_id_valid) == 0u) {
        return gbuffer_empty_output(); // the clear value: "no geometry won this pixel"
    }

    // ---- the id's three fields, then the item row the raster dispatched ----
    const uint id_item = (packed >> visibility_id_item_shift) & visibility_id_item_mask;
    const uint id_meshlet = (packed >> visibility_id_meshlet_shift) & visibility_id_meshlet_mask;
    const uint id_triangle = (packed >> visibility_id_triangle_shift) & visibility_id_triangle_mask;
    // THE ITEM TABLE IS ONE DESCRIPTOR WITH PER-FRAME LANES INSIDE IT, not a per-frame array of slots: the host
    // writes a frame's 4096 rows at `frame_slot * capacity + item` (runtime::record_visibility_items and
    // `visibility_items_write`), and the descriptor sits at the single slot the host wrote it at - so the frame
    // slot is part of the INDEX, never added to the heap slot. (Measured the hard way: adding it to the slot
    // instead read the next array's descriptor - a storage IMAGE - as a structured buffer, and the device was
    // lost on the frame that used the second slot.)
    const uint item_row = push.frame_slot * visibility_item_capacity + id_item;
    const VisibilityDrawItem item = heap_at<StructuredBuffer<VisibilityDrawItem>>(heap_slots_visibility_items)[item_row];

    // ---- the meshlet record: the item's run base plus the id's order inside the run (the CPU reference's
    //      `resolve_indices` states the same arithmetic) ----
    bool hzb_live = false;
    if (item.meshlet_table == 2u)
    {
        hzb_live = hzb_survivors_usable(heap_frame_slot, id_item);
    }
    const MeshletRecord meshlet = software_selected && item.meshlet_table == 2u
        ? culled_meshlet_at(heap_frame_slot, item.meshlet_base + id_meshlet)
        : (hzb_live ? hzb_survivor_at(heap_frame_slot, item.meshlet_base + id_meshlet)
                    : (item.meshlet_table == 0u ? meshlet_at(item.meshlet_base + id_meshlet)
                                               : culled_meshlet_at(heap_frame_slot, item.meshlet_base + id_meshlet)));
    if (id_triangle * 3u >= meshlet.index_count) {
        unresolved = true; // an id past its meshlet's window cannot name a triangle
        return gbuffer_empty_output();
    }

    // ---- the geometry fetch: the SAME 80-byte interleaved vertex and index decode the raster lane used, through
    //      the same helpers (`mesh_geometry.slang`), with the item's addresses and the meshlet's own window ----
    const MeshGeometryLanes geometry = visibility_geometry(item);

    const VR_MAT4 view_proj = mul(visibility_camera.view, visibility_camera.proj);
    visibility_triangle triangle;
    for (uint corner = 0u; corner < mesh_indices_per_triangle; ++corner) {
        const MeshVertex v = visibility_vertex(geometry, meshlet, id_triangle, corner);
        // THE SAME TWO PRODUCTS THE RASTER STAGE COMPUTED for this vertex: object -> world by the item's model, then
        // world -> clip by the camera pair the raster configured itself with (view*proj, jitter included).
        const float4 world = visibility_world(v.position, item.model);
        triangle.world[corner] = world.xyz;
        // THE NORMAL GOES THROUGH THE MODEL TOO, and it is the attribute a missing transform is LEAST likely to
        // announce: the fragment path's mesh stage writes `normalize(mul(object_normal, world))`, so a resolve that
        // interpolated the OBJECT-space normal would hand `gather_surface_finish` a geometric normal in the model's
        // frame and light the surface with it - a whole-frame shading shift, measured on this plan's first admitted
        // capture (the helmet's normals came out rotated, its G-buffer normal channel differing in z while x/y
        // agreed, which is what a Y-up -> Z-up model rotation looks like when one producer forgets it). w = 0 keeps
        // the translation out of a direction.
        triangle.normal[corner] = mul(float4(v.normal, 0.0), item.model).xyz;
        triangle.uv[corner] = v.uv;
        triangle.clip[corner] = visibility_clip(world, view_proj);
        // the previous world position is the LOCAL position through last frame's model matrix: v0 admits static
        // draws only (no skin, no morph), so this is the whole deformation term - see gbuffer.slang's note.
        triangle.prev_world[corner] = mul(float4(v.position, 1.0), previous_transform_at(heap_previous_slot, item.motion_index)).xyz;
    }

    // ---- THE NEAR-PLANE CLIP (visibility-buffer item 3) ----
    // The rasterizer clips a triangle that crosses the eye plane; v0 projected the three corners and gave up when
    // one had w <= 0, which measured as 99.25% of sponza's unresolved pixels (852,390 of 858,868). The same three
    // clip-space vertices are clipped here against w >= EPS (Sutherland-Hodgman), the attributes ride the SAME
    // edge parameter as the positions - correct because they are interpolated linearly in homogeneous space, which
    // is exactly the space the rasterizer interpolates a clipped edge in - and the resulting polygon is fanned
    // back into triangles. The pixel lies in exactly one fan triangle (the id won for a pixel the raster clipped
    // polygon covered), so the first triangle whose screen barycentrics contain the centre is the one to rebuild.
    const float clip_epsilon = 1e-5;
    float4 clipped_clip[4];
    float3 clipped_world[4];
    float3 clipped_normal[4];
    float2 clipped_uv[4];
    float3 clipped_prev_world[4];
    uint clipped_count = 0u;
    uint behind = 0u;
    for (uint corner = 0u; corner < mesh_indices_per_triangle; ++corner)
        behind += triangle.clip[corner].w < clip_epsilon ? 1u : 0u;
    if (behind == mesh_indices_per_triangle) {
        mesh_stat_add(push.frame_slot, mesh_stat_visibility_clipped, 1u);
        unresolved = true; // the whole triangle is behind the eye: the rasterizer never wrote a pixel for it
        return gbuffer_empty_output();
    }
    for (uint corner = 0u; corner < mesh_indices_per_triangle; ++corner) {
        const uint next = (corner + 1u) % mesh_indices_per_triangle;
        const float wa = triangle.clip[corner].w;
        const float wb = triangle.clip[next].w;
        const bool inside_a = wa >= clip_epsilon;
        const bool inside_b = wb >= clip_epsilon;
        if (inside_a) {
            clipped_clip[clipped_count] = triangle.clip[corner];
            clipped_world[clipped_count] = triangle.world[corner];
            clipped_normal[clipped_count] = triangle.normal[corner];
            clipped_uv[clipped_count] = triangle.uv[corner];
            clipped_prev_world[clipped_count] = triangle.prev_world[corner];
            ++clipped_count;
        }
        if (inside_a != inside_b) {
            const float t_clip = (clip_epsilon - wa) / (wb - wa);
            clipped_clip[clipped_count] = lerp(triangle.clip[corner], triangle.clip[next], t_clip);
            clipped_world[clipped_count] = lerp(triangle.world[corner], triangle.world[next], t_clip);
            clipped_normal[clipped_count] = lerp(triangle.normal[corner], triangle.normal[next], t_clip);
            clipped_uv[clipped_count] = lerp(triangle.uv[corner], triangle.uv[next], t_clip);
            clipped_prev_world[clipped_count] = lerp(triangle.prev_world[corner], triangle.prev_world[next], t_clip);
            ++clipped_count;
        }
    }
    if (behind != 0u)
        mesh_stat_add(push.frame_slot, mesh_stat_visibility_clipped, 1u); // counted at the crossing itself

    // ---- where the clipped polygon is on screen, and which fan triangle holds the pixel ----
    const float2 centre = float2(float(pixel.x) + 0.5, float(pixel.y) + 0.5);
    float2 screen[3];
    float3 screen_weights;
    bool found = false;
    for (uint fan = 0u; fan + 3u <= clipped_count && !found; ++fan) {
        const uint i0 = 0u;
        const uint i1 = fan + 1u;
        const uint i2 = fan + 2u;
        bool projects = true;
        projects = projects && clip_to_screen_pixel(clipped_clip[i0], extent, screen[0]);
        projects = projects && clip_to_screen_pixel(clipped_clip[i1], extent, screen[1]);
        projects = projects && clip_to_screen_pixel(clipped_clip[i2], extent, screen[2]);
        if (!projects)
            continue;
        screen_barycentrics(centre, screen[0], screen[1], screen[2], screen_weights);
        // THE UNCLIPPED TRIANGLE KEEPS v0's BEHAVIOUR EXACTLY: the id was written by the rasterizer for this
        // pixel, so its own triangle is accepted as it was before this clip existed (an edge pixel can produce
        // a barycentric a hair negative through the top-left rule, and rejecting it would turn a rendered pixel
        // into a hole). Only a CLIPPED polygon needs the containment test: its fan triangles are new geometry.
        if (behind == 0u) {
            if (all(screen_weights == float3(0.0, 0.0, 0.0))) {
                unresolved = true; // degenerate screen triangle: it covered no pixel (v0's own verdict)
                return gbuffer_empty_output();
            }
        } else if (any(screen_weights < float3(-1e-5, -1e-5, -1e-5))) {
            continue; // this fan triangle does not cover the pixel; the next one (if any) does
        }
        triangle.clip[0] = clipped_clip[i0];
        triangle.clip[1] = clipped_clip[i1];
        triangle.clip[2] = clipped_clip[i2];
        triangle.world[0] = clipped_world[i0];
        triangle.world[1] = clipped_world[i1];
        triangle.world[2] = clipped_world[i2];
        triangle.normal[0] = clipped_normal[i0];
        triangle.normal[1] = clipped_normal[i1];
        triangle.normal[2] = clipped_normal[i2];
        triangle.uv[0] = clipped_uv[i0];
        triangle.uv[1] = clipped_uv[i1];
        triangle.uv[2] = clipped_uv[i2];
        triangle.prev_world[0] = clipped_prev_world[i0];
        triangle.prev_world[1] = clipped_prev_world[i1];
        triangle.prev_world[2] = clipped_prev_world[i2];
        found = true;
    }
    if (!found) {
        unresolved = true; // degenerate screen triangle: the clipped polygon covered no pixel
        return gbuffer_empty_output();
    }

    // ---- the depth check the G-buffer depth makes possible: NDC z is linear in SCREEN space, so the SCREEN weights
    //      (not the perspective-corrected ones) recover the depth the rasterizer wrote for this centre ----
    const float z_ndc = screen_weights.x * (triangle.clip[0].z / triangle.clip[0].w) + screen_weights.y * (triangle.clip[1].z / triangle.clip[1].w) +
                        screen_weights.z * (triangle.clip[2].z / triangle.clip[2].w);
    depth_mismatch = abs(z_ndc - depth_value) > visibility_depth_tolerance;

    // ---- the attributes: perspective-correct weights, then a linear interpolation of the three vertices ----
    const float3 weights = perspective_correct_weights(screen_weights, float3(triangle.clip[0].w, triangle.clip[1].w, triangle.clip[2].w));
    if (all(weights == float3(0.0, 0.0, 0.0))) {
        unresolved = true;
        return gbuffer_empty_output();
    }
    const float3 world_pos = weights.x * triangle.world[0] + weights.y * triangle.world[1] + weights.z * triangle.world[2];
    const float3 normal = weights.x * triangle.normal[0] + weights.y * triangle.normal[1] + weights.z * triangle.normal[2];
    const float2 uv = weights.x * triangle.uv[0] + weights.y * triangle.uv[1] + weights.z * triangle.uv[2];
    const float3 prev_world_pos = weights.x * triangle.prev_world[0] + weights.y * triangle.prev_world[1] + weights.z * triangle.prev_world[2];

    // ---- the tangent frame, from the TRIANGLE's edges: the same linear system `surface_tbn` solves from the
    //      derivatives, with the same degenerate-UV fallback (the normal-map path is the only consumer) ----
    const float3 dp1 = triangle.world[1] - triangle.world[0];
    const float3 dp2 = triangle.world[2] - triangle.world[0];
    const float2 duv1 = triangle.uv[1] - triangle.uv[0];
    const float2 duv2 = triangle.uv[2] - triangle.uv[0];
    const float uv_denom = duv1.x * duv2.y - duv2.x * duv1.y;
    float3 frame_tangent = float3(1.0, 0.0, 0.0);
    float3 frame_bitangent = float3(0.0, 1.0, 0.0);
    const bool frame_valid = abs(uv_denom) >= 1e-8;
    if (frame_valid) {
        frame_tangent = normalize((duv2.y * dp1 - duv1.y * dp2) / uv_denom);
        frame_bitangent = normalize((duv1.x * dp2 - duv2.x * dp1) / uv_denom);
    }

    // the rasterizer's front/back decision for this pixel: FRONT is a negative signed area (see triangle_twice_area)
    const bool is_front = triangle_twice_area(screen[0], screen[1], screen[2]) < 0.0;

    // ---- the UV-per-pixel scale the material fetch's explicit LOD is derived from (see the leaf's helpers) ----
    // The ratio of the triangle's UV area to its SCREEN area is the linear UV scale of this triangle in uv units
    // per pixel; the derivative helpers a fragment stage would call are the local version of the same quantity.
    const float twice_uv = abs((triangle.uv[1].x - triangle.uv[0].x) * (triangle.uv[2].y - triangle.uv[0].y) -
                               (triangle.uv[1].y - triangle.uv[0].y) * (triangle.uv[2].x - triangle.uv[0].x));
    const float twice_screen = abs(triangle_twice_area(screen[0], screen[1], screen[2]));
    visibility_uv_footprint = twice_screen > 0.0 ? sqrt(twice_uv / twice_screen) : 0.0;
    if (push.mip_feedback_slot != 0u && item.material_index < 16384u) {
        // Demand comes from geometry, independently of currently resident texture dimensions.
        float desired = visibility_uv_footprint > 0.0 && isfinite(visibility_uv_footprint)
            ? min(16384.0, ceil(2.0 / visibility_uv_footprint)) : 16384.0;
        uint ignored;
        heap_at<RWByteAddressBuffer>(push.mip_feedback_slot).InterlockedMax(item.material_index * 4u, uint(max(1.0, desired)), ignored);
    }

    // ---- the SHARED material decode, then the SHARED five-target composition: the same two phases the fragment
    //      G-buffer stage calls, so the two producers cannot disagree about what a surface is. THE ORDER OF THE TWO
    //      PHASES IS THE FRAGMENT PATH's (MASK verdict first, everything else after); this stage has no derivative to
    //      place between them and no discard to run, so it simply branches on the verdict. ----
    const surface_base base = gather_surface_base(item.material_index, uv);
    // A masked MASK pixel cannot reach this point: the RASTER RASTER fragment ran the glTF test and discarded it
    // before the depth write (item 2), so a valid id means the raster KEPT the fragment. This stage therefore does
    // NOT re-cut it: its own texel can differ by a mip (the LOD is rebuilt from the triangle, not the derivative),
    // and re-cutting would turn a raster-accepted pixel into a hole. The verdict belongs to the lane that owns the
    // depth test.
    const surface_sample surface = gather_surface_finish(base, normal, uv, is_front, frame_tangent, frame_bitangent, frame_valid);
    const float2 velocity = motion_vector(world_pos, prev_world_pos, visibility_camera.view_proj_unjittered, visibility_camera.prev_view_proj);
    return gbuffer_from_surface(surface, item.material_index, velocity);
}

[numthreads(8, 8, 1)]
void main(const uint3 dispatch : SV_DispatchThreadID) {
    // THE FIVE OUTPUTS: the G-buffer family's storage twins (see heap_slot_constants.slang), one per swapchain image
    RWTexture2D<float4> albedo_metallic = heap_at<RWTexture2D<float4>>(heap_image_slot(heap_slots_gbuffer_albedo_storage));
    RWTexture2D<float4> normal_roughness = heap_at<RWTexture2D<float4>>(heap_image_slot(heap_slots_gbuffer_normal_storage));
    RWTexture2D<float4> material = heap_at<RWTexture2D<float4>>(heap_image_slot(heap_slots_gbuffer_material_storage));
    RWTexture2D<float2> velocity = heap_at<RWTexture2D<float2>>(heap_image_slot(heap_slots_gbuffer_velocity_storage));
    RWTexture2D<float4> scene_color = heap_at<RWTexture2D<float4>>(heap_image_slot(heap_slots_scene_color_storage));

    uint width = 0u;
    uint height = 0u;
    albedo_metallic.GetDimensions(width, height);
    if (dispatch.x >= width || dispatch.y >= height) {
        return; // a group straddling the right/bottom edge: the extra threads write nothing
    }
    const uint2 pixel = dispatch.xy;
    const float2 extent = float2(float(width), float(height));

    // the two inputs: the id (integer load - no sampler, no filtering) and the frame's G-buffer depth (the proven
    // depth-fetch pattern from `gbuffer_debug`/`deferred`: the sampled float view through the nearest gbuffer sampler)
    uint packed = heap_at<Texture2D<uint>>(heap_image_slot(heap_slots_visibility)).Load(int3(pixel, 0));
    bool software_selected = false;
    const float2 uv = (float2(pixel) + 0.5) / extent;
    float depth_value = heap_texel_lod0(heap_at<Texture2D<float4>>(heap_image_slot(heap_slots_gbuffer_depth)), heap_sampler_gbuffer, uv).r;

    // The third endpoint lane freezes hybrid eligibility for this frame. OFF
    // never touches a software descriptor. Resolve still sees unmerged HW depth.
    if (push.spare_lane != 0u) {
        const uint sw_id = heap_at<Texture2D<uint>>(heap_image_slot(heap_slots_software_id)).Load(int3(pixel, 0));
        const uint sw_depth = heap_at<Texture2D<uint>>(heap_image_slot(heap_slots_software_depth)).Load(int3(pixel, 0));
        if (choose_visibility_candidate(packed, depth_value, sw_id, sw_depth)) {
            packed = sw_id;
            depth_value = asfloat(sw_depth);
            software_selected = true;
            mesh_stat_add(push.frame_slot, 14u, 1u);
        }
    }
    bool depth_mismatch = false;
    bool unresolved = false;
    const gbuffer_output out = resolve_pixel(pixel, packed, depth_value, extent, software_selected, depth_mismatch, unresolved);

    albedo_metallic[pixel] = out.albedo_metallic;
    normal_roughness[pixel] = out.normal_roughness;
    material[pixel] = out.material;
    velocity[pixel] = out.velocity;
    scene_color[pixel] = out.scene_color;
    // THE DIAGNOSTICS the capture lane's depth output and the id's correctness are witnessed by: two atomics per
    // FAILING pixel (a healthy frame increments neither). Counted in this frame's lane of the mesh statistics
    // buffer, which the runtime reads back and logs at shutdown - see runtime::log_meshlet_stats.
    if (depth_mismatch) {
        mesh_stat_add(push.frame_slot, mesh_stat_visibility_depth_mismatch, 1u);
    }
    if (unresolved) {
        mesh_stat_add(push.frame_slot, mesh_stat_visibility_unresolved, 1u);
    }
}
