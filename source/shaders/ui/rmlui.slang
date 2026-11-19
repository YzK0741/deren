struct PushConstants {
    uint64_t vertex_address;
    uint texture_index;
    uint sampler_index;
    float offset_x;
    float offset_y;
    float width;
    float height;
    uint srgb_output;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> push;

struct UiVertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

[shader("vertex")]
UiVertex vertex_main(uint vertex_id : SV_VertexID) {
    uint* data = (uint*)push.vertex_address + vertex_id * 5;
    float2 pixel = float2(asfloat(data[0]), asfloat(data[1])) + float2(push.offset_x, push.offset_y);
    UiVertex result;
    result.position = float4(pixel / float2(push.width, push.height) * 2.0 - 1.0, 0.0, 1.0);
    result.uv = float2(asfloat(data[2]), asfloat(data[3]));
    result.color = float4((data[4] >> uint4(0, 8, 16, 24)) & 255u) / 255.0;
    return result;
}

[shader("fragment")]
float4 fragment_main(UiVertex vertex) : SV_Target {
    DescriptorHandle<Texture2D<float4>> texture = (DescriptorHandle<Texture2D<float4>>)uint2(push.texture_index, 0);
    DescriptorHandle<SamplerState> sampler = (DescriptorHandle<SamplerState>)uint2(push.sampler_index, 0);
    float4 sampled = texture.SampleLevel(sampler, vertex.uv, 0);
    float4 premultiplied = sampled * vertex.color;
    // The RHI's alpha mode expects straight RGB.
    float3 color = premultiplied.rgb / max(premultiplied.a, 1e-6);
    if (push.srgb_output != 0) {
        color = select(color <= 0.04045, color / 12.92, pow((color + 0.055) / 1.055, 2.4));
    }
    return float4(color, premultiplied.a);
}
