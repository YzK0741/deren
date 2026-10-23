// Triangle-driven A/B prototype. Keep coverage math identical to the tiled producer.
#define SOFTWARE_TRIANGLE_ATOMIC
#include "scene/software_visibility.slang"

[numthreads(8, 8, 1)]
void main(uint3 group_id : SV_GroupID, uint3 lane : SV_GroupThreadID)
{
    RWTexture2D<uint> depth_image = heap_at<RWTexture2D<uint>>(heap_image_slot(heap_slots_software_depth_storage));
    RWTexture2D<uint> id_image = heap_at<RWTexture2D<uint>>(heap_image_slot(heap_slots_software_id_storage));
    RWStructuredBuffer<uint64_t> candidates = heap_at<RWStructuredBuffer<uint64_t>>(heap_slots_software_atomic + push.frame_slot);
    uint width, height;
    id_image.GetDimensions(width, height);
    const uint tiles_x = (width + 15u) / 16u;
    const uint tiles_y = (height + 15u) / 16u;
    if ((push.spare_lane & 6u) != 0u)
    {
        [unroll] for (uint y = 0u; y < 2u; ++y)
        [unroll] for (uint x = 0u; x < 2u; ++x)
        {
            uint2 pixel = group_id.xy * 16u + lane.xy * 2u + uint2(x, y);
            if (pixel.x >= width || pixel.y >= height) continue;
            uint index = pixel.y * width + pixel.x;
            if ((push.spare_lane & 2u) != 0u)
                candidates[index] = ~uint64_t(0);
            else
            {
                uint64_t candidate = candidates[index];
                depth_image[pixel] = uint(candidate >> 32u);
                id_image[pixel] = candidate == ~uint64_t(0) ? 0u : uint(candidate);
            }
        }
        return;
    }
    const uint tiles = tiles_x * tiles_y;
    const uint base = push.frame_slot * (software_header_words + tiles * 3u + 1u + software_entry_budget * 2u);
    StructuredBuffer<uint> words = heap_at<StructuredBuffer<uint>>(heap_slots_software_tiles);
    const uint entry = group_id.x + group_id.y * 65535u;
    const uint count = words[base + 2u];
    if (words[base] != tiles_x || words[base + 1u] != tiles_y || words[base + 3u] != 0u ||
        count > software_entry_budget || entry >= count) return;
    const uint entries_word = base + software_header_words + tiles * 3u + 1u;
    const uint item_index = words[entries_word + entry * 2u];
    const uint meshlet_index = words[entries_word + entry * 2u + 1u];
    if (item_index > visibility_id_item_mask || meshlet_index > visibility_id_meshlet_mask) return;
    const VisibilityDrawItem item = heap_at<StructuredBuffer<VisibilityDrawItem>>(heap_slots_visibility_items)[push.frame_slot * visibility_item_capacity + item_index];
    if (item.meshlet_table > 2u) return;
    if ((push.spare_lane & 1u) != 0u && hzb_survivors_usable(push.frame_slot, item_index))
    {
        StructuredBuffer<uint> results = heap_at<StructuredBuffer<uint>>(heap_slots_hzb_results);
        const uint flag = results[hzb_results_lane_base(push.frame_slot) + hzb_results_flags + item.meshlet_base + meshlet_index];
        if (flag != 0u && flag != 3u) return;
    }
    const Material material = material_at(heap_slots_materials, item.material_index);
    const MeshletRecord record = item.meshlet_table == 0u ? meshlet_at(item.meshlet_base + meshlet_index)
        : culled_meshlet_at(push.frame_slot, item.meshlet_base + meshlet_index);
    const MeshGeometryLanes geometry = visibility_geometry(item);
    const VR_MAT4 view_proj = mul(visibility_camera.view, visibility_camera.proj);
    const uint triangle_count = min(record.index_count / 3u, software_triangles_per_meshlet);
    for (uint triangle = lane.y * 8u + lane.x; triangle < triangle_count; triangle += 64u)
    {
        const SoftwareTriangleData data = software_prepare_triangle(triangle, item, record, geometry, view_proj,
                                                                   float2(width, height), (material.flags & 8u) != 0u);
        if (data.valid == 0u) continue;
        // Clamp in float before converting: huge offscreen coordinates must never overflow integer conversion.
        const int2 low = int2(ceil(clamp(data.bounds_min - 0.5, float2(0), float2(width, height))));
        const int2 high = int2(floor(clamp(data.bounds_max - 0.5, float2(-1), float2(width - 1u, height - 1u))));
        const uint id = software_pack_id(item_index, meshlet_index, triangle);
        for (int y = low.y; y <= high.y; ++y)
        for (int x = low.x; x <= high.x; ++x)
        {
            float raw_depth;
            if (!software_triangle_covers_pixel(data, float2(x, y) + 0.5, raw_depth)) continue;
            const uint bits = asuint(clamp(raw_depth, 0.0, 1.0));
            const uint64_t candidate = (uint64_t(bits) << 32u) | uint64_t(id);
            uint64_t previous;
            InterlockedMin(candidates[uint(y) * width + uint(x)], candidate, previous);
        }
    }
}
