// A deterministic traversal acceptance probe. Each frame slot has a different TLAS transform.
#include "heap_access.slang"
[[vk::binding(7, 0)]] RaytracingAccelerationStructure scene[2];
struct ProbePush { uint2 destination; uint frame_slot; };
[[vk::push_constant]] ConstantBuffer<ProbePush> pc;
struct ProbePayload { uint hit; };

void probe_trace(RaytracingAccelerationStructure structure)
{
    uint pixel = DispatchRaysIndex().x;
    RayDesc ray;
    ray.Origin = pixel == 1 ? float3(20, 20, 1) : float3(0.25 + 3 * pc.frame_slot, 0.25, 1);
    ray.Direction = float3(0, 0, -1);
    ray.TMin = 0.01;
    ray.TMax = 2;
    ProbePayload payload;
    TraceRay(structure, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, pixel == 2 ? 0 : 255,
             0, 0, 0, ray, payload);
    uint* result = (uint*)((uint64_t(pc.destination.y) << 32) | uint64_t(pc.destination.x));
    result[pc.frame_slot * 3 + pixel] = payload.hit;
}
[shader("raygeneration")]
void rgen_main() { probe_trace(scene[pc.frame_slot]); }
// Diagnostic counterpart: identical rays and output, with heap-native AS loading.
[shader("raygeneration")]
void native_main() { probe_trace(heap_at<RaytracingAccelerationStructure>(16384u + 1000u + pc.frame_slot)); }
[shader("closesthit")]
void chit_main(inout ProbePayload payload, BuiltInTriangleIntersectionAttributes attributes) { payload.hit = 1; }
[shader("miss")]
void miss_main(inout ProbePayload payload) { payload.hit = 0; }
