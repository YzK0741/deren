// Copies the resolve's current denoiser outputs into the history images the next frame reads.
//
// WHY THIS IS A SEPARATE PASS: reprojection and the temporal resolve both READ last frame's radiance, moments,
// surface and depth while WRITING this frame's. Keeping both in one image would be a read/write data race across
// threads (the reprojected uv can land on any pixel), so the stages write `*_current`/`denoised` and this pass
// moves the results into `*_history` after every reader has finished. It also copies the 1/8-resolution average.
#include "lighting/reflection_denoise_common.slang"

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID)
{
    uint2 size = reflection_screen_size();
    if (dispatch_id.x >= size.x || dispatch_id.y >= size.y)
    {
        return;
    }
    int2 pixel = int2(dispatch_id.xy);

    Texture2D<float4> denoised = heap_at<Texture2D<float4>>(heap_slots_rt_reflection_denoised + heap_frame_slot);
    RWTexture2D<float4> denoised_history = heap_at<RWTexture2D<float4>>(heap_slots_rt_reflection_denoised_history_storage + heap_frame_slot);
    denoised_history[pixel] = denoised.Load(int3(pixel, 0));

    Texture2D<float4> moments = heap_at<Texture2D<float4>>(heap_slots_rt_reflection_moments + heap_frame_slot);
    RWTexture2D<float2> moments_history = heap_at<RWTexture2D<float2>>(heap_slots_rt_reflection_moments_history_storage + heap_frame_slot);
    moments_history[pixel] = moments.Load(int3(pixel, 0)).xy;

    Texture2D<float4> surface = heap_at<Texture2D<float4>>(heap_slots_rt_reflection_surface_current + heap_frame_slot);
    RWTexture2D<float4> surface_history = heap_at<RWTexture2D<float4>>(heap_slots_rt_reflection_surface_history_storage + heap_frame_slot);
    surface_history[pixel] = surface.Load(int3(pixel, 0));

    Texture2D<uint> depth = heap_at<Texture2D<uint>>(heap_slots_rt_reflection_depth_current + heap_frame_slot);
    RWTexture2D<uint> depth_history = heap_at<RWTexture2D<uint>>(heap_slots_rt_reflection_depth_history_storage + heap_frame_slot);
    depth_history[pixel] = depth.Load(int3(pixel, 0));

    // One lane per 8x8 group copies the 1/8-resolution average. The aligned pixel always exists for every
    // average texel, including the partial group at the right/bottom edge.
    if ((dispatch_id.x & 7u) == 0u && (dispatch_id.y & 7u) == 0u)
    {
        int2 average_pixel = pixel / 8;
        Texture2D<float4> average = heap_at<Texture2D<float4>>(heap_slots_rt_reflection_average + heap_frame_slot);
        RWTexture2D<float4> average_history = heap_at<RWTexture2D<float4>>(heap_slots_rt_reflection_average_history_storage + heap_frame_slot);
        average_history[average_pixel] = average.Load(int3(average_pixel, 0));
    }
}
