#ifndef DEREN_MESH_FETCH_SLANG
#define DEREN_MESH_FETCH_SLANG
// Shared static-geometry decode, camera and projection for visibility producers.
// ---- 1. THE PACKED ID (mirror of deren.engine.visibility::id and of visibility.slang) ----
static const uint visibility_id_triangle_shift = 0u;
static const uint visibility_id_triangle_mask = 0x7Fu;    // 7 bits
static const uint visibility_id_meshlet_shift = 7u;
static const uint visibility_id_meshlet_mask = 0xFFFu;    // 12 bits
static const uint visibility_id_item_shift = 19u;
static const uint visibility_id_item_mask = 0xFFFu;       // 12 bits
static const uint visibility_id_valid = 1u << 31u;
/// the draw-item table's rows PER FRAME LANE (mirror of deren.engine.visibility::id_item_capacity): the host writes
/// a frame's rows at `frame_slot * this + item`, so a resolve adds exactly this stride to the INDEX - the table is one
/// descriptor with the lanes inside it, not a per-frame array of descriptors (see the item fetch below).
static const uint visibility_item_capacity = 4096u;

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


MeshGeometryLanes visibility_geometry(VisibilityDrawItem item) {
    MeshGeometryLanes geometry;
    geometry.vertex_address_low = item.vertex_address_low;
    geometry.vertex_address_high = item.vertex_address_high;
    geometry.index_address_low = item.index_address_low;
    geometry.index_address_high = item.index_address_high;
    geometry.first_index = 0u;
    geometry.index_count = 0u;
    geometry.base_vertex = 0;
    geometry.index_width = item.index_width;
    return geometry;
}
MeshVertex visibility_vertex(MeshGeometryLanes geometry, MeshletRecord record, uint triangle, uint corner) {
    const uint raw_index = mesh_index_at(geometry, record.first_index + triangle * mesh_indices_per_triangle + corner);
    return mesh_vertex_at(geometry, uint(int(raw_index) + record.base_vertex));
}
float4 visibility_world(float3 position, VR_MAT4 model) { return mul(float4(position, 1.0), model); }
float4 visibility_clip(float4 world, VR_MAT4 view_proj) { return mul(world, view_proj); }

#endif
