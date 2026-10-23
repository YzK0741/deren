// M4 hybrid software visibility producer. One 8x8 workgroup owns a 16x16 tile.
// Each candidate triangle is fetched/projected once per workgroup, cached in shared memory, then tested only
// against pixel centers inside its conservative screen-space bounding box. Pixel ownership remains unique so the
// (depth, ID) ordering is deterministic without requiring device-wide atomics.
#include "common/heap_access.slang"
#include "common/material_record.slang"
#include "geometry/mesh_geometry.slang"
#include "common/hzb.slang"

struct SoftwareRasterPush
{
    uint frame_slot;
    uint image_index;
    uint spare_lane;
};
[[vk::push_constant]] ConstantBuffer<SoftwareRasterPush> push;
#define heap_frame_slot (push.frame_slot)
#define heap_image_index (push.image_index)

#include "common/mesh_fetch.slang"

static const uint software_tile_size = 16u;
static const uint software_entry_budget = 65536u;
static const uint software_header_words = 6u;
static const uint software_triangles_per_meshlet = 85u;
static const uint software_threads_per_tile = 64u;

struct SoftwareTriangleData
{
    float2 a;
    float2 b;
    float2 c;
    float3 depth;
    float twice_area;
    float2 bounds_min;
    float2 bounds_max;
    uint valid;
};
#ifndef SOFTWARE_TRIANGLE_ATOMIC
groupshared SoftwareTriangleData software_triangles[software_triangles_per_meshlet];
#endif

uint software_pack_id(uint item, uint meshlet, uint triangle)
{
    return visibility_id_valid | ((item & visibility_id_item_mask) << visibility_id_item_shift) |
           ((meshlet & visibility_id_meshlet_mask) << visibility_id_meshlet_shift) |
           ((triangle & visibility_id_triangle_mask) << visibility_id_triangle_shift);
}

SoftwareTriangleData software_prepare_triangle(uint triangle_index, VisibilityDrawItem item,
                                               MeshletRecord record, MeshGeometryLanes geometry,
                                               VR_MAT4 view_proj, float2 extent, bool double_sided)
{
    SoftwareTriangleData result = (SoftwareTriangleData)0;
    [unroll]
    for (uint corner = 0u; corner < 3u; ++corner)
    {
        const MeshVertex vertex = visibility_vertex(geometry, record, triangle_index, corner);
        const float4 clip = visibility_clip(visibility_world(vertex.position, item.model), view_proj);
        if (!all(isfinite(clip)) || clip.w <= 0.0)
            return result;
        float2 screen;
        if (!clip_to_screen_pixel(clip, extent, screen))
            return result;
        if (corner == 0u)
        {
            result.a = screen;
            result.depth.x = clip.z / clip.w;
        }
        else if (corner == 1u)
        {
            result.b = screen;
            result.depth.y = clip.z / clip.w;
        }
        else
        {
            result.c = screen;
            result.depth.z = clip.z / clip.w;
        }
    }

    result.twice_area = triangle_twice_area(result.a, result.b, result.c);
    if (!isfinite(result.twice_area) || result.twice_area == 0.0 || (!double_sided && result.twice_area >= 0.0) ||
        !all(isfinite(result.depth)))
        return result;
    result.bounds_min = min(result.a, min(result.b, result.c));
    result.bounds_max = max(result.a, max(result.b, result.c));
    result.valid = 1u;
    return result;
}

bool software_triangle_covers_pixel(SoftwareTriangleData triangle, float2 pixel_center, out float raw_depth)
{
    raw_depth = 0.0;
    if (triangle.valid == 0u || any(pixel_center < triangle.bounds_min) || any(pixel_center > triangle.bounds_max))
        return false;

    const float2 p = pixel_center;
    const float twice_area = triangle.twice_area;
    const float wa = ((triangle.b.x - p.x) * (triangle.c.y - p.y) - (triangle.b.y - p.y) * (triangle.c.x - p.x)) / twice_area;
    const float wb = ((triangle.c.x - p.x) * (triangle.a.y - p.y) - (triangle.c.y - p.y) * (triangle.a.x - p.x)) / twice_area;
    const float wc = 1.0 - wa - wb;
    const float3 weights = float3(wa, wb, wc);
    if (!all(isfinite(weights)) || any(weights < 0.0))
        return false;
    raw_depth = dot(weights, triangle.depth);
    return isfinite(raw_depth);
}

#ifndef SOFTWARE_TRIANGLE_ATOMIC
[numthreads(8, 8, 1)]
void main(uint3 group_id : SV_GroupID, uint3 group_thread_id : SV_GroupThreadID)
{
    RWTexture2D<uint> software_depth = heap_at<RWTexture2D<uint>>(heap_image_slot(heap_slots_software_depth_storage));
    RWTexture2D<uint> software_id = heap_at<RWTexture2D<uint>>(heap_image_slot(heap_slots_software_id_storage));
    uint width, height;
    software_id.GetDimensions(width, height);
    const uint tiles_x = (width + software_tile_size - 1u) / software_tile_size;
    const uint tiles_y = (height + software_tile_size - 1u) / software_tile_size;

    // The first dispatch clears both images so inactive CSR tiles cannot carry stale results into resolve. This
    // simple storage-only clear avoids transfer usage requirements and is ordered before the sparse raster dispatch.
    if ((push.spare_lane & 2u) != 0u)
    {
        if (group_id.x >= tiles_x || group_id.y >= tiles_y)
            return;
        const uint2 origin = group_id.xy * software_tile_size;
        [unroll]
        for (uint pixel_y = 0u; pixel_y < 2u; ++pixel_y)
        [unroll]
        for (uint pixel_x = 0u; pixel_x < 2u; ++pixel_x)
        {
            const uint2 pixel = origin + group_thread_id.xy * 2u + uint2(pixel_x, pixel_y);
            if (pixel.x < width && pixel.y < height)
            {
                software_depth[pixel] = 0xFFFFFFFFu;
                software_id[pixel] = 0u;
            }
        }
        return;
    }

    const uint tiles = tiles_x * tiles_y;
    const uint lane_base = push.frame_slot * (software_header_words + tiles * 3u + 1u + software_entry_budget * 2u);
    StructuredBuffer<uint> tile_words = heap_at<StructuredBuffer<uint>>(heap_slots_software_tiles);
    const uint list_tiles_x = tile_words[lane_base + 0u];
    const uint list_tiles_y = tile_words[lane_base + 1u];
    const uint entry_count = tile_words[lane_base + 2u];
    const uint overflow = tile_words[lane_base + 3u];
    const uint active_tile_count = tile_words[lane_base + 5u];
    const bool list_valid = list_tiles_x == tiles_x && list_tiles_y == tiles_y &&
                            entry_count <= software_entry_budget && active_tile_count <= tiles && overflow == 0u;
    if (!list_valid || group_id.x >= active_tile_count)
        return;
    const uint offsets_word = lane_base + software_header_words + tiles;
    const uint active_tiles_word = offsets_word + tiles + 1u;
    const uint entries_word = active_tiles_word + tiles;
    const uint tile = tile_words[active_tiles_word + group_id.x];
    if (tile >= tiles)
        return;
    const uint begin = list_valid ? min(tile_words[offsets_word + tile], entry_count) : 0u;
    const uint end = list_valid ? min(tile_words[offsets_word + tile + 1u], entry_count) : 0u;
    const uint2 tile_origin = uint2(tile % tiles_x, tile / tiles_x) * software_tile_size;
    const VR_MAT4 view_proj = mul(visibility_camera.view, visibility_camera.proj);
    const float2 extent = float2(float(width), float(height));
    const uint thread_index = group_thread_id.y * 8u + group_thread_id.x;

    uint best_depth[4];
    uint best_id[4];
    [unroll]
    for (uint pixel_index = 0u; pixel_index < 4u; ++pixel_index)
    {
        best_depth[pixel_index] = 0xFFFFFFFFu;
        best_id[pixel_index] = 0u;
    }

    for (uint entry = begin; entry < end; ++entry)
    {
        const uint entry_word = entries_word + entry * 2u;
        const uint item_index = tile_words[entry_word + 0u];
        const uint meshlet_index = tile_words[entry_word + 1u];
        if (item_index > visibility_id_item_mask || meshlet_index > visibility_id_meshlet_mask)
            continue;
        const uint item_row = push.frame_slot * visibility_item_capacity + item_index;
        const VisibilityDrawItem item = heap_at<StructuredBuffer<VisibilityDrawItem>>(heap_slots_visibility_items)[item_row];
        if (item.meshlet_table > 2u)
            continue;
        if ((push.spare_lane & 1u) != 0u && hzb_survivors_usable(push.frame_slot, item_index))
        {
            StructuredBuffer<uint> hzb_results = heap_at<StructuredBuffer<uint>>(heap_slots_hzb_results);
            const uint flags_index = hzb_results_lane_base(push.frame_slot) + hzb_results_flags + item.meshlet_base + meshlet_index;
            const uint flag = hzb_results[flags_index];
            if (flag != 0u && flag != 3u)
                continue; // software suffixes use the same final HZB/recovery visibility decision as hardware.
        }
        const Material material = material_at(heap_slots_materials, item.material_index);
        const bool double_sided = (material.flags & 8u) != 0u;
        const MeshletRecord record = item.meshlet_table == 0u
            ? meshlet_at(item.meshlet_base + meshlet_index)
            : culled_meshlet_at(push.frame_slot, item.meshlet_base + meshlet_index);
        const MeshGeometryLanes geometry = visibility_geometry(item);
        const uint triangle_count = min(record.index_count / 3u, software_triangles_per_meshlet);

        // One lane prepares each triangle; shared transformed triangle data is reused by all 256 tile pixels.
        for (uint triangle = thread_index; triangle < triangle_count; triangle += software_threads_per_tile)
            software_triangles[triangle] = software_prepare_triangle(triangle, item, record, geometry, view_proj, extent, double_sided);
        GroupMemoryBarrierWithGroupSync();

        [unroll]
        for (uint pixel_y = 0u; pixel_y < 2u; ++pixel_y)
        [unroll]
        for (uint pixel_x = 0u; pixel_x < 2u; ++pixel_x)
        {
            const uint pixel_index = pixel_y * 2u + pixel_x;
            const uint2 pixel = tile_origin + group_thread_id.xy * 2u + uint2(pixel_x, pixel_y);
            if (pixel.x >= width || pixel.y >= height)
                continue;
            const float2 pixel_center = float2(pixel) + 0.5;
            for (uint triangle = 0u; triangle < triangle_count; ++triangle)
            {
                float raw_depth;
                if (!software_triangle_covers_pixel(software_triangles[triangle], pixel_center, raw_depth))
                    continue;
                const uint depth_bits = asuint(clamp(raw_depth, 0.0, 1.0));
                const uint id = software_pack_id(item_index, meshlet_index, triangle);
                if (depth_bits < best_depth[pixel_index] ||
                    (depth_bits == best_depth[pixel_index] && (best_id[pixel_index] == 0u || id < best_id[pixel_index])))
                {
                    best_depth[pixel_index] = depth_bits;
                    best_id[pixel_index] = id;
                }
            }
        }
        // All lanes have finished reading the cache before the next entry reuses it.
        GroupMemoryBarrierWithGroupSync();
    }

    [unroll]
    for (uint pixel_y = 0u; pixel_y < 2u; ++pixel_y)
    [unroll]
    for (uint pixel_x = 0u; pixel_x < 2u; ++pixel_x)
    {
        const uint pixel_index = pixel_y * 2u + pixel_x;
        const uint2 pixel = tile_origin + group_thread_id.xy * 2u + uint2(pixel_x, pixel_y);
        if (pixel.x >= width || pixel.y >= height)
            continue;
        software_depth[pixel] = best_depth[pixel_index];
        software_id[pixel] = best_id[pixel_index];
    }
}

#endif
