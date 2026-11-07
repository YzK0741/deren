// AMD FidelityFX Reflection Denoiser - stage 1 (reproject).
// The stage body and its MIT notice live in third_party/ffx_denoiser/.
#define REFLECTION_DENOISE_REPROJECT 1
#include "lighting/reflection_denoise_common.slang"

#include "third_party/ffx_denoiser/ffx_denoiser_reflections_reproject.h"

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID, uint group_index : SV_GroupIndex)
{
    uint2 remapped_group_thread_id = FFX_DNSR_Reflections_RemapLane8x8(group_index);
    uint2 dispatch_group_id = dispatch_id.xy / 8;
    uint2 remapped_dispatch_thread_id = dispatch_group_id * 8 + remapped_group_thread_id;

    // AMD's stage only writes these three images inside its glossy branch. Every other pixel must still
    // have a defined reprojection for the prefilter and resolve that read the whole screen: seed a black
    // reprojection with unit variance and one sample, the same values the stage writes when it rejects a
    // reprojection.
    int2 seed_pixel = int2(remapped_dispatch_thread_id);
    FFX_DNSR_Reflections_StoreRadianceReprojected(seed_pixel, float3(0.0, 0.0, 0.0));
    FFX_DNSR_Reflections_StoreVariance(seed_pixel, 1.0);
    FFX_DNSR_Reflections_StoreNumSamples(seed_pixel, 1.0);

    FFX_DNSR_Reflections_Reproject(
        int2(remapped_dispatch_thread_id),
        int2(remapped_group_thread_id),
        reflection_screen_size(),
        push.temporal_stability_factor,
        int(push.max_samples));
}
