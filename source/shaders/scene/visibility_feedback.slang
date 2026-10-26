#include "common/heap_access.slang"
#include "common/software_candidate.slang"
struct FeedbackPush { uint frame_slot; uint image_index; uint hybrid; uint output_slot; uint material_count; uint item_count; };
[[vk::push_constant]] ConstantBuffer<FeedbackPush> push;
#define heap_frame_slot push.frame_slot
#define heap_image_index push.image_index
[numthreads(8, 8, 1)]
void main(uint3 thread : SV_DispatchThreadID) {
    Texture2D<uint> ids = heap_at<Texture2D<uint>>(heap_image_slot(heap_slots_visibility));
    uint width, height;
    ids.GetDimensions(width, height);
    if (thread.x >= width || thread.y >= height) return;
    uint2 pixel = thread.xy;
    uint packed = ids.Load(int3(pixel, 0));
    if (push.hybrid != 0) {
        float depth = heap_texel_lod0(heap_at<Texture2D<float4>>(heap_image_slot(heap_slots_gbuffer_depth)), heap_sampler_gbuffer,
                                      (float2(pixel) + 0.5) / float2(width, height)).r;
        uint sw_id = heap_at<Texture2D<uint>>(heap_image_slot(heap_slots_software_id)).Load(int3(pixel, 0));
        uint sw_depth = heap_at<Texture2D<uint>>(heap_image_slot(heap_slots_software_depth)).Load(int3(pixel, 0));
        if (choose_visibility_candidate(packed, depth, sw_id, sw_depth)) packed = sw_id;
    }
    if ((packed & 0x80000000u) == 0) return;
    uint item = (packed >> 19u) & 4095u;
    RWByteAddressBuffer output = heap_at<RWByteAddressBuffer>(push.output_slot);
    uint ignored;
    if (item >= push.item_count) {
        output.InterlockedOr(2048u, 1u, ignored);
        return;
    }
    uint material = heap_at<ByteAddressBuffer>(heap_slots_visibility_items).Load((push.frame_slot * 4096u + item) * 112u + 36u);
    if (material >= push.material_count || material >= 16384u) output.InterlockedOr(2048u, 1u, ignored);
    else output.InterlockedOr((material / 32u) * 4u, 1u << (material % 32u), ignored);
}
