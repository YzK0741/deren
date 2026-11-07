// AMD FidelityFX Reflection Denoiser - stage 3 (temporal resolve).
// The stage body and its MIT notice live in third_party/ffx_denoiser/.
#include "lighting/reflection_denoise_common.slang"

#include "third_party/ffx_denoiser/ffx_denoiser_reflections_resolve_temporal.h"

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID, uint group_index : SV_GroupIndex)
{
    uint2 remapped_group_thread_id = FFX_DNSR_Reflections_RemapLane8x8(group_index);
    uint2 dispatch_group_id = dispatch_id.xy / 8;
    uint2 remapped_dispatch_thread_id = dispatch_group_id * 8 + remapped_group_thread_id;

    FFX_DNSR_Reflections_ResolveTemporal(
        int2(remapped_dispatch_thread_id),
        int2(remapped_group_thread_id),
        reflection_screen_size(),
        1.0 / float2(reflection_screen_size()),
        1.0);
}
