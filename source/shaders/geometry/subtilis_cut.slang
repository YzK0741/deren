// THE GPU SUBTILIS CUT (M2): one persistent workgroup per item.
//
// The immutable DAG groups, hierarchy references and cluster records live in buffers uploaded once per
// geometry generation. The item record carries their device addresses, the model matrix and where the
// selected run and its mesh-task command must go, so this pass needs no descriptor set and no heap slot of
// its own.
//
// ARCHITECTURE. This follows UE's persistent-thread culling shape rather than a thread-0 serial loop:
//  - every lane owns one 32-bit word of the group decision bitset (64 lanes cover the host's 2048-group
//    limit), so direct decisions and the dependency closure need no atomics or read-modify-write races;
//  - the closure runs as monotonically-clearing passes separated by group barriers; a pass may expose
//    newly-cleared producers, so it repeats until no decision changes (bounded by `group_count`);
//  - cluster emission is a contiguous-chunk compaction: each lane counts its chunk, lane 0 prefix-sums the
//    64 counts in chunk order, then each lane writes its selected clusters at its prefix. Chunks are
//    contiguous cluster-id ranges, so the output order is exactly the CPU reference's stable order.
//
// ORDER IS A CONTRACT. The output must be the same stable cluster-id order the CPU reference emits
// (`select_cut`). The chunked compaction preserves it without a serial thread.
//
// NO HYSTERESIS. The first GPU cut has no history buffer; `target_error_px` is the threshold. The CPU path
// stays available as the fallback and as the A/B reference.

struct CutPush
{
    uint2 items;       // the per-frame cut payload's device address, low half first
    uint item_count;   // dispatch group count
    uint reserved;
};
[[vk::push_constant]] ConstantBuffer<CutPush> push;

struct CutFrame
{
    float4 view[4];
    float4 projection[4];
    uint width;
    uint height;
    float near_plane;
    float target_error_px;
    uint kind;          // 1 = perspective, 2 = orthographic
    uint item_offset;
    uint item_stride;
    uint item_capacity;
};

struct CutItem
{
    float4 model[4];
    uint64_t groups;
    uint64_t references;
    uint64_t clusters;
    uint64_t meshlets;
    uint64_t command;
    uint group_count;
    uint cluster_count;
    uint output_base;
    uint output_capacity;
    uint command_index;
    uint flags;
    uint64_t page_arena;
    uint64_t page_table;
    uint64_t page_requests;
    uint page_count;
    uint request_capacity;
    uint request_frame;
    uint lod0_count;
};

// Host `dag_group`: fine range, coarse range, LOD sphere, then error. All members are 4-byte scalars.
struct CutGroup
{
    uint fine_first;
    uint fine_count;
    uint coarse_first;
    uint coarse_count;
    float center_x;
    float center_y;
    float center_z;
    float radius;
    float error;
};

struct CutMeshlet
{
    uint first_index;
    uint index_count;
    int base_vertex;
    uint page_id;      // streamed index page + 1; 0 means the draw's own index buffer
    float center_x;
    float center_y;
    float center_z;
    float radius;
    float axis_x;
    float axis_y;
    float axis_z;
    float cone_cos;
};

struct CutCluster
{
    CutMeshlet record;
    uint producer;
    uint consumer;
};

static const uint cut_max_groups = 2048u;
static const uint cut_words = 64u;
static const uint cut_none = 0xFFFFFFFFu;

// One workgroup per item; 64 lanes = one lane per group-bitset word.
static const uint cut_group_size = 64u;

groupshared uint cut_coarsen[cut_words];
groupshared uint cut_changed_count[cut_group_size];
groupshared uint cut_changed;
groupshared uint cut_cluster_counts[cut_group_size];
groupshared uint cut_cluster_offsets[cut_group_size + 1u];

bool cut_shared_bit(uint id)
{
    return (cut_coarsen[id >> 5u] & (1u << (id & 31u))) != 0u;
}

bool cut_finite(float v)
{
    return !isnan(v) && !isinf(v);
}

bool cut_finite16(float4 m[4])
{
    for (uint i = 0; i < 4; ++i)
        for (uint j = 0; j < 4; ++j)
            if (!cut_finite(m[i][j]))
                return false;
    return true;
}

bool cut_affine(float4 m[4])
{
    return cut_finite16(m) && m[0].w == 0.0f && m[1].w == 0.0f && m[2].w == 0.0f && m[3].w == 1.0f;
}

bool cut_projection_type(float4 p[4], out uint kind)
{
    kind = 0;
    if (!cut_finite16(p) || p[0].x <= 0.0f || p[1].y == 0.0f || p[0].y != 0.0f || p[0].z != 0.0f || p[0].w != 0.0f ||
        p[1].x != 0.0f || p[1].z != 0.0f || p[1].w != 0.0f || p[3].z >= 0.0f)
        return false;
    if (p[2].w == -1.0f && p[3].w == 0.0f && p[2].z <= -1.0f && p[3].x == 0.0f && p[3].y == 0.0f)
    {
        kind = 1;
        return true;
    }
    if (p[2].w == 0.0f && p[3].w == 1.0f && p[2].z < 0.0f && p[2].x == 0.0f && p[2].y == 0.0f)
    {
        kind = 2;
        return true;
    }
    return false;
}

float cut_scale_bound(float4 m[4])
{
    float one = 0.0f;
    float inf = 0.0f;
    for (uint i = 0; i < 3; ++i)
    {
        float col = 0.0f;
        float row = 0.0f;
        for (uint j = 0; j < 3; ++j)
        {
            col += abs(m[i][j]);
            row += abs(m[j][i]);
        }
        one = max(one, col);
        inf = max(inf, row);
    }
    return sqrt(one * inf);
}

// view * model, both column-major float4 columns.
void cut_camera(float4 view[4], float4 model[4], out float4 camera[4])
{
    camera[0] = view[0] * model[0].x + view[1] * model[0].y + view[2] * model[0].z + view[3] * model[0].w;
    camera[1] = view[0] * model[1].x + view[1] * model[1].y + view[2] * model[1].z + view[3] * model[1].w;
    camera[2] = view[0] * model[2].x + view[1] * model[2].y + view[2] * model[2].z + view[3] * model[2].w;
    camera[3] = view[0] * model[3].x + view[1] * model[3].y + view[2] * model[3].z + view[3] * model[3].w;
}

bool cut_cluster_selected(CutCluster c)
{
    const bool producer_open = c.producer == cut_none || cut_shared_bit(c.producer);
    const bool consumer_closed = c.consumer == cut_none || !cut_shared_bit(c.consumer);
    return producer_open && consumer_closed;
}

// The scene page table is ordered by (section, first) and the index section is 1. The cut only consumes
// index pages; the full cut buffers still provide the group/cluster/hierarchy data, because a 32-byte wire
// cluster carries neither the normal-cone axis nor the cone cosine a 48-byte output record needs.
static const uint cut_page_section_indices = 1u;
static const uint cut_page_state_absent = 0u;
static const uint cut_page_state_requested = 1u;
static const uint cut_page_state_resident = 2u;

bool cut_page_lookup(const CutItem item, const uint index, out uint page_index, out uint page_first, out uint page_count, out uint state)
{
    page_index = cut_none;
    page_first = 0u;
    page_count = 0u;
    state = cut_page_state_absent;
    if (item.page_table == 0ull || item.page_count == 0u)
        return false;

    uint* words = (uint*)item.page_table;
    uint lo = 0u;
    uint hi = item.page_count;
    while (lo < hi)
    {
        const uint mid = lo + (hi - lo) / 2u;
        uint* entry = words + mid * 8u;
        const uint section = entry[1];
        const uint first = entry[2];
        if (section < cut_page_section_indices || (section == cut_page_section_indices && first <= index))
            lo = mid + 1u;
        else
            hi = mid;
    }
    if (lo == 0u)
        return false;

    uint* entry = words + (lo - 1u) * 8u;
    if (entry[1] != cut_page_section_indices)
        return false;
    page_index = lo - 1u;
    page_first = entry[2];
    page_count = entry[3];
    state = entry[6];
    return true;
}

void cut_request_page(const CutItem item, const uint item_index, const uint page_index, const uint state)
{
    if (item.page_requests == 0ull || item.request_capacity == 0u || state != cut_page_state_absent)
        return;

    uint* header = (uint*)item.page_requests;
    uint slot = 0u;
    InterlockedAdd(header[0], 1u, slot);
    if (slot >= item.request_capacity)
    {
        header[2] = 1u; // overflow: the host reports it as a page-miss metric, never as a silent drop
        return;
    }
    uint* request = header + 4u + slot * 4u;
    request[0] = page_index;
    request[1] = 0u;
    request[2] = item_index;
    request[3] = item.request_frame;
}

groupshared uint cut_page_fallback;

[shader("compute")]
[numthreads(cut_group_size, 1, 1)]
void comp_main(uint3 group : SV_GroupID, uint3 lane : SV_GroupThreadID)
{
    if (group.x >= push.item_count)
        return;

    const uint64_t payload = (uint64_t(push.items.y) << 32u) | uint64_t(push.items.x);
    CutFrame frame = *(CutFrame*)payload;
    CutItem item = *(CutItem*)(payload + uint64_t(frame.item_offset) + uint64_t(group.x) * uint64_t(frame.item_stride));
    uint* command = (uint*)item.command;

    const uint group_count = item.group_count;
    uint kind = 0;
    bool force = !cut_affine(frame.view) || !cut_affine(item.model) || !cut_projection_type(frame.projection, kind);
    force |= frame.width == 0u || frame.height == 0u || !cut_finite(frame.near_plane) || frame.near_plane <= 0.0f ||
             !cut_finite(frame.target_error_px) || frame.target_error_px < 0.0f || group_count > cut_max_groups ||
             kind != frame.kind || frame.kind == 0u;

    float4 camera[4] = { float4(0.0f), float4(0.0f), float4(0.0f), float4(0.0f) };
    float scale = 1.0f;
    float pixels = 1.0f;
    if (!force)
    {
        cut_camera(frame.view, item.model, camera);
        scale = cut_scale_bound(camera);
        pixels = max(abs(frame.projection[0].x) * float(frame.width), abs(frame.projection[1].y) * float(frame.height)) * 0.5f;
        force = !cut_finite(scale) || !cut_finite(pixels);
    }

    CutGroup* groups = (CutGroup*)item.groups;
    CutCluster* clusters = (CutCluster*)item.clusters;
    uint* references = (uint*)item.references;

    // Each lane owns one bitset word. Direct decisions are written as one masked word, so neighbouring
    // groups in the same word cannot lose each other's updates.
    {
        const uint w = lane.x;
        uint bits = 0u;
        if (!force)
        {
            const uint base = w * 32u;
            for (uint b = 0u; b < 32u; ++b)
            {
                const uint id = base + b;
                if (id >= group_count)
                    break;
                CutGroup g = groups[id];
                float depth = -(camera[0].z * g.center_x + camera[1].z * g.center_y + camera[2].z * g.center_z + camera[3].z);
                float closest = depth - g.radius * scale;
                if (closest <= frame.near_plane)
                    continue;
                float metric = g.error * scale * pixels;
                if (kind == 1u)
                    metric /= closest;
                if (!cut_finite(metric))
                    continue;
                if (metric <= frame.target_error_px)
                    bits |= (1u << b);
            }
        }
        cut_coarsen[w] = bits;
    }
    GroupMemoryBarrierWithGroupSync();

    // Mirrors `close_coarsening`: a coarse group is demoted only when one of its fine clusters still has an
    // uncoarsened producer. Decisions only clear, so repeated passes converge monotonically to the CPU's
    // ascending-id result. Read the whole snapshot before writing, so a lane cannot observe another lane's
    // half-updated word.
    for (uint pass = 0u; pass < group_count; ++pass)
    {
        const uint w = lane.x;
        uint bits = cut_coarsen[w];
        uint cleared = 0u;
        const uint base = w * 32u;
        for (uint b = 0u; b < 32u; ++b)
        {
            const uint id = base + b;
            const uint mask = 1u << b;
            if (id >= group_count || (bits & mask) == 0u)
                continue;
            CutGroup g = groups[id];
            for (uint j = 0u; j < g.fine_count; ++j)
            {
                const uint cluster = references[g.fine_first + j];
                const uint producer = clusters[cluster].producer;
                if (producer != cut_none && !cut_shared_bit(producer))
                {
                    bits &= ~mask;
                    cleared = 1u;
                    break;
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();
        cut_coarsen[w] = bits;
        cut_changed_count[w] = cleared;
        GroupMemoryBarrierWithGroupSync();
        if (lane.x == 0u)
        {
            uint any = 0u;
            for (uint i = 0u; i < cut_group_size; ++i)
                any |= cut_changed_count[i];
            cut_changed = any;
        }
        GroupMemoryBarrierWithGroupSync();
        if (cut_changed == 0u)
            break;
    }
    GroupMemoryBarrierWithGroupSync();

    // Stable cluster-id compaction: 64 contiguous chunks, each counted by one lane, then prefix-summed in
    // chunk order. Every selected record is written once, at its exact CPU-reference position.
    const uint cluster_count = item.cluster_count;
    const uint chunk = (cluster_count + cut_group_size - 1u) / cut_group_size;
    const uint begin = min(lane.x * chunk, cluster_count);
    const uint end = min(begin + chunk, cluster_count);
    uint local = 0u;
    for (uint i = begin; i < end; ++i)
    {
        CutCluster c = clusters[i];
        if (cut_cluster_selected(c))
            ++local;
    }
    cut_cluster_counts[lane.x] = local;
    GroupMemoryBarrierWithGroupSync();

    if (lane.x == 0u)
    {
        uint running = 0u;
        for (uint i = 0u; i < cut_group_size; ++i)
        {
            cut_cluster_offsets[i] = running;
            running += cut_cluster_counts[i];
        }
        cut_cluster_offsets[cut_group_size] = running;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint selected = cut_cluster_offsets[cut_group_size];
    if (selected > item.output_capacity)
    {
        // The host only enables the GPU path when the DAG's maximum selected bound fits the item's
        // capacity. Reaching this is a host bug; refuse the run rather than truncate it.
        if (lane.x == 0u)
        {
            command[0] = 0u;
            command[1] = 1u;
            command[2] = 1u;
        }
        return;
    }

    // Index-page residency gate. A cluster is only emitted with a page id when its whole index window lies
    // inside ONE resident index page. Anything else falls the WHOLE item back to the LOD0 records the host
    // preloaded at `item.meshlets`: a partially streamed item cannot be mixed with LOD0 records because the
    // visibility path treats the item's command count as one contiguous run.
    cut_page_fallback = 0u;
    GroupMemoryBarrierWithGroupSync();
    if (item.page_table != 0ull && item.page_count != 0u)
    {
        for (uint i = begin; i < end; ++i)
        {
            CutCluster c = clusters[i];
            if (!cut_cluster_selected(c))
                continue;
            const uint first = c.record.first_index;
            const uint count = c.record.index_count;
            const uint last = first + count - 1u;
            uint page_index = cut_none;
            uint page_first = 0u;
            uint page_span = 0u;
            uint page_state = cut_page_state_absent;
            if (!cut_page_lookup(item, first, page_index, page_first, page_span, page_state))
            {
                cut_page_fallback = 1u;
                continue;
            }

            const uint64_t page_end = uint64_t(page_first) + uint64_t(page_span);
            if (uint64_t(last) >= page_end)
            {
                // The cluster crosses a page boundary. Request both ends (the host installs by page) and
                // fall back this frame; a later frame can only succeed if one page covers the whole window.
                cut_page_fallback = 1u;
                cut_request_page(item, group.x, page_index, page_state);
                uint last_page = cut_none;
                uint last_first = 0u;
                uint last_span = 0u;
                uint last_state = cut_page_state_absent;
                if (cut_page_lookup(item, last, last_page, last_first, last_span, last_state))
                    cut_request_page(item, group.x, last_page, last_state);
                continue;
            }

            if (page_state != cut_page_state_resident)
            {
                cut_request_page(item, group.x, page_index, page_state);
                cut_page_fallback = 1u;
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (cut_page_fallback != 0u)
    {
        if (lane.x == 0u)
        {
            command[0] = item.lod0_count;
            command[1] = 1u;
            command[2] = 1u;
        }
        return;
    }

    uint dst_index = cut_cluster_offsets[lane.x];
    for (uint i = begin; i < end; ++i)
    {
        CutCluster c = clusters[i];
        if (!cut_cluster_selected(c))
            continue;
        uint page_id = 0u;
        if (item.page_table != 0ull && item.page_count != 0u)
        {
            uint page_index = cut_none;
            uint page_first = 0u;
            uint page_span = 0u;
            uint page_state = cut_page_state_absent;
            if (cut_page_lookup(item, c.record.first_index, page_index, page_first, page_span, page_state) && page_state == cut_page_state_resident)
                page_id = page_index + 1u;
        }
        CutMeshlet record = c.record;
        record.page_id = page_id;
        CutMeshlet* dst = (CutMeshlet*)(item.meshlets + uint64_t(dst_index) * 48ull);
        *dst = record;
        ++dst_index;
    }

    if (lane.x == 0u)
    {
        command[0] = selected;
        command[1] = 1u;
        command[2] = 1u;
    }
}
