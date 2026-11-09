// shaders/lighting/rt_reflection.slang - roughness-gated reflection trace.
//
// THE REFLECTION PASS starts at the G-buffer surface and writes radiance plus hit distance. With one sample it
// travels along the mirror direction about the G-buffer normal; surfaces above
// RT_REFLECTION_TRACE_MAX_ROUGHNESS skip the ray and sample the prefiltered environment at a roughness-selected
// mip instead, because a single mirror ray is not a useful estimate of a wide lobe. With two to four samples
// the ray directions are GGX-importance-sampled across that lobe and averaged, which is what lets the pass
// trace the rough surfaces the one-sample path leaves to the environment. The result is denoised by the
// reflection_denoise passes; the deferred lighting stage weighs it by Fresnel and (1 - roughness), and the toon
// stage uses the same roughness-aware weight.
//
// THE PAYLOAD IS WRITTEN EXACTLY ONCE ON EVERY PATH, and the raygen does NOT initialise it. The ray-traced
// shadow pass learned this the hard way: a raygen store equal to the miss store is treated as redundant and
// dropped, so an escaped reflection ray reads an undefined payload. Closest hit and miss each write the whole
// struct; nothing else touches it.

#include "common/heap_access.slang"

struct reflection_payload
{
    float3 radiance; // the radiance arriving along the reflection direction
    float distance;  // world-space distance to the hit, or a large value on a miss
};

// Above this material roughness the traced lobe is too wide to estimate with at most four rays: the surface's
// true reflection lobe is broad enough that the mirror/few-ray estimate is noisy rather than blurry. Those
// pixels take the prefiltered environment instead, at a mip chosen by roughness. UE's Lumen calls the same
// decision `MaxRoughnessToTrace` and defaults it to 0.4; 0.55 keeps the trace for the glossy parts of the
// character (boots, armour trim) while the matte cloth falls back.
static const float RT_REFLECTION_TRACE_MAX_ROUGHNESS = 0.55;

// The denoiser's glossy band is widened to RT_REFLECTION_TRACE_MAX_ROUGHNESS when the pass traces more than
// one ray (see `reflection_denoise_common.slang`). The one-sample path keeps the denoiser's original 0.2
// threshold, which is what makes `rt_reflection_samples = 1` byte-identical to the pre-sampling pass.

// The prefixes this file reads. The camera block supplies the position the view vector needs; the light block
// keeps light_dir at the same offset the shadow pass uses. Both are read through heap_access's accessors.
struct CameraUBO
{
    VR_MAT4 view;
    VR_MAT4 proj;
    float4 camera_pos;
};

struct LightUBO
{
    VR_MAT4 light_view_proj[4];
    float4 light_dir;
};

#include "common/material_record.slang"
// THE ANY-HIT PREDICATE IS SHARED WITH THE SHADOW TRACER: the same MASK cut must hide the transparent part of
// a leaf card from a reflection ray, exactly as it hides it from a shadow ray.
#include "common/rt_alpha_mask.slang"

struct RtReflectionPush
{
    VR_MAT4 inv_view_proj; // clip -> world
    float4 params;         // x = ray tmin, y = absolute offset floor, z = relative offset scale, w = rays per pixel
    uint frame_slot;
    uint image_index;
};
[[vk::push_constant]] ConstantBuffer<RtReflectionPush> pc;
#define heap_frame_slot (pc.frame_slot)
#define heap_image_index (pc.image_index)
[[vk::binding(16, 0)]] RaytracingAccelerationStructure mapped_tlas[2];

/// @brief world position of a depth-buffer sample, the same reconstruction the shadow pass uses
float3 reflection_world_position_from_depth(float2 uv, float depth)
{
    const float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    const float4 world = mul(clip, pc.inv_view_proj);
    return world.xyz / world.w;
}

/// @brief a cheap per-sample integer hash; the low bits of the dispatch index are too regular to seed with
uint reflection_hash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

/// @brief uniform [0,1) from a small PCG-style state; one call advances the state
float reflection_rand(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float((word >> 22u) ^ word) / 4294967295.0;
}

/// @brief importance-sample the GGX visible-normal lobe and mirror the view about it
///
/// THE NDF-ONLY FORM IS NOT ENOUGH. Sampling `D(h)` and reflecting unconditionally sends a
/// visible fraction of the samples below the surface at grazing angles (about a third at
/// `N·V = 0.25`, roughness 0.55). Those rays start inside the mesh, hit the same object from
/// behind, and closest-hit shades that backface with its colour. This is Heitz's
/// visible-normal distribution (VNDF) form: it stretches the view vector by alpha, samples a
/// disc around it, and unstretches the result, so the samples are distributed over the half
/// vectors that are actually visible from `view`.
///
/// @param normal G-buffer normal, in world space
/// @param view unit vector from the surface toward the camera
/// @param roughness linear material roughness, [0,1]
/// @param u two uniform [0,1) samples
float3 reflection_ggx_direction(float3 normal, float3 view, float roughness, float2 u)
{
    const float alpha = max(roughness * roughness, 1.0e-3);

    // The tangent frame and the view vector in it. `tangent x bitangent == normal`, so the
    // local-to-world transform below preserves the hemisphere.
    const float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    const float3 tangent = normalize(cross(up, normal));
    const float3 bitangent = cross(normal, tangent);
    const float3 view_local = float3(dot(view, tangent), dot(view, bitangent), dot(view, normal));

    // Stretch the view vector by alpha, sample the projected disc, then unstretch.
    const float3 stretched_view = normalize(float3(alpha * view_local.x, alpha * view_local.y, max(view_local.z, 1.0e-6)));
    const float stretched_length_squared = stretched_view.x * stretched_view.x + stretched_view.y * stretched_view.y;
    const float3 axis_x = stretched_length_squared > 1.0e-12
                              ? float3(-stretched_view.y, stretched_view.x, 0.0) / sqrt(stretched_length_squared)
                              : float3(1.0, 0.0, 0.0);
    const float3 axis_y = cross(stretched_view, axis_x);

    const float radius = sqrt(u.x);
    const float phi = 6.28318530718 * u.y;
    const float disc_x = radius * cos(phi);
    const float disc_y = radius * sin(phi);
    const float3 disc_point = disc_x * axis_x + disc_y * axis_y +
                              sqrt(max(1.0 - disc_x * disc_x - disc_y * disc_y, 0.0)) * stretched_view;
    const float3 half_local = normalize(float3(alpha * disc_point.x, alpha * disc_point.y, max(disc_point.z, 1.0e-6)));

    const float3 half_vector = normalize(tangent * half_local.x + bitangent * half_local.y + normal * half_local.z);
    return reflect(-view, half_vector);
}

// =============================================================================================================
// THE RAYGEN (entry `rgen_main`): one or more reflection rays per covered pixel.
//
// A sky pixel writes zero radiance and an "infinitely far" distance; the deferred composite only reads this
// where the G-buffer actually has geometry. One sample uses `reflect(-view, normal)`, the mirror direction of
// the camera ray about the raw G-buffer normal; two to four samples use the VNDF above. The offset along the
// normal keeps the ray from immediately hitting the surface it started on.
// =============================================================================================================
[shader("raygeneration")]
void rgen_main()
{
    RWTexture2D<float4> reflection = heap_at<RWTexture2D<float4>>(heap_slots_rt_reflection_storage + heap_frame_slot);
    uint extent_x, extent_y;
    reflection.GetDimensions(extent_x, extent_y);

    const int2 pixel = int2(DispatchRaysIndex().xy);
    if (pixel.x >= int(extent_x) || pixel.y >= int(extent_y))
    {
        return;
    }
    const float2 uv = (float2(pixel) + 0.5) / float2(float(extent_x), float(extent_y));

    const float depth = heap_texel_lod0(heap_at<Texture2D<float4>>(heap_slots_gbuffer_depth + heap_image_index), heap_sampler_gbuffer, uv).r;
    if (depth >= 1.0)
    {
        reflection[pixel] = float4(0.0, 0.0, 0.0, 1e30);
        return;
    }

    const float4 normal_roughness = heap_texel_lod0(heap_at<Texture2D<float4>>(heap_slots_gbuffer_normal + heap_image_index), heap_sampler_gbuffer, uv);
    const float3 normal = normalize(normal_roughness.xyz);
    const float3 world_pos = reflection_world_position_from_depth(uv, depth);
    const float3 view = normalize(camera_at(heap_camera_slot).camera_pos.xyz - world_pos);
    const float roughness = saturate(normal_roughness.w);
    const uint sample_count = clamp(uint(pc.params.w + 0.5), 1u, 4u);
    const int levels = max(env_cube_levels() - 1, 1);

    // ROUGH SURFACES SKIP THE RAY. The G-buffer's roughness is the material's linear roughness; above the
    // threshold the true lobe is so wide that even four rays are a noisy estimate. The prefiltered environment
    // already contains the same lighting integrated over exactly that lobe, so sampling it at a
    // roughness-selected mip is both closer to the correct answer and one texture fetch instead of up to four
    // BVH traversals. `sample_count` only changes the path below this gate.
    if (roughness > RT_REFLECTION_TRACE_MAX_ROUGHNESS)
    {
        const float lod = roughness * float(levels);
        reflection[pixel] = float4(env_cube_sample_lod(reflect(-view, normal), lod), 1e30);
        return;
    }

    const float distance = length(world_pos - camera_at(heap_camera_slot).camera_pos.xyz);
    const float3 origin = world_pos + normal * max(pc.params.y, pc.params.z * distance);

    float3 radiance_sum = float3(0.0, 0.0, 0.0);
    float distance_sum = 0.0;
    uint valid_samples = 0u;
    uint hit_samples = 0u;
    for (uint sample = 0u; sample < sample_count; ++sample)
    {
        float3 direction;
        if (sample_count == 1u)
        {
            direction = reflect(-view, normal);
        }
        else
        {
            // INDEX/FRAME/SAMPLE: a ring slot alone repeats every two frames. `image_index` adds the swapchain
            // image's cycle, so the temporal resolve sees a longer sample sequence. The two randoms are
            // separate statements because argument evaluation order is unspecified in Slang.
            uint rng = reflection_hash(uint(pixel.x) + uint(pixel.y) * extent_x + pc.frame_slot * 0x9e3779b9u +
                                       pc.image_index * 0x85ebca6bu + sample * 0xc2b2ae35u);
            const float u_x = reflection_rand(rng);
            const float u_y = reflection_rand(rng);
            direction = reflection_ggx_direction(normal, view, roughness, float2(u_x, u_y));

            // VNDF should keep almost every sample above the surface, but a grazing view can still return a
            // direction that would start inside the mesh. Reject those instead of shading the object's backface.
            if (dot(direction, normal) <= 0.0)
            {
                continue;
            }
        }

        RayDesc ray;
        ray.Origin = origin;
        ray.TMin = pc.params.x;
        ray.Direction = direction;
        ray.TMax = 1e30;

        reflection_payload payload;
        TraceRay(mapped_tlas[heap_frame_slot],
                 RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                 0x2, // RT-reflection instances only; bit0 marks shadow-only geometry
                 0,
                 0,
                 0,
                 ray,
                 payload);

        // A miss shades the environment through the miss shader's fixed mip. Multi-sample mode knows the
        // material roughness, so it can pick the mip that matches the sampled lobe instead.
        if (sample_count > 1u && payload.distance >= 1.0e29)
        {
            payload.radiance = env_cube_sample_lod(direction, roughness * float(levels));
        }
        radiance_sum += payload.radiance;
        ++valid_samples;
        // THE DISTANCE IS THE DENOISER'S RAY LENGTH. A miss is not a hit at 1e30: averaging it into the hit
        // distance makes FFX reconstruct a fake hit point far outside the scene and reject otherwise valid
        // history. Keep the hit average and report "no hit" only when every sample missed.
        if (payload.distance < 1.0e29)
        {
            distance_sum += payload.distance;
            ++hit_samples;
        }
    }

    // Every sample was rejected (only possible on the multi-sample path): the environment is the honest
    // fallback, and it keeps the previous frame's distance semantics.
    if (valid_samples == 0u)
    {
        reflection[pixel] = float4(env_cube_sample_lod(reflect(-view, normal), roughness * float(levels)), 1e30);
        return;
    }

    const float inverse_samples = 1.0 / float(valid_samples);
    const float output_distance = hit_samples > 0u ? distance_sum / float(hit_samples) : 1e30;
    reflection[pixel] = float4(radiance_sum * inverse_samples, output_distance);
}

// =============================================================================================================
// THE ANY-HIT STAGE (entry `ahit_main`): the alphaMode MASK cut for reflection rays.
//
// THE REFLECTION USED TO REFLECT MASKED GEOMETRY AS A SOLID CARD: a ray query has no any-hit stage, and this
// pipeline's reflection trace had no alpha test either. `IgnoreHit` (GLSL's ignoreIntersectionEXT) refuses the
// transparent part of the triangle before the closest hit can shade it. The predicate is the SAME function the
// shadow any-hit calls, so the two tracers cannot drift on which texels are solid.
// =============================================================================================================
[shader("anyhit")]
void ahit_main(inout reflection_payload payload)
{
    const RtMaskInstance instance = heap_at<StructuredBuffer<RtMaskInstance>>(heap_mask_instance_slot)[InstanceID()];
    if (rt_alpha_mask_cut(instance, pc.frame_slot))
    {
        IgnoreHit();
    }
}

// =============================================================================================================
// THE CLOSEST-HIT STAGE (entry `chit_main`): a cheap single-bounce shade of the hit surface.
//
// This is intentionally not the deferred path's full material evaluation: the reflection is filtered and
// weighted down by the composite, so the first pass keeps the hit cost small. It still varies per hit (base
// colour, sun term, environment specular) so a mirror shows the geometry rather than one flat colour.
// =============================================================================================================

[shader("closesthit")]
void chit_main(inout reflection_payload payload)
{
    const RtMaskInstance instance = heap_at<StructuredBuffer<RtMaskInstance>>(heap_mask_instance_slot)[InstanceID()];
    const Material material = material_at(heap_slots_materials, instance.material_index);

    // The geometric normal, in object space from the hit triangle's vertices and then taken to world space by
    // the instance transform. No attribute fetch is involved: a triangle hit has no interpolated normal here.
    float* vertices = (float*)rt_mask_lane_address(instance.vertex_address);
    const uint stride = instance.vertex_stride / 4u;
    const bool indexed = (instance.index_address.x != 0u) || (instance.index_address.y != 0u);
    float3 positions[3];
    for (uint k = 0u; k < 3u; ++k)
    {
        const uint i = uint(PrimitiveIndex()) * 3u + k;
        uint vertex = i;
        if (indexed)
        {
            uint* indices = (uint*)rt_mask_lane_address(instance.index_address);
            if (instance.index_type == 0u)
            {
                const uint word = indices[i >> 1u];
                vertex = (i & 1u) == 0u ? (word & 0xFFFFu) : (word >> 16u);
            }
            else
            {
                vertex = indices[i];
            }
        }
        const uint at = vertex * stride;
        positions[k] = float3(vertices[at + 0u], vertices[at + 1u], vertices[at + 2u]);
    }

    float3 normal_object = cross(positions[1] - positions[0], positions[2] - positions[0]);
    float3 normal_world = normalize(mul(instance.model, float4(normal_object, 0.0)).xyz);
    // A double-sided hit can face away from the incoming ray; flip it so the sun term is never negative.
    if (dot(normal_world, WorldRayDirection()) > 0.0)
    {
        normal_world = -normal_world;
    }

    const float3 sun_direction = normalize(light_at(heap_light_slot).light_dir.xyz);
    const float sun = saturate(dot(normal_world, sun_direction));
    const float3 albedo = material.base_color_factor.rgb;
    const float3 environment = env_cube_sample_lod(reflect(WorldRayDirection(), normal_world), 2.0, 0);

    payload.radiance = albedo * (0.15 + 0.85 * sun) + environment * 0.25;
    payload.distance = RayTCurrent();
}

/// @brief the ray escaped the scene: the reflection is the environment along the ray
[shader("miss")]
void miss_main(inout reflection_payload payload)
{
    payload.radiance = env_cube_sample_lod(WorldRayDirection(), 1.0, 0);
    payload.distance = 1e30;
}
