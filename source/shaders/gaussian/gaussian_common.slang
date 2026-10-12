#ifndef GAUSSIAN_COMMON_SLANG
#define GAUSSIAN_COMMON_SLANG

// Matches geometry_record / sh_record / instance_record / draw_reference.
static const uint gaussian_geometry_stride = 48u;
static const uint gaussian_sh_stride = 192u;
static const uint gaussian_instance_stride = 160u;
static const uint gaussian_reference_stride = 8u;
static const float gaussian_regularization = 0.3;
static const float gaussian_radius_squared = 9.0;
static const float gaussian_alpha_cutoff = 1.0 / 255.0;

uint64_t gaussian_address(uint low, uint high)
{
    return uint64_t(low) | (uint64_t(high) << 32u);
}
float4 gaussian_float4(uint64_t address, uint word)
{
    uint* p = (uint*)address;
    return asfloat(uint4(p[word], p[word + 1u], p[word + 2u], p[word + 3u]));
}
float gaussian_covariance(float3 a, float3 b, float3 covariance_x, float3 covariance_yz)
{
    return dot(a, float3(
        covariance_x.x * b.x + covariance_x.y * b.y + covariance_x.z * b.z,
        covariance_x.y * b.x + covariance_yz.x * b.y + covariance_yz.y * b.z,
        covariance_x.z * b.x + covariance_yz.y * b.y + covariance_yz.z * b.z));
}
float3 gaussian_sh(uint64_t address, uint degree, float3 direction)
{
    const float x = direction.x, y = direction.y, z = direction.z;
    const float xx = x * x, yy = y * y, zz = z * z;
    const float basis[16] = {
        0.28209479177387814,
        -0.4886025119029199 * y, 0.4886025119029199 * z, -0.4886025119029199 * x,
        1.0925484305920792 * x * y, -1.0925484305920792 * y * z,
        0.31539156525252005 * (2.0 * zz - xx - yy),
        -1.0925484305920792 * x * z, 0.5462742152960396 * (xx - yy),
        -0.5900435899266435 * y * (3.0 * xx - yy), 2.890611442640554 * x * y * z,
        -0.4570457994644658 * y * (4.0 * zz - xx - yy),
        0.3731763325901154 * z * (2.0 * zz - 3.0 * xx - 3.0 * yy),
        -0.4570457994644658 * x * (4.0 * zz - xx - yy),
        1.445305721320277 * z * (xx - yy), -0.5900435899266435 * x * (xx - 3.0 * yy)};
    uint* p = (uint*)address;
    float3 color = float3(0.5);
    const uint used = (degree + 1u) * (degree + 1u);
    for (uint i = 0u; i < used; ++i)
    {
        color += basis[i] * asfloat(uint3(p[i * 3u], p[i * 3u + 1u], p[i * 3u + 2u]));
    }
    return max(color, float3(0.0));
}
#endif
