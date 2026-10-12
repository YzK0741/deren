#include "gaussian/gaussian_projection.slang"

// The host pushes 112 camera bytes, then the heap appends three index lanes.
struct GaussianPush
{
    GaussianParameters camera;
    uint frame_slot; // 112
    uint image_index; // 116
    uint spare_lane; // 120
};
[[vk::push_constant]] ConstantBuffer<GaussianPush> pc;

[shader("vertex")]
GaussianVertex vertex_main(uint vertex_index : SV_VertexID, uint reference_index : SV_InstanceID)
{
    GaussianVertex output;
    output.position = float4(0.0, 0.0, 2.0, 1.0);
    output.coordinate = float2(0.0);
    output.color_opacity = float4(0.0);
    if (reference_index >= pc.camera.visible_count || vertex_index >= 6u)
        return output;
    uint* reference = (uint*)(pc.camera.references_address + uint64_t(reference_index) * gaussian_reference_stride);
    if ((pc.camera.flags & 1u) != 0u)
        return gaussian_cached_vertex(pc.camera, vertex_index, reference[0], reference[1]);
    uint visibility;
    float3 center;
    return gaussian_project(pc.camera, vertex_index, reference[0], reference[1], false, visibility, center);
}

[shader("fragment")]
float4 fragment_main(GaussianVertex input) : SV_Target
{
    const float radius_squared = dot(input.coordinate, input.coordinate);
    const float alpha = min(0.99, input.color_opacity.w * exp(-0.5 * radius_squared));
    if (radius_squared > gaussian_radius_squared || alpha < gaussian_alpha_cutoff)
    {
        discard;
    }
    // Far to near uses ordinary, unpremultiplied alpha blending.
    return float4(input.color_opacity.xyz, alpha);
}
