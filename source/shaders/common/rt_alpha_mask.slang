// shaders/common/rt_alpha_mask.slang - the alphaMode MASK cut shared by every traced any-hit stage.
//
// WHY THIS IS ONE FILE: the shadow and the reflection tracers both meet the same triangle and must answer the
// same question the same way. A second copy is a second predicate to keep in step with `shaders/surface.slang`
// and `shaders/shadow.slang`; a drift there is silent, because the frame still looks like a shadow or a
// reflection, only with the wrong cut. The caller supplies `heap_access.slang` first (this file reads the heap
// through its macros) and then calls `rt_alpha_mask_cut` from its any-hit entry.
//
// WHAT THE CUT DOES, in one line: it interpolates the material's base-colour ALPHA at the hit and refuses the
// intersection when that alpha is below the material's cutoff, with the SAME predicate the raster paths use
// (`base_color_factor.a * albedo.a < alpha_cutoff`). THREE DETAILS ARE FORCED BY WHERE IT RUNS:
//  - the fetch is an EXPLICIT-LOD fetch (`heap_texel_lod0`): a hit shader has no derivatives, so an
//    implicit-LOD sample would make the module invalid (docs/slang_migration.md TRAP 4) - and LOD 0 is what
//    the raster test samples anyway;
//  - the UV is NOT an interpolated varying, because there is none. It is FETCHED: the primitive index names
//    the triangle, its three vertex indices come from the geometry's index buffer and their UVs from the
//    vertex buffer, both through the DEVICE ADDRESSES in the instance record. That is GLSL's
//    GL_EXT_buffer_reference, and Slang spells the same thing as a C-style pointer cast from the 64-bit
//    address - `(const float*)address` - which emits OpConvertUToPtr into a PhysicalStorageBuffer pointer
//    (verified in a probe before this port: OpCapability PhysicalStorageBufferAddresses + OpConvertUToPtr).
//  - the barycentrics are computed by hand in OBJECT space: a triangle hit has no `gl_HitAttributeEXT` worth
//    reading without an intersection shader, and this pipeline has none - triangle geometry is intersected by
//    the fixed-function unit, and VkAccelerationStructureGeometryTrianglesDataKHR has no attribute field to
//    declare one in.
//
// THE TWO "OPAQUE" DECLARATIONS MUST STAY LIFTED wherever this cut has to run: no RAY_FLAG_OPAQUE in the
// raygen and no VK_GEOMETRY_OPAQUE_BIT_KHR on the BLAS, because both DECLARE "no any-hit shader may be
// invoked". Measured on an RTX 4060 with the closest-hit stage silenced, neither flag actually suppressed the
// stage on that driver, but a conforming driver would skip it and the cut would silently do nothing - the
// worst kind of failure, because the frame would still look like a shadow or a reflection.

#ifndef DEREN_RT_ALPHA_MASK_SLANG
#define DEREN_RT_ALPHA_MASK_SLANG

// The instance table: one record per instance of the top level structure, filled by EVERY structure build
// (acceleration_structure::instance_record, whose 96-byte std430 layout this mirrors). The two addresses are
// held as 32-bit HALVES, the shape this project carries an address in.
struct RtMaskInstance
{
    uint2 vertex_address; // the vertex buffer's device address
    uint2 index_address;  // the index buffer's, or zero when the geometry is not indexed (the bake's output)
    VR_MAT4 model;        // object -> world (not read here; the traversal's transform is)
    uint vertex_stride;   // bytes per vertex of the interleaved layout
    uint index_type;      // VkIndexType: 0 = uint16, 1 = uint32
    uint material_index;  // into the material table above
    uint primitive_index;
};

/// @brief material_record::flags bit4 - "alphaMode MASK", the same bit shaders/surface.slang tests
#define RT_MASK_ALPHA_FLAG 16u
/// @brief the uv's offset in the interleaved vertex, in floats: position(0-2) normal(3-5) uv(6-7)
#define RT_MASK_UV_FLOAT_OFFSET 6u

/// @brief the 64-bit device address held in two lanes (little-endian, low half first)
uint64_t rt_mask_lane_address(uint2 halves)
{
    return (uint64_t(halves.y) << 32u) | uint64_t(halves.x);
}

/// @brief true when the any-hit stage must refuse this intersection (a MASK texel below its cutoff)
/// @param instance the hit instance record from the TLAS instance table
/// @param frame_slot the frame slot selecting this frame's texture bank
bool rt_alpha_mask_cut(RtMaskInstance instance, uint frame_slot)
{
    const Material material = material_at(heap_slots_materials, instance.material_index);
    if ((material.flags & RT_MASK_ALPHA_FLAG) == 0u)
    {
        // OPAQUE or BLEND: the raster shadow/reflection paths do not cut those either, so the hit stands.
        return false;
    }

    // The three vertices of the triangle the ray hit, by the geometry's addressing: an index (or, for a
    // non-indexed source - which is what the mask bake's expanded geometry is - the vertex's position in
    // the list), times the stride, off the base.
    float* vertices = (float*)rt_mask_lane_address(instance.vertex_address);
    const uint stride = instance.vertex_stride / 4u;
    const bool indexed = (instance.index_address.x != 0u) || (instance.index_address.y != 0u);
    float3 positions[3];
    float2 uvs[3];
    for (uint k = 0u; k < 3u; ++k)
    {
        const uint i = uint(PrimitiveIndex()) * 3u + k;
        uint vertex = i;
        if (indexed)
        {
            uint* indices = (uint*)rt_mask_lane_address(instance.index_address);
            if (instance.index_type == 0u)
            {
                const uint word = indices[i >> 1u];
                vertex = (i & 1u) == 0u ? (word & 0xFFFFu) : (word >> 16u);
            }
            else
            {
                vertex = indices[i];
            }
        }
        const uint at = vertex * stride;
        positions[k] = float3(vertices[at + 0u], vertices[at + 1u], vertices[at + 2u]);
        uvs[k] = float2(vertices[at + RT_MASK_UV_FLOAT_OFFSET + 0u], vertices[at + RT_MASK_UV_FLOAT_OFFSET + 1u]);
    }

    // The hit's barycentric coordinates, computed in OBJECT space because that is the space the vertices are
    // in: the traversal transforms the ray there. WorldToObject3x4() is the traversal's inverse transform - an
    // affine transform stored as three rows of four - so it multiplies a float4 and yields a float3, which is
    // also what makes the direction's translation drop out (w = 0) and leaves the parametric hit distance
    // unchanged. Moller-Trumbore's u and v are the weights of vertices 1 and 2, which is what the interpolation
    // below uses.
    const float3 origin_object = mul(WorldToObject3x4(), float4(WorldRayOrigin(), 1.0));
    const float3 direction_object = mul(WorldToObject3x4(), float4(WorldRayDirection(), 0.0));
    const float3 hit_point = origin_object + direction_object * RayTCurrent();
    const float3 edge_1 = positions[1] - positions[0];
    const float3 edge_2 = positions[2] - positions[0];
    const float3 pv = cross(direction_object, edge_2);
    const float det = dot(edge_1, pv);
    if (abs(det) < 1e-12)
    {
        // A degenerate triangle has no UV frame to interpolate in. Committing the hit is the conservative
        // answer: an unrefused hit keeps geometry, while a wrong refusal deletes it.
        return false;
    }
    const float inv_det = 1.0 / det;
    const float3 tv = hit_point - positions[0];
    const float u = dot(tv, pv) * inv_det;
    const float v = dot(cross(tv, edge_1), direction_object) * inv_det;
    const float2 uv = uvs[0] * (1.0 - u - v) + uvs[1] * u + uvs[2] * v;

    // THE CUT: the mirror of shaders/shadow.slang's discard and of shaders/surface.slang's MASK test.
    const float alpha = material.base_color_factor.a * heap_texel_lod0(heap_at<Texture2D<float4>>(heap_slots_textures + frame_slot * heap_texture_bank_stride + material.tex_indices.x), heap_sampler_texture, uv).a;
    return alpha < material.alpha_cutoff;
}

#endif // DEREN_RT_ALPHA_MASK_SLANG
