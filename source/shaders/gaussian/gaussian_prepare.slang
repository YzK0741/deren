#include "gaussian/gaussian_projection.slang"

struct GaussianPreparation
{
    GaussianParameters raster; // 0..111
    float4 forward; // 112
    uint64_t ranges_address; // 128
    uint64_t keys_address; // 136
    uint64_t values_address; // 144
    uint64_t status_address; // 152
    uint64_t draw_address; // 160
    uint candidate_count; // 168
    uint range_count; // 172
};
struct GaussianPreparationPush
{
    uint64_t parameters_address;
    uint frame_slot;
    uint image_index;
    uint spare_lane;
};
[[vk::push_constant]] ConstantBuffer<GaussianPreparationPush> pc;

uint2 gaussian_candidate(GaussianPreparation config, uint index)
{
    uint4* ranges = (uint4*)config.ranges_address;
    uint first = 0u;
    uint last = config.range_count;
    while (first < last)
    {
        uint middle = first + (last - first) / 2u;
        if (index >= ranges[middle].y)
            first = middle + 1u;
        else
            last = middle;
    }
    uint4 range = ranges[first];
    return uint2(range.z, index - range.x);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void reset_main()
{
    GaussianPreparation config = *(GaussianPreparation*)pc.parameters_address;
    uint* status = (uint*)config.status_address;
    uint* draw = (uint*)config.draw_address;
    for (uint i = 0u; i < 4u; ++i)
    {
        status[i] = 0u;
        draw[i] = 0u;
    }
}

[shader("compute")]
[numthreads(128, 1, 1)]
void prepare_main(uint3 id : SV_DispatchThreadID)
{
    GaussianPreparation config = *(GaussianPreparation*)pc.parameters_address;
    if (id.x >= config.candidate_count) return;
    uint* keys = (uint*)config.keys_address;
    uint* values = (uint*)config.values_address;
    uint* status = (uint*)config.status_address;
    uint2 candidate = gaussian_candidate(config, id.x);
    uint visibility;
    float3 world;
    GaussianProjection cached;
    GaussianVertex projected = gaussian_project_with_cache(config.raster, 0u, candidate.x, candidate.y, true, visibility, world, cached);
    float depth = dot(world - config.raster.camera_position.xyz, config.forward.xyz);
    if (!isfinite(depth)) visibility = 3u;
    uint key = 0xffffffffu;
    if (visibility == 0u)
    {
        uint* instance = (uint*)(config.raster.instances_address + uint64_t(candidate.x) * gaussian_instance_stride);
        const uint64_t address = gaussian_address(instance[37], instance[38]) + uint64_t(candidate.y) * 48u;
        *(GaussianProjection*)address = cached;
        // Float flip of negative forward depth gives far-to-near ascending uint32 keys.
        float negative_depth = depth == 0.0 ? 0.0 : -depth;
        uint bits = asuint(negative_depth);
        key = (bits & 0x80000000u) != 0u ? ~bits : bits ^ 0x80000000u;
    }
    uint ignored;
    InterlockedAdd(status[visibility], 1u, ignored);
    keys[id.x] = key;
    values[id.x] = id.x;
}

[shader("compute")]
[numthreads(128, 1, 1)]
void finalize_main(uint3 id : SV_DispatchThreadID)
{
    GaussianPreparation config = *(GaussianPreparation*)pc.parameters_address;
    uint* status = (uint*)config.status_address;
    if (id.x == 0u)
    {
        uint* draw = (uint*)config.draw_address;
        draw[0] = 6u;
        draw[1] = status[3] == 0u ? status[0] : 0u;
        draw[2] = 0u;
        draw[3] = 0u;
    }
    if (status[3] != 0u || id.x >= status[0]) return;
    uint* values = (uint*)config.values_address;
    uint2 candidate = gaussian_candidate(config, values[id.x]);
    uint2* references = (uint2*)config.raster.references_address;
    references[id.x] = candidate;
}
