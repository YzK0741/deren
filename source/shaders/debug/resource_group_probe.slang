struct Parameters {
    uint data;
    uint output;
    uint image;
    uint storage;
    uint sampler;
    uint texture_group;
    uint uniform_data;
    uint output_index;
};
[[vk::push_constant]] ConstantBuffer<Parameters> push;
#include "common/texture_group.slang"
struct UniformData { uint4 value; };
[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    DescriptorHandle<StructuredBuffer<uint>> data = (DescriptorHandle<StructuredBuffer<uint>>)uint2(push.data, 0);
    DescriptorHandle<RWStructuredBuffer<uint>> output = (DescriptorHandle<RWStructuredBuffer<uint>>)uint2(push.output, 0);
    DescriptorHandle<Texture2D<float4>> image = (DescriptorHandle<Texture2D<float4>>)uint2(push.image, 0);
    DescriptorHandle<RWTexture2D<float4>> storage = (DescriptorHandle<RWTexture2D<float4>>)uint2(push.storage, 0);
    DescriptorHandle<SamplerState> sampler = (DescriptorHandle<SamplerState>)uint2(push.sampler, 0);
    DescriptorHandle<ConstantBuffer<UniformData>> uniform_data = (DescriptorHandle<ConstantBuffer<UniformData>>)uint2(push.uniform_data, 0);
    TextureGroup group = GetLoadedTextureGroup();
    float4 color = image.SampleLevel(sampler, float2(0.5), 0);
    storage[uint2(0,0)] = color;
    uint i = push.output_index * 5;
    output[i] = data[0];
    output[i+1] = uint(round(color.r*255));
    output[i+2] = group.present_mask;
    output[i+3] = uint(round(LoadTexture2D(group, 0).SampleLevel(sampler, float2(0.5), 0).r*255));
    output[i+4] = uniform_data.value.x;
}
