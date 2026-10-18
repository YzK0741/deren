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
//  * it does not choose a TEXTURE MIP the way a fragment does. The material fetch is the shared decode's
//    EXPLICIT-LOD-0 spelling - a compute stage cannot use an implicit `Sample` (Slang would declare the
//    `ComputeDerivativeGroupQuadsKHR` capability, which this renderer's device does not enable; see
//    heap_access.slang's `heap_sample_lod0`) - while the fragment path's derivative-driven sampler picks a higher mip
//    on a minified surface. That is an EXPECTED difference between the two pictures on magnified-away detail (V0.6
//    quantifies it), not a bug: pinning LOD 0 on the fragment side too would change the old path's picture, which is
//    a bigger decision than v0.
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
// THE COMPUTE STAGE'S TEXTURE FETCH (see surface_body.slang's macro note): explicit LOD 0, for the capability reason
// above. DEFINED BEFORE THE SHARED BODY IS INCLUDED, because the macro is expanded where the body's function is
// defined; `gbuffer_body.slang` includes `surface_body.slang`, so the override must arrive first.
#define DEREN_SURFACE_SAMPLE_TEXTURE heap_sample_lod0
#include "scene/gbuffer_body.slang"

// This stage's only push fields: the two heap indices every heap-native stage carries, plus the third lane the
// renderer's push endpoint always sends (the post chain's source slot - declared so the block is CONTIGUOUS, see
// surface.slang's `spare_lane` note). No dimensions: `RWTexture2D::GetDimensions` answers them, which keeps the
// dispatch's extent out of a second place that could disagree with the images.
struct VisibilityResolvePush
{
    uint frame_slot;  // which per-frame lane the item table and the camera read through
    uint image_index; // which per-swapchain-image lane the id/depth/output slots read through
    uint spare_lane;  // the endpoint's third lane: pushed and never read
};
[[vk::push_constant]] ConstantBuffer<VisibilityResolvePush> push;
#define heap_frame_slot (push.frame_slot)
#define heap_image_index (push.image_index)

// ---- 1. THE PACKED ID (mirror of deren.engine.visibility::id and of visibility.slang) ----
static const uint visibility_id_triangle_shift = 0u;
static const uint visibility_id_triangle_mask = 0x7Fu;    // 7 bits
static const uint visibility_id_meshlet_shift = 7u;
static const uint visibility_id_meshlet_mask = 0x3FFFu;   // 14 bits
static const uint visibility_id_item_shift = 21u;
static const uint visibility_id_item_mask = 0x3FFu;       // 10 bits
static const uint visibility_id_valid = 1u << 31u;
/// the draw-item table's rows PER FRAME LANE (mirror of deren.engine.visibility::id_item_capacity): the host writes
/// a frame's rows at `frame_slot * this + item`, so a resolve adds exactly this stride to the INDEX - the table is one
/// descriptor with the lanes inside it, not a per-frame array of descriptors (see the item fetch below).
static const uint visibility_item_capacity = 1024u;

// ---- 2. THE PER-FRAME DRAW-ITEM RECORD (mirror of deren.engine.visibility::draw_item, 112 bytes) ----
// Field order and byte offsets are the C++ record's; `model` stays the LAST member because the host writes the
// 48 bytes of lanes and then the 16-float matrix that came out of the same push block the raster used. The matrix
// is declared `VR_MAT4` (`row_major float4x4` = SPIR-V ColMajor), which is the spelling the host's column-major
// glm::mat4 bytes are read with - exactly as the raster stage's push block reads it.
struct VisibilityDrawItem
{
    uint vertex_address_low;  // device address of the item's vertex buffer base, low 32 bits
    uint vertex_address_high; // ... and high 32
    uint index_address_low;   // device address of the item's index buffer base, low 32 bits
    uint index_address_high;  // ... and high 32
    uint first_index;         // the item's index window (informational: the meshlet record carries the real one)
    int base_vertex;          // informational, same reason
    uint index_width;         // bytes per index: 2 or 4
    uint meshlet_base;        // where the item's meshlet run starts in its source table
    uint meshlet_table;       // 0 = the import-time table, 1 = this frame's host-culled table
    uint material_index;      // the material the raster fragment would have shaded with
    uint motion_index;        // previous_transform index (the velocity half)
    uint flags;               // reserved: 0 in v0
    VR_MAT4 model;            // row-major in the record's spelling: the draw item's model matrix
};

// ---- 3. THE CAMERA FIELDS THIS STAGE READS (mirror of deren.vulkan::camera_ubo's order) ----
// `proj` is the JITTERED projection (the raster lane used it, so the reconstructed clip positions are on the same
// sub-pixel offsets), while the motion vector needs the unjittered pair - exactly as `gbuffer.slang` reads them.
// The middle members are declared although unread: a block member's offset is its position in the layout.
struct VisibilityCamera
{
    VR_MAT4 view;
    VR_MAT4 proj;
    float4 camera_pos;
    VR_MAT4 view_proj_unjittered;
    VR_MAT4 prev_view_proj;
};
/// @brief this frame's camera through the heap: the `heap_camera_slot` macro (added to `heap_frame_slot`) is what
///        picks the per-frame slot, exactly as every scene stage reads it - and the block type above is declared
///        HERE rather than shared because `lighting/shading.slang`'s CameraUBO cannot be included by a compute
///        stage (it comes with the 144-byte scene push block; see surface_body.slang's header).
#define visibility_camera heap_at<ConstantBuffer<VisibilityCamera>>(heap_camera_slot)

/// the depth check's tolerance, in NDC z: the rasterizer interpolates z_ndc linearly in screen space and this stage
/// recomputes it from the same three vertices, so the two agree to float rounding - a mismatch means the id names a
/// different triangle than the depth the pixel kept, which is the one failure a resolved surface could hide.
static const float visibility_depth_tolerance = 1e-3;

/// @brief the pixel position of a clip-space position, or false when the vertex is behind the eye
/// @note THE Y SIGN IS VULKAN'S, not a choice: framebuffer y grows DOWN and clip-space y grows down too, so the
///       transform is (+0.5*y_ndc + 0.5)*height with no mirror - this projection already carries the mirror
///       (its proj[1][1] is negative). The CPU reference (`visibility::clip_to_screen`) pins the same expression.
bool clip_to_screen_pixel(const float4 clip, const float2 extent, out float2 pixel) {
    if (clip.w <= 0.0) {
        pixel = float2(0.0, 0.0);
        return false;
    }
    const float inv_w = 1.0 / clip.w;
    pixel = float2((clip.x * inv_w * 0.5 + 0.5) * extent.x, (clip.y * inv_w * 0.5 + 0.5) * extent.y);
    return true;
}

/// @brief twice the SIGNED area of the screen-space triangle, the sign the rasterizer's front-face rule uses
/// @note frontFace is COUNTER_CLOCKWISE in this renderer, and in Vulkan's y-down framebuffer coordinates a
///       counter-clockwise triangle has NEGATIVE area under this cross product - so `area < 0` is FRONT. The CPU
///       reference's `visibility::is_front_face` pins that convention with a test.
float triangle_twice_area(const float2 a, const float2 b, const float2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

/// @brief the screen-space barycentrics of @p p in the triangle (@p a, @p b, @p c)
/// @note ALL ZERO for a degenerate triangle - a defined answer rather than a division by zero (the caller treats it
///       as "no triangle"). The same signed weights the CPU reference's `screen_barycentrics` returns.
void screen_barycentrics(const float2 p, const float2 a, const float2 b, const float2 c, out float3 weights) {
    const float twice_area = triangle_twice_area(a, b, c);
    if (twice_area == 0.0) {
        weights = float3(0.0, 0.0, 0.0);
        return;
    }
    const float wa = ((b.x - p.x) * (c.y - p.y) - (b.y - p.y) * (c.x - p.x)) / twice_area;
    const float wb = ((c.x - p.x) * (a.y - p.y) - (c.y - p.y) * (a.x - p.x)) / twice_area;
    weights = float3(wa, wb, 1.0 - wa - wb);
}

/// @brief perspective-correct the screen-space weights with the vertices' clip w
/// @return the object-space weights (normalized to one), or all zero when they collapse (degenerate input)
float3 perspective_correct_weights(const float3 screen_weights, const float3 clip_w) {
    if (clip_w.x == 0.0 || clip_w.y == 0.0 || clip_w.z == 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 raw = screen_weights / clip_w;
    const float sum = raw.x + raw.y + raw.z;
    if (sum == 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    return raw / sum;
}

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
    // writes a frame's 1024 rows at `frame_slot * capacity + item` (runtime::record_visibility_items and
    // `visibility_items_write`), and the descriptor sits at the single slot the host wrote it at - so the frame
    // slot is part of the INDEX, never added to the heap slot. (Measured the hard way: adding it to the slot
    // instead read the next array's descriptor - a storage IMAGE - as a structured buffer, and the device was
    // lost on the frame that used the second slot.)
    const uint item_row = push.frame_slot * visibility_item_capacity + id_item;
    const VisibilityDrawItem item = heap_at<StructuredBuffer<VisibilityDrawItem>>(heap_slots_visibility_items)[item_row];

    // ---- the meshlet record: the item's run base plus the id's order inside the run (the CPU reference's
    //      `resolve_indices` states the same arithmetic) ----
    const MeshletRecord meshlet = item.meshlet_table == 0u ? meshlet_at(item.meshlet_base + id_meshlet)
                                                           : culled_meshlet_at(heap_frame_slot, item.meshlet_base + id_meshlet);
    if (id_triangle * 3u >= meshlet.index_count) {
        unresolved = true; // an id past its meshlet's window cannot name a triangle
        return gbuffer_empty_output();
    }

    // ---- the geometry fetch: the SAME 64-byte interleaved vertex and index decode the raster lane used, through
    //      the same helpers (`mesh_geometry.slang`), with the item's addresses and the meshlet's own window ----
    MeshGeometryLanes geometry;
    geometry.vertex_address_low = item.vertex_address_low;
    geometry.vertex_address_high = item.vertex_address_high;
    geometry.index_address_low = item.index_address_low;
    geometry.index_address_high = item.index_address_high;
    geometry.first_index = 0u;
    geometry.index_count = 0u;
    geometry.base_vertex = 0; // unused: the meshlet record carries the run's own base vertex (the meshlet entries' rule)
    geometry.index_width = item.index_width;

    const VR_MAT4 view_proj = mul(visibility_camera.view, visibility_camera.proj);
    visibility_triangle triangle;
    for (uint corner = 0u; corner < mesh_indices_per_triangle; ++corner) {
        const uint raw_index = mesh_index_at(geometry, meshlet.first_index + id_triangle * mesh_indices_per_triangle + corner);
        const uint vertex_index = uint(int(raw_index) + meshlet.base_vertex);
        const MeshVertex v = mesh_vertex_at(geometry, vertex_index);
        // THE SAME TWO PRODUCTS THE RASTER STAGE COMPUTED for this vertex: object -> world by the item's model, then
        // world -> clip by the camera pair the raster configured itself with (view*proj, jitter included).
        const float4 world = mul(float4(v.position, 1.0), item.model);
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
        triangle.clip[corner] = mul(world, view_proj);
        // the previous world position is the LOCAL position through last frame's model matrix: v0 admits static
        // draws only (no skin, no morph), so this is the whole deformation term - see gbuffer.slang's note.
        triangle.prev_world[corner] = mul(float4(v.position, 1.0), previous_transform_at(heap_previous_slot, item.motion_index)).xyz;
    }

    // ---- where the triangle is on screen, and whether the pixel is inside it ----
    float2 screen[3];
    for (uint corner = 0u; corner < mesh_indices_per_triangle; ++corner) {
        if (!clip_to_screen_pixel(triangle.clip[corner], extent, screen[corner])) {
            // a vertex behind the eye: the rasterizer clipped this triangle, and v0 cannot rebuild the clipped
            // triangle from three projected points (see the header). Counted, not silently wrong.
            unresolved = true;
            return gbuffer_empty_output();
        }
    }
    const float2 centre = float2(float(pixel.x) + 0.5, float(pixel.y) + 0.5);
    float3 screen_weights;
    screen_barycentrics(centre, screen[0], screen[1], screen[2], screen_weights);
    if (all(screen_weights == float3(0.0, 0.0, 0.0))) {
        unresolved = true; // degenerate screen triangle: it covered no pixel
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

    // ---- the SHARED material decode, then the SHARED five-target composition: the same two phases the fragment
    //      G-buffer stage calls, so the two producers cannot disagree about what a surface is. THE ORDER OF THE TWO
    //      PHASES IS THE FRAGMENT PATH's (MASK verdict first, everything else after); this stage has no derivative to
    //      place between them and no discard to run, so it simply branches on the verdict. ----
    const surface_base base = gather_surface_base(item.material_index, uv);
    if (base.alpha_masked) {
        unresolved = true; // v0 excludes MASK materials: a resolve cannot cut a pixel whose depth is already written
        return gbuffer_empty_output();
    }
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
    const uint packed = heap_at<Texture2D<uint>>(heap_image_slot(heap_slots_visibility)).Load(int3(pixel, 0));
    const float2 uv = (float2(pixel) + 0.5) / extent;
    const float depth_value = heap_texel_lod0(heap_at<Texture2D<float4>>(heap_image_slot(heap_slots_gbuffer_depth)), heap_sampler_gbuffer, uv).r;

    bool depth_mismatch = false;
    bool unresolved = false;
    const gbuffer_output out = resolve_pixel(pixel, packed, depth_value, extent, depth_mismatch, unresolved);

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
