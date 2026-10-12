// Backend sampling interface; the caller's push block provides texture_group.
#ifndef DEREN_TEXTURE_GROUP_SLANG
#define DEREN_TEXTURE_GROUP_SLANG
static const uint texture_group_table_slot = 17664u;
static const uint texture_group_texture_base = 17665u;
struct TextureGroup {
    uint indices[16];
    uint present_mask;
    uint padding[3];
};
typedef DescriptorHandle<Texture2D<float4>> TextureGroupTexture2D;
TextureGroup GetLoadedTextureGroup() {
    return ((DescriptorHandle<StructuredBuffer<TextureGroup>>)uint2(texture_group_table_slot, 0))[push.texture_group];
}
bool HasTexture(TextureGroup group, uint slot) {
    return slot < 16 && (group.present_mask & (1u << slot)) != 0;
}
TextureGroupTexture2D LoadTexture2D(TextureGroup group, uint slot) {
    uint index = slot < 16 ? group.indices[slot] : 0;
    return (TextureGroupTexture2D)uint2(NonUniformResourceIndex(texture_group_texture_base + index), 0);
}
DescriptorHandle<SamplerState> GetTextureGroupSampler() {
    return (DescriptorHandle<SamplerState>)uint2(2048u, 0);
}
#endif
