struct PushConstants {
    uint64_t destination;
    uint texture_group;
    uint output_index;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> push;
#include "texture_group.slang"
[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    TextureGroup group = GetLoadedTextureGroup();
    float4 a = LoadTexture2D(group, 0).SampleLevel(GetTextureGroupSampler(), float2(0.5), 0);
    float4 b = LoadTexture2D(group, 15).SampleLevel(GetTextureGroupSampler(), float2(0.5), 0);
    uint* output = (uint*)push.destination;
    uint offset = push.output_index * 3;
    output[offset] = group.present_mask;
    output[offset + 1] = uint(round(a.r * 255)) | (uint(round(a.g * 255)) << 8) | (uint(round(a.b * 255)) << 16);
    output[offset + 2] = uint(round(b.r * 255)) | (uint(round(b.g * 255)) << 8) | (uint(round(b.b * 255)) << 16);
}
