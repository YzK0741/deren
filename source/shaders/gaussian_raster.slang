#include "gaussian_common.slang"

// Explicit columns match GLM storage without depending on Slang matrix lowering.
struct GaussianPush
{
    float4 vp0; // 0
    float4 vp1; // 16
    float4 vp2; // 32
    float4 vp3; // 48
    float4 camera_position; // 64
    uint64_t references_address; // 80
    uint64_t instances_address; // 88
    float viewport_width; // 96
    float viewport_height; // 100
    uint visible_count; // 104
    uint flags; // 108
};
[[vk::push_constant]] ConstantBuffer<GaussianPush> pc;

struct GaussianVertex
{
    float4 position : SV_Position;
    [[vk::location(0)]] float2 coordinate : GAUSSIAN_COORD;
    [[vk::location(1)]] nointerpolation float4 color_opacity : GAUSSIAN_COLOR;
};

[shader("vertex")]
GaussianVertex vertex_main(uint vertex_index : SV_VertexID, uint reference_index : SV_InstanceID)
{
    GaussianVertex output;
    output.position = float4(0.0, 0.0, 2.0, 1.0);
    output.coordinate = float2(0.0);
    output.color_opacity = float4(0.0);
    if (reference_index >= pc.visible_count || vertex_index >= 6u)
    {
        return output;
    }
    uint* reference = (uint*)(pc.references_address + uint64_t(reference_index) * gaussian_reference_stride);
    const uint instance_index = reference[0];
    const uint local_index = reference[1];
    const uint64_t instance_address = pc.instances_address + uint64_t(instance_index) * gaussian_instance_stride;
    uint* instance = (uint*)instance_address;
    const uint64_t geometry_address = gaussian_address(instance[0], instance[1]) + uint64_t(local_index) * gaussian_geometry_stride;
    const uint64_t sh_address = gaussian_address(instance[2], instance[3]) + uint64_t(local_index) * gaussian_sh_stride;
    const uint degree = instance[36];
    if (degree > 3u)
    {
        return output;
    }
    const float4 m0 = gaussian_float4(instance_address, 4u);
    const float4 m1 = gaussian_float4(instance_address, 8u);
    const float4 m2 = gaussian_float4(instance_address, 12u);
    const float4 m3 = gaussian_float4(instance_address, 16u);
    const float4 center_opacity = gaussian_float4(geometry_address, 0u);
    const float3 covariance_x = gaussian_float4(geometry_address, 4u).xyz;
    const float3 covariance_yz = gaussian_float4(geometry_address, 8u).xyz;
    const float4 world = m0 * center_opacity.x + m1 * center_opacity.y + m2 * center_opacity.z + m3;
    const float4 clip = pc.vp0 * world.x + pc.vp1 * world.y + pc.vp2 * world.z + pc.vp3 * world.w;
    if (!all(isfinite(clip)) || clip.w <= 0.0)
    {
        return output;
    }
    const float4 row0 = float4(pc.vp0.x, pc.vp1.x, pc.vp2.x, pc.vp3.x);
    const float4 row1 = float4(pc.vp0.y, pc.vp1.y, pc.vp2.y, pc.vp3.y);
    const float4 row2 = float4(pc.vp0.z, pc.vp1.z, pc.vp2.z, pc.vp3.z);
    const float4 row3 = float4(pc.vp0.w, pc.vp1.w, pc.vp2.w, pc.vp3.w);
    const float4 planes[2] = {row2, row3 - row2};
    for (uint i = 0u; i < 2u; ++i)
    {
        const float3 n = float3(dot(planes[i].xyz, m0.xyz), dot(planes[i].xyz, m1.xyz), dot(planes[i].xyz, m2.xyz));
        const float radius = 3.0 * sqrt(max(0.0, gaussian_covariance(n, n, covariance_x, covariance_yz)));
        const float distance = dot(planes[i], world);
        if (!isfinite(radius) || distance + radius < 0.0 || (i == 0u && distance - radius <= 0.0))
        {
            return output;
        }
    }
    const float3 ndc = clip.xyz / clip.w;
    const float3 jx_world = (0.5 * pc.viewport_width / clip.w) * (row0.xyz - ndc.x * row3.xyz);
    const float3 jy_world = (0.5 * pc.viewport_height / clip.w) * (row1.xyz - ndc.y * row3.xyz);
    const float3 jx = float3(dot(jx_world, m0.xyz), dot(jx_world, m1.xyz), dot(jx_world, m2.xyz));
    const float3 jy = float3(dot(jy_world, m0.xyz), dot(jy_world, m1.xyz), dot(jy_world, m2.xyz));
    const float xx = gaussian_covariance(jx, jx, covariance_x, covariance_yz) + gaussian_regularization;
    const float xy = gaussian_covariance(jx, jy, covariance_x, covariance_yz);
    const float yy = gaussian_covariance(jy, jy, covariance_x, covariance_yz) + gaussian_regularization;
    if (!all(isfinite(float3(xx, xy, yy))) || xx <= 0.0 || yy <= 0.0 || xx * yy - xy * xy <= 0.0)
    {
        return output;
    }
    const float root_x = sqrt(xx);
    const float cross_y = xy / root_x;
    const float2 basis_x = float2(root_x, cross_y);
    const float2 basis_y = float2(0.0, sqrt(max(gaussian_regularization, yy - cross_y * cross_y)));
    const float2 center_pixel = (ndc.xy + 1.0) * 0.5 * float2(pc.viewport_width, pc.viewport_height);
    const float2 radius = 3.0 * float2(root_x, length(float2(cross_y, basis_y.y)));
    if (center_pixel.x + radius.x < 0.0 || center_pixel.x - radius.x > pc.viewport_width ||
        center_pixel.y + radius.y < 0.0 || center_pixel.y - radius.y > pc.viewport_height)
    {
        return output;
    }
    const float2 coordinates[6] = {
        float2(-3.0, -3.0), float2(3.0, -3.0), float2(3.0, 3.0),
        float2(-3.0, -3.0), float2(3.0, 3.0), float2(-3.0, 3.0)};
    const float2 coordinate = coordinates[vertex_index];
    const float2 pixel_offset = basis_x * coordinate.x + basis_y * coordinate.y;
    const float4 im0 = gaussian_float4(instance_address, 20u);
    const float4 im1 = gaussian_float4(instance_address, 24u);
    const float4 im2 = gaussian_float4(instance_address, 28u);
    const float4 im3 = gaussian_float4(instance_address, 32u);
    const float3 local_camera = (im0 * pc.camera_position.x + im1 * pc.camera_position.y + im2 * pc.camera_position.z + im3).xyz;
    const float3 direction = normalize(center_opacity.xyz - local_camera);
    const float3 color = gaussian_sh(sh_address, degree, direction);
    const float4 position = float4(ndc.xy + 2.0 * pixel_offset / float2(pc.viewport_width, pc.viewport_height), min(1.0, ndc.z), 1.0);
    if (!all(isfinite(position)) || !all(isfinite(color)))
    {
        return output;
    }
    output.position = position;
    output.coordinate = coordinate;
    output.color_opacity = float4(color, center_opacity.w);
    return output;
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
