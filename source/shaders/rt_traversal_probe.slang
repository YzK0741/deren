// A deterministic traversal acceptance probe. Each frame slot has a different TLAS transform.
#include "heap_access.slang"
[[vk::binding(7, 0)]] RaytracingAccelerationStructure scene[2];
struct ProbePush { uint2 destination; uint frame_slot; uint padding; uint2 resource_heap; };
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
// Read the same AS entries directly from the bound heap's GPU address. The current
// NVIDIA driver loses the device when ResourceHeapEXT supplies this pointer; BDA
// keeps native heap loading and integer-to-AS conversion without any binding mapping.
[shader("raygeneration")]
void native_main()
{
    uint64_t heap = (uint64_t(pc.resource_heap.y) << 32) | uint64_t(pc.resource_heap.x);
    uint64_t address = *(uint64_t*)(heap + uint64_t(16384u + 1000u + pc.frame_slot) * 64u);
    RaytracingAccelerationStructure structure = spirv_asm {
        result:$$RaytracingAccelerationStructure = OpConvertUToAccelerationStructureKHR $address;
    };
    probe_trace(structure);
}
// Retain the original built-in variant for driver/compiler diagnosis. It can be
// compiled independently with -entry builtin_main, using the same probe push block.
[shader("raygeneration")]
void builtin_main() { probe_trace(heap_at<RaytracingAccelerationStructure>(16384u + 1000u + pc.frame_slot)); }
[shader("closesthit")]
void chit_main(inout ProbePayload payload, BuiltInTriangleIntersectionAttributes attributes) { payload.hit = 1; }
[shader("miss")]
void miss_main(inout ProbePayload payload) { payload.hit = 0; }
