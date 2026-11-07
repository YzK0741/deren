// shaders/lighting/rt_reflection.slang - roughness-gated reflection trace.
//
// THE FIRST REFLECTION PASS, deliberately narrow: the ray starts at the G-buffer surface, travels along the
// reflection of the view direction about the G-buffer normal, and writes the radiance it found plus the hit
// distance. Surfaces above RT_REFLECTION_TRACE_MAX_ROUGHNESS skip the ray and sample the prefiltered
// environment at a roughness-selected mip instead, because a single mirror ray is not a useful estimate of a
// wide lobe. The result is denoised by the reflection_denoise passes; the deferred lighting stage weighs it by
// Fresnel and (1 - roughness), and the toon stage uses the same roughness-aware weight.
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

// Above this material roughness a single mirror ray is no longer a useful estimate: the surface's true
// reflection lobe is wide enough that the mirror direction is an arbitrary sample of it, and one ray per
// pixel reads as a sharp, wrong highlight rather than as blur. Those pixels take the prefiltered environment
// instead, at a mip chosen by roughness. UE's Lumen calls the same decision `MaxRoughnessToTrace` and defaults
// it to 0.4; 0.55 keeps the trace for the glossy parts of the character (boots, armour trim) while the matte
// cloth falls back.
static const float RT_REFLECTION_TRACE_MAX_ROUGHNESS = 0.55;

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
    float4 params;         // x = ray tmin, y = absolute offset floor, z = relative offset scale, w = unused
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

// =============================================================================================================
// THE RAYGEN (entry `rgen_main`): one reflection ray per covered pixel.
//
// A sky pixel writes zero radiance and an "infinitely far" distance; the deferred composite only reads this
// where the G-buffer actually has geometry. The ray direction is `reflect(-view, normal)`, i.e. the mirror
// direction of the camera ray about the raw G-buffer normal. The offset along the normal keeps the ray from
// immediately hitting the surface it started on.
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
    const float3 direction = reflect(-view, normal);

    // ROUGH SURFACES SKIP THE RAY. The G-buffer's roughness is the material's linear roughness; above the
    // threshold the true lobe is so wide that the mirror direction is a meaningless single sample of it.
    // The prefiltered environment already contains the same lighting integrated over exactly that lobe, so
    // sampling it at a roughness-selected mip is both closer to the correct answer and one texture fetch
    // instead of a BVH traversal. The distance is left "infinitely far" so the composite still knows this
    // value did not come from a local hit. Scene-wide IBL only: this pass does not decode the G-buffer's
    // material id, so a per-object environment slot is a later refinement.
    const float roughness = saturate(normal_roughness.w);
    if (roughness > RT_REFLECTION_TRACE_MAX_ROUGHNESS)
    {
        const int levels = max(env_cube_levels() - 1, 1);
        const float lod = roughness * float(levels);
        reflection[pixel] = float4(env_cube_sample_lod(direction, lod), 1e30);
        return;
    }

    const float distance = length(world_pos - camera_at(heap_camera_slot).camera_pos.xyz);
    const float3 origin = world_pos + normal * max(pc.params.y, pc.params.z * distance);

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

    reflection[pixel] = float4(payload.radiance, payload.distance);
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
