struct Parameters { uint data; };
[[vk::push_constant]] ConstantBuffer<Parameters> push;
[shader("vertex")]
float4 vertex_main(uint id : SV_VertexID) : SV_Position {
    return float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0.0, 1.0);
}
[shader("fragment")]
float4 fragment_main() : SV_Target0 {
    DescriptorHandle<StructuredBuffer<uint>> data = (DescriptorHandle<StructuredBuffer<uint>>)uint2(push.data,0);
    return float4(float(data[0])/255.0,0.0,0.0,1.0);
}

[shader("vertex")]
float4 vertex_resource_main(uint id : SV_VertexID) : SV_Position {
    DescriptorHandle<StructuredBuffer<uint>> data = (DescriptorHandle<StructuredBuffer<uint>>)uint2(push.data,0);
    return float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, float(data[0])/255.0, 1.0);
}
