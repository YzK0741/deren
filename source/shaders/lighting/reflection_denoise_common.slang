// The Deren-side contract for AMD's FidelityFX Reflection Denoiser stages.
//
// The three stage headers under `third_party/ffx_denoiser/` are ported from AMD's
// `ffx-reflection-dnsr` sample and keep AMD's MIT notice. They call a small set of
// load/sample/store callbacks and the view-space helpers below; this file supplies
// those callbacks from Deren's frame heap. The wrapper stages only choose the 8x8
// lane remapping and dispatch one thread per pixel.
//
// DEREN CONVENTIONS THAT DIFFER FROM THE SAMPLE:
//   * the G-buffer normal is already a world-space normal (no `2*x-1` decode);
//   * the velocity target already stores UV-space `current_uv - previous_uv`, so it
//     is used directly instead of the sample's `* float2(0.5, -0.5)`;
//   * depth is raw D32 in [0,1] and the projection is Vulkan-style (no D3D Y flip);
//   * the history depth image is R32_UINT (asuint(depth)), so its bilinear sample
//     is reconstructed by hand - integer images have no filtered sampler.
#include "common/heap_access.slang"

struct CameraUBO
{
    VR_MAT4 view;
    VR_MAT4 proj;
    float4 camera_pos;
    VR_MAT4 view_proj_unjittered;
    VR_MAT4 prev_view_proj;
};

struct ReflectionDenoisePush
{
    float temporal_stability_factor;
    uint max_samples;
    uint history_valid;
    uint screen_width;
    uint screen_height;
    // The roughness below which the AMD stages filter. The one-sample reflection pass keeps AMD's 0.2;
    // `rt_reflection_samples > 1` raises it to the tracer's 0.55 without changing the one-sample path.
    float max_roughness;
    uint frame_slot;
    uint image_index;
};

[[vk::push_constant]] ConstantBuffer<ReflectionDenoisePush> push;

#define heap_frame_slot (push.frame_slot)
#define heap_image_index (push.image_index)

uint2 reflection_screen_size()
{
    return uint2(push.screen_width, push.screen_height);
}

int2 reflection_clamp_pixel(int2 pixel)
{
    return clamp(pixel, int2(0, 0), int2(reflection_screen_size()) - 1);
}

float reflection_srgb_to_linear(float c)
{
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 reflection_srgb_to_linear(float3 c)
{
    return float3(reflection_srgb_to_linear(c.x), reflection_srgb_to_linear(c.y), reflection_srgb_to_linear(c.z));
}

// ---- camera / projection (Vulkan row-vector convention used across Deren) ----

float4x4 reflection_matrix_inverse(float4x4 matrix)
{
    return spirv_asm { result:$$float4x4 = OpExtInst glsl450 MatrixInverse $matrix; };
}

float3 FFX_DNSR_Reflections_ScreenSpaceToViewSpace(float3 uvz)
{
    VR_MAT4 proj = camera_at(heap_camera_slot).proj;
    float4 clip = float4(uvz.xy * 2.0 - 1.0, uvz.z, 1.0);
    float4 view = mul(clip, reflection_matrix_inverse(proj));
    return view.xyz / view.w;
}

float3 FFX_DNSR_Reflections_ViewSpaceToWorldSpace(float4 view_position)
{
    VR_MAT4 view = camera_at(heap_camera_slot).view;
    float4 world = mul(view_position, reflection_matrix_inverse(view));
    return world.xyz / world.w;
}

float3 FFX_DNSR_Reflections_WorldSpaceToScreenSpacePrevious(float3 world_position)
{
    VR_MAT4 prev_view_proj = camera_at(heap_camera_slot).prev_view_proj;
    float4 clip = mul(float4(world_position, 1.0), prev_view_proj);
    float2 ndc = clip.xy / clip.w;
    return float3(ndc * 0.5 + 0.5, 0.0);
}

float FFX_DNSR_Reflections_GetLinearDepth(float2 uv, float depth)
{
    return abs(FFX_DNSR_Reflections_ScreenSpaceToViewSpace(float3(uv, depth)).z);
}

bool FFX_DNSR_Reflections_IsGlossyReflection(float roughness)
{
    return roughness < push.max_roughness;
}

bool FFX_DNSR_Reflections_IsMirrorReflection(float roughness)
{
    return roughness < 0.0001;
}

// ---- current-frame G-buffer reads ----

float FFX_DNSR_Reflections_LoadDepth(int2 pixel_coordinate)
{
    Texture2D<float4> depth = heap_at<Texture2D<float4>>(heap_slots_gbuffer_depth + heap_image_index);
    return depth.Load(int3(reflection_clamp_pixel(pixel_coordinate), 0)).r;
}

float3 FFX_DNSR_Reflections_LoadWorldSpaceNormal(int2 pixel_coordinate)
{
    Texture2D<float4> normal = heap_at<Texture2D<float4>>(heap_slots_gbuffer_normal + heap_image_index);
    return normal.Load(int3(reflection_clamp_pixel(pixel_coordinate), 0)).xyz;
}

float FFX_DNSR_Reflections_LoadRoughness(int2 pixel_coordinate)
{
    Texture2D<float4> normal = heap_at<Texture2D<float4>>(heap_slots_gbuffer_normal + heap_image_index);
    return normal.Load(int3(reflection_clamp_pixel(pixel_coordinate), 0)).w;
}

float2 FFX_DNSR_Reflections_LoadMotionVector(int2 pixel_coordinate)
{
    Texture2D<float4> velocity = heap_at<Texture2D<float4>>(heap_slots_gbuffer_velocity + heap_image_index);
    return velocity.Load(int3(reflection_clamp_pixel(pixel_coordinate), 0)).rg;
}

float3 reflection_raw_radiance(int2 pixel_coordinate)
{
    Texture2D<float4> raw = heap_at<Texture2D<float4>>(heap_slots_rt_reflection + heap_frame_slot);
    float3 radiance = raw.Load(int3(reflection_clamp_pixel(pixel_coordinate), 0)).xyz;
    // Pixels the reflection ray did not fill (and any firefly that slipped through) must not enter the
    // temporal history: the resolve treats history as a color sample, and one inf/NaN would persist.
    if (any(isinf(radiance)) || any(isnan(radiance))) {
        return float3(0.0, 0.0, 0.0);
    }
    return max(radiance, float3(0.0, 0.0, 0.0));
}

float3 FFX_DNSR_Reflections_LoadRadiance(int2 pixel_coordinate)
{
    // Each stage loads a different radiance image through this one callback. Reprojection builds its
    // neighborhood from the raw reflection, exactly like AMD's Reproject.hlsl; the temporal resolve
    // reconstructs its local color box from the prefiltered radiance, like AMD's ResolveTemporal.hlsl.
    // The prefilter has its own raw reader in LoadNeighborhood below.
#if defined(REFLECTION_DENOISE_REPROJECT)
    return reflection_raw_radiance(pixel_coordinate);
#else
    Texture2D<float4> prefiltered = heap_at<Texture2D<float4>>(heap_slots_rt_reflection_prefiltered + heap_frame_slot);
    return prefiltered.Load(int3(reflection_clamp_pixel(pixel_coordinate), 0)).xyz;
#endif
}

float FFX_DNSR_Reflections_LoadRayLength(int2 pixel_coordinate)
{
    Texture2D<float4> raw = heap_at<Texture2D<float4>>(heap_slots_rt_reflection + heap_frame_slot);
    return raw.Load(int3(reflection_clamp_pixel(pixel_coordinate), 0)).w;
}

// ---- history reads ----

float3 FFX_DNSR_Reflections_LoadRadianceHistory(int2 pixel_coordinate)
{
    if (push.history_valid == 0u)
    {
        return 0.0;
    }
    Texture2D<float4> history = heap_at<Texture2D<float4>>(heap_slots_rt_reflection_denoised_history + heap_frame_slot);
    return history.Load(int3(reflection_clamp_pixel(pixel_coordinate), 0)).xyz;
}

float3 FFX_DNSR_Reflections_SampleRadianceHistory(float2 uv)
{
    if (push.history_valid == 0u)
    {
        return 0.0;
    }
    return heap_texel_lod0(heap_at<Texture2D<float4>>(heap_slots_rt_reflection_denoised_history + heap_frame_slot), heap_sampler_taa, uv).xyz;
}

float3 FFX_DNSR_Reflections_SampleWorldSpaceNormalHistory(float2 uv)
{
    if (push.history_valid == 0u)
    {
        return float3(0.0, 0.0, 1.0);
    }
    float4 history = heap_texel_lod0(heap_at<Texture2D<float4>>(heap_slots_rt_reflection_surface_history + heap_frame_slot), heap_sampler_taa, uv);
    return normalize(history.xyz);
}

float3 FFX_DNSR_Reflections_LoadWorldSpaceNormalHistory(int2 pixel_coordinate)
{
    if (push.history_valid == 0u)
    {
        return float3(0.0, 0.0, 1.0);
    }
    Texture2D<float4> history = heap_at<Texture2D<float4>>(heap_slots_rt_reflection_surface_history + heap_frame_slot);
    return normalize(history.Load(int3(reflection_clamp_pixel(pixel_coordinate), 0)).xyz);
}

float FFX_DNSR_Reflections_SampleRoughnessHistory(float2 uv)
{
    if (push.history_valid == 0u)
    {
        return 1.0;
    }
    float4 history = heap_texel_lod0(heap_at<Texture2D<float4>>(heap_slots_rt_reflection_surface_history + heap_frame_slot), heap_sampler_taa, uv);
    return history.w;
}

float FFX_DNSR_Reflections_LoadRoughnessHistory(int2 pixel_coordinate)
{
    if (push.history_valid == 0u)
    {
        return 1.0;
    }
    Texture2D<float4> history = heap_at<Texture2D<float4>>(heap_slots_rt_reflection_surface_history + heap_frame_slot);
    return history.Load(int3(reflection_clamp_pixel(pixel_coordinate), 0)).w;
}

float FFX_DNSR_Reflections_LoadDepthHistory(int2 pixel_coordinate)
{
    if (push.history_valid == 0u)
    {
        return FFX_DNSR_Reflections_LoadDepth(pixel_coordinate);
    }
    Texture2D<uint> history = heap_at<Texture2D<uint>>(heap_slots_rt_reflection_depth_history + heap_frame_slot);
    return asfloat(history.Load(int3(reflection_clamp_pixel(pixel_coordinate), 0)).r);
}

float FFX_DNSR_Reflections_SampleDepthHistory(float2 uv)
{
    if (push.history_valid == 0u)
    {
        int2 pixel = int2(uv * float2(reflection_screen_size()));
        return FFX_DNSR_Reflections_LoadDepth(pixel);
    }
    float2 size = float2(reflection_screen_size());
    float2 position = uv * size - 0.5;
    int2 base = int2(floor(position));
    float2 fraction = frac(position);
    float d00 = FFX_DNSR_Reflections_LoadDepthHistory(base + int2(0, 0));
    float d10 = FFX_DNSR_Reflections_LoadDepthHistory(base + int2(1, 0));
    float d01 = FFX_DNSR_Reflections_LoadDepthHistory(base + int2(0, 1));
    float d11 = FFX_DNSR_Reflections_LoadDepthHistory(base + int2(1, 1));
    return lerp(lerp(d00, d10, fraction.x), lerp(d01, d11, fraction.x), fraction.y);
}

float FFX_DNSR_Reflections_SampleVarianceHistory(float2 uv)
{
    if (push.history_valid == 0u)
    {
        return 1.0;
    }
    float2 moments = heap_texel_lod0(heap_at<Texture2D<float4>>(heap_slots_rt_reflection_moments_history + heap_frame_slot), heap_sampler_taa, uv).xy;
    return moments.x;
}

float FFX_DNSR_Reflections_SampleNumSamplesHistory(float2 uv)
{
    if (push.history_valid == 0u)
    {
        return 0.0;
    }
    float2 moments = heap_texel_lod0(heap_at<Texture2D<float4>>(heap_slots_rt_reflection_moments_history + heap_frame_slot), heap_sampler_taa, uv).xy;
    return moments.y;
}

float3 FFX_DNSR_Reflections_SamplePreviousAverageRadiance(float2 uv)
{
    if (push.history_valid == 0u)
    {
        return 0.0;
    }
    return heap_texel_lod0(heap_at<Texture2D<float4>>(heap_slots_rt_reflection_average_history + heap_frame_slot), heap_sampler_taa, uv).xyz;
}

float3 FFX_DNSR_Reflections_SampleAverageRadiance(float2 uv)
{
    return heap_texel_lod0(heap_at<Texture2D<float4>>(heap_slots_rt_reflection_average + heap_frame_slot), heap_sampler_taa, uv).xyz;
}

// ---- current-frame denoiser intermediate reads ----

float3 FFX_DNSR_Reflections_LoadRadianceReprojected(int2 pixel_coordinate)
{
    Texture2D<float4> reprojected = heap_at<Texture2D<float4>>(heap_slots_rt_reflection_reprojected + heap_frame_slot);
    return reprojected.Load(int3(reflection_clamp_pixel(pixel_coordinate), 0)).xyz;
}

float FFX_DNSR_Reflections_LoadVariance(int2 pixel_coordinate)
{
    Texture2D<float4> moments = heap_at<Texture2D<float4>>(heap_slots_rt_reflection_moments_prefiltered + heap_frame_slot);
    return moments.Load(int3(reflection_clamp_pixel(pixel_coordinate), 0)).x;
}

float FFX_DNSR_Reflections_LoadNumSamples(int2 pixel_coordinate)
{
    Texture2D<float4> moments = heap_at<Texture2D<float4>>(heap_slots_rt_reflection_moments_prefiltered + heap_frame_slot);
    return moments.Load(int3(reflection_clamp_pixel(pixel_coordinate), 0)).y;
}

float FFX_DNSR_Reflections_LoadReprojectedNumSamples(int2 pixel_coordinate)
{
    Texture2D<float4> moments = heap_at<Texture2D<float4>>(heap_slots_rt_reflection_moments + heap_frame_slot);
    return moments.Load(int3(reflection_clamp_pixel(pixel_coordinate), 0)).y;
}

void FFX_DNSR_Reflections_LoadNeighborhood(
    int2 pixel_coordinate,
    out float3 radiance,
    out float variance,
    out float3 normal,
    out float depth,
    int2 screen_size)
{
    // AMD's Prefilter.hlsl filters the RAW radiance; the variance next to it is the value the
    // reprojection stage wrote for this pixel. Reading the reprojected radiance here would feed
    // prefilter its own temporal neighbor instead of the current reflection.
    radiance = reflection_raw_radiance(pixel_coordinate);
    Texture2D<float4> moments = heap_at<Texture2D<float4>>(heap_slots_rt_reflection_moments + heap_frame_slot);
    variance = moments.Load(int3(reflection_clamp_pixel(pixel_coordinate), 0)).x;
    normal = FFX_DNSR_Reflections_LoadWorldSpaceNormal(pixel_coordinate);
    float2 uv = (float2(pixel_coordinate) + 0.5) / float2(screen_size);
    depth = FFX_DNSR_Reflections_GetLinearDepth(uv, FFX_DNSR_Reflections_LoadDepth(pixel_coordinate));
}

// ---- writes ----

void FFX_DNSR_Reflections_StoreRadianceReprojected(int2 pixel_coordinate, float3 value)
{
    RWTexture2D<float4> reprojected = heap_at<RWTexture2D<float4>>(heap_slots_rt_reflection_reprojected_storage + heap_frame_slot);
    reprojected[reflection_clamp_pixel(pixel_coordinate)] = float4(value, 1.0);
}

void FFX_DNSR_Reflections_StoreAverageRadiance(int2 pixel_coordinate, float3 value)
{
    // ONLY the current average: reprojection reads last frame's average, so the history is copied after the
    // resolve has consumed it (see reflection_copy_history.slang).
    RWTexture2D<float4> average = heap_at<RWTexture2D<float4>>(heap_slots_rt_reflection_average_storage + heap_frame_slot);
    average[reflection_clamp_pixel(pixel_coordinate)] = float4(value, 1.0);
}

void FFX_DNSR_Reflections_StoreVariance(int2 pixel_coordinate, float value)
{
    RWTexture2D<float2> moments = heap_at<RWTexture2D<float2>>(heap_slots_rt_reflection_moments_storage + heap_frame_slot);
    int2 pixel = reflection_clamp_pixel(pixel_coordinate);
    moments[pixel] = float2(value, moments[pixel].y);
}

void FFX_DNSR_Reflections_StoreNumSamples(int2 pixel_coordinate, float value)
{
    RWTexture2D<float2> moments = heap_at<RWTexture2D<float2>>(heap_slots_rt_reflection_moments_storage + heap_frame_slot);
    int2 pixel = reflection_clamp_pixel(pixel_coordinate);
    moments[pixel] = float2(moments[pixel].x, value);
}

void FFX_DNSR_Reflections_StorePrefilteredReflections(int2 pixel_coordinate, float3 radiance, float variance)
{
    RWTexture2D<float4> prefiltered = heap_at<RWTexture2D<float4>>(heap_slots_rt_reflection_prefiltered_storage + heap_frame_slot);
    RWTexture2D<float2> moments = heap_at<RWTexture2D<float2>>(heap_slots_rt_reflection_moments_prefiltered_storage + heap_frame_slot);
    int2 pixel = reflection_clamp_pixel(pixel_coordinate);
    prefiltered[pixel] = float4(radiance, 1.0);
    moments[pixel] = float2(variance, FFX_DNSR_Reflections_LoadReprojectedNumSamples(pixel));
}

void FFX_DNSR_Reflections_StoreTemporalAccumulation(int2 pixel_coordinate, float3 radiance, float variance)
{
    int2 pixel = reflection_clamp_pixel(pixel_coordinate);
    // The resolve writes the CURRENT images; the history-copy stage moves them into the history images after
    // this pass has consumed the old ones. Writing the history images here would race with the reads above.
    RWTexture2D<float4> denoised = heap_at<RWTexture2D<float4>>(heap_slots_rt_reflection_denoised_storage + heap_frame_slot);
    RWTexture2D<float2> moments = heap_at<RWTexture2D<float2>>(heap_slots_rt_reflection_moments_storage + heap_frame_slot);
    RWTexture2D<float4> surface = heap_at<RWTexture2D<float4>>(heap_slots_rt_reflection_surface_current_storage + heap_frame_slot);
    RWTexture2D<uint> depth = heap_at<RWTexture2D<uint>>(heap_slots_rt_reflection_depth_current_storage + heap_frame_slot);

    denoised[pixel] = float4(radiance, 1.0);
    moments[pixel] = float2(variance, FFX_DNSR_Reflections_LoadNumSamples(pixel));
    surface[pixel] = float4(FFX_DNSR_Reflections_LoadWorldSpaceNormal(pixel), FFX_DNSR_Reflections_LoadRoughness(pixel));
    depth[pixel] = asuint(FFX_DNSR_Reflections_LoadDepth(pixel));
}
