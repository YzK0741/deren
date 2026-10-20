// THE HZB CULL STAGE (M3 phase B1): one workgroup per ITEM, walking that item's host-selected candidates and
// testing each against the HISTORY HZB - the max pyramid of the same frame slot's PREVIOUS use, i.e. the depth of
// frame N-2 (spec §4.3, §6, §7.2, §8.2). It writes the per-candidate flags, the compacted survivor list, the
// per-item survivor count and the frame's stats block. NOTHING CONSUMES THE SURVIVORS IN THIS PHASE: the mesh
// stage keeps reading the host-culled table (phase B2 swaps it), so this stage cannot change a pixel - its
// acceptance is the in-engine verification against the committed CPU reference.
//
// WHY ONE WORKGROUP PER ITEM. The item is the unit whose candidates the host already compacted into ONE run of the
// host-culled meshlet table (runtime.cpp's record_visibility_items publishes `visible_run`s at the primitive's own
// meshlet_base), and the run's count is known to the host, so `dispatch(item_count, 1, 1)` is exactly the work that
// exists - no item-id table, no prefix sum, no indirect dispatch (§2.1: the RHI has no dispatch-indirect verb and
// the cull does not need one). The workgroup is 64 threads because candidate counts are small (Bistro: ~13 per
// item near the camera) and the extra threads cost one early-out.
//
// THE CANDIDATE RECORD SOURCE IS THE HOST-CULLED LANE, NOT THE IMPORT-TIME TABLE. `HzbItem.source` names it:
// 0 = the item is not a host-cut run (its candidates are the shared meshlet table, which phase B1 deliberately does
// NOT walk - the host writes candidate_count = 0 for those items), 1 = the item's candidate records are
// `culled_meshlet_at(frame_slot, meshlet_base + i)`, the exact lane the host wrote while it recorded the frame.
//
// COMPACTION ORDER IS A CONTRACT, NOT A CONVENIENCE (spec §4.3, §10.1 item 3): survivors are written in ASCENDING
// candidate order into the SAME absolute index space as the flags/source table
// (`survivors[frame_slot * meshlet_capacity + meshlet_base + k]` = the k-th survivor RECORD), because the CPU
// reference's `hzb_compact` is the contract and the in-engine probe compares entry by entry. This stage gets that
// order WITHOUT wave intrinsics: after the per-candidate flags are written, thread 0 re-reads them in index order
// and copies the kept records out serially. That costs at most `candidate_count` iterations ON ONE THREAD per item
// (bounded in total by the meshlet table's 65536 records, i.e. a few thousand instructions per frame at Bistro's
// candidate counts) and it is deterministic by construction - a subgroup-ballot/prefix scheme would need a
// capability and would still have to allocate per-wave bases deterministically.
//
// THE TABLE LAYOUTS AND THE TEST ARE NOT DECLARED HERE: `common/hzb.slang` mirrors the CPU reference
// (deren::engine::visibility::hzb) and `geometry/mesh_geometry.slang` carries the meshlet record fetch, exactly as
// the visibility raster stage reads its records. A drift in either is not a validation error, it is a wrong
// decision - which is why the in-engine probe re-runs the CPU reference over the dumped data every capture.
#include "common/heap_access.slang"
#include "geometry/mesh_geometry.slang"
#include "common/hzb.slang"

/// the pass framework composes this block per dispatch: the two values the host knows before the frame records,
/// then the two heap index lanes every stage block ends with (spec C14). `image_index` is unused (this stage reads
/// no image) but is declared because the framework appends it to EVERY stage's block.
struct HzbCullPush
{
    uint item_count;      // the number of items = the dispatch's group count
    uint expected_width;  // the render extent the history header must describe (§7.2's header guard)
    uint expected_height;
    uint frame_slot;      // appended by the pass framework: which lane of every HZB table this frame owns
    uint image_index;     // appended by the pass framework; unused by this stage
};
[[vk::push_constant]] ConstantBuffer<HzbCullPush> push;
// the framework appends the index lanes by byte offset; the accessor macro keeps the shader's indexing the same
// spelling the other stages use (heap_access.slang's per-frame arrays add this to their heap slot).
#define heap_frame_slot (push.frame_slot)

/// the shader's [numthreads(64, 1, 1)]: the host's dispatch is one group per item and the pass's behaviour declares
/// the same number (source/engine/pass/hzb_cull.cpp). The serial compaction below relies on this being the width.
static const uint hzb_cull_group_size = 64u;

// ---- THE DRAW-ITEM MODEL MATRIX, from heap_slots_visibility_items ----
//
// The cull needs ONE field of the item: the model matrix. The draw-item record is 112 bytes = 28 u32 words (whose
// shape deren::engine::visibility::draw_item pins on the host, and whose reader in this tree is
// visibility_resolve.slang's VisibilityDrawItem), with the model's 16 floats at dword 12. Reading the words and
// bit-casting is deliberate: a `StructuredBuffer<VR_MAT4>` at this slot would stride 64 bytes, not 112, and the
// model would come out of the wrong bytes for every item but the first.
static const uint hzb_draw_item_dwords = 28u;      // mirrors visibility::draw_item_bytes / sizeof(uint32_t)
static const uint hzb_draw_item_model_dword = 12u; // mirrors visibility::draw_item_model_offset / sizeof(uint32_t)

/// @brief the item's model matrix, as the CPU reference reads it (the 16 floats in the renderer's memory order)
HzbMatrix hzb_draw_item_model(StructuredBuffer<uint> items, uint item_row)
{
    const uint base = item_row * hzb_draw_item_dwords + hzb_draw_item_model_dword;
    HzbMatrix model;
    for (uint index = 0u; index < 16u; ++index)
    {
        model.lane[index] = asfloat(items[base + index]);
    }
    return model;
}

/// @brief one stats row's atomic add (the only cross-item accumulation this stage does, §4.3)
/// @note ZERO-ADDS ARE SKIPPED: `InterlockedAdd(x, 0)` is still an atomic on the same cache line across ~2900 items,
///       and the released path (no candidates, nothing culled) is the common one.
void hzb_stat_add(RWStructuredBuffer<uint> results, uint results_lane_base, uint stat, uint value)
{
    if (value != 0u)
    {
        InterlockedAdd(results[results_lane_base + hzb_results_stats + stat], value);
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 group_id : SV_GroupID, uint thread_id : SV_GroupThreadID)
{
    const uint item = group_id.x;
    if (item >= push.item_count)
    {
        return; // the dispatch is exactly item_count groups, so this is defence only
    }
    const uint slot = push.frame_slot;
    // THE PER-FRAME LANES ARE INDEXED INSIDE ONE DESCRIPTOR (§5.3): the slot is part of the INDEX, never added to
    // the heap slot. `survivors` uses the meshlet table's own absolute index space, so the survivor's position and
    // the candidate's flags entry name the same candidate (§4.3).
    const uint results_lane_base = hzb_results_lane_base(slot);
    const uint survivors_lane_base = slot * hzb_candidate_capacity;
    const uint count_slot = results_lane_base + hzb_results_pass1_count + item;

    RWStructuredBuffer<float> pyramid = heap_at<RWStructuredBuffer<float>>(heap_slots_hzb_levels);
    RWStructuredBuffer<uint> header = heap_at<RWStructuredBuffer<uint>>(heap_slots_hzb_header);
    RWStructuredBuffer<uint> results = heap_at<RWStructuredBuffer<uint>>(heap_slots_hzb_results);
    RWStructuredBuffer<MeshletRecord> survivors = heap_at<RWStructuredBuffer<MeshletRecord>>(heap_slots_hzb_survivors);
    HzbItem src = heap_at<RWStructuredBuffer<HzbItem>>(heap_slots_hzb_items)[hzb_items_lane_base(slot) + item];

    // ---- THE EMPTY ITEM: publish a deterministic zero and leave. Not-a-host-cut-run items (source 0) and runs the
    // host emptied are both `candidate_count == 0`; writing the count (rather than leaving last frame's) is what
    // lets the probe/verify read every item's answer for THIS frame. ----
    if (src.source != hzb_item_source_culled || src.candidate_count == 0u)
    {
        if (thread_id == 0u)
        {
            results[count_slot] = 0u;
        }
        return;
    }
    // ---- OVERFLOW: DO NOT PUBLISH A PARTIAL RESULT (spec §9.4). The id's meshlet field is 12 bits and the flags
    // table is meshlet_capacity records, so a run past either cannot be described at all: the item's count is
    // frozen to 0 (nothing is drawn from a survivor list that does not exist) and the host's stats line reports the
    // overflow. The cull lane is a pure prediction in phase B, so "no survivors" here can never lose geometry. ----
    if (src.candidate_count > hzb_item_candidate_capacity || src.meshlet_base > hzb_candidate_capacity ||
        src.candidate_count > hzb_candidate_capacity - src.meshlet_base)
    {
        if (thread_id == 0u)
        {
            results[count_slot] = 0u;
            hzb_stat_add(results, results_lane_base, hzb_stat_overflow_items, 1u);
        }
        return;
    }

    StructuredBuffer<uint> draw_items = heap_at<StructuredBuffer<uint>>(heap_slots_visibility_items);
    const uint pipeline_item_row = slot * hzb_item_candidate_capacity + item;
    // THE FLAGS AND THE SURVIVOR RECORDS LIVE IN THE MESHLET TABLE'S OWN ABSOLUTE INDEX SPACE (§4.3): candidate
    // `meshlet_base + i` writes `flags[meshlet_base + i]`, and the k-th survivor is stored at
    // `survivors[meshlet_base + k]` of this frame slot's lane. `culled_meshlet_at` (mesh_geometry.slang) adds the
    // frame-slot lane itself, so reading a candidate and writing its survivor position is one index space - which
    // is what lets phase B2's raster hand `group_id.x` to the same record fetch the visibility stage already has.
    const uint candidate_base = survivors_lane_base + src.meshlet_base;

    // ---- THE HISTORY HEADER, ONCE PER ITEM: the camera/extent that BUILT the pyramid this stage reads (§4.1). An
    // unusable header (magic/extent/level-count mismatch - a resize race, §7.2) means NO candidate may be culled:
    // every one is copied to the survivor list unchanged, because phase B2's raster will read this table. ----
    const HzbHistory history = hzb_read_history(header, slot, push.expected_width, push.expected_height);
    if (!history.usable || !history.projection_ok)
    {
        for (uint index = thread_id; index < src.candidate_count; index += hzb_cull_group_size)
        {
            const MeshletRecord record = culled_meshlet_at(slot, src.meshlet_base + index);
            results[results_lane_base + hzb_results_flags + src.meshlet_base + index] = 0u;
            survivors[candidate_base + index] = record;
        }
        GroupMemoryBarrierWithGroupSync();
        if (thread_id == 0u)
        {
            results[count_slot] = src.candidate_count;
            hzb_stat_add(results, results_lane_base, hzb_stat_candidates, src.candidate_count);
            hzb_stat_add(results, results_lane_base, hzb_stat_survivors, src.candidate_count);
        }
        return;
    }

    // ---- THE TEST, ONE CANDIDATE PER THREAD STRIDE: conservative sphere-vs-HZB (§6); 0 = survivor / 1 = culled is
    // the flags table's contract (§4.3), and `hzb_occluded == false` is the fail-open direction. ----
    const HzbMatrix model = hzb_draw_item_model(draw_items, pipeline_item_row);
    const uint pyramid_lane_base = slot * hzb_pyramid_words(history.width, history.height);
    for (uint index = thread_id; index < src.candidate_count; index += hzb_cull_group_size)
    {
        const MeshletRecord record = culled_meshlet_at(slot, src.meshlet_base + index);
        const bool culled_by_history = hzb_occluded(history, pyramid, pyramid_lane_base, model, record.center, record.radius);
        results[results_lane_base + hzb_results_flags + src.meshlet_base + index] = culled_by_history ? 1u : 0u;
    }
    // THE COMPACTION'S PRECONDITION: every flag thread of this workgroup has written its word before thread 0 reads
    // the run back in index order (a group-scope barrier covers the global writes of the group's own invocations).
    GroupMemoryBarrierWithGroupSync();
    if (thread_id == 0u)
    {
        // ---- THE DETERMINISTIC COMPACTION (see the file header): thread 0 walks the item's candidates in ascending
        // order and copies the kept ones to `meshlet_base + k`. The candidate records are read again from the lane -
        // up to `candidate_count` reads on one thread, bounded by the meshlet table's capacity in total. ----
        uint running = 0u;
        for (uint index = 0u; index < src.candidate_count; ++index)
        {
            if (results[results_lane_base + hzb_results_flags + src.meshlet_base + index] == 0u)
            {
                survivors[candidate_base + running] = culled_meshlet_at(slot, src.meshlet_base + index);
                ++running;
            }
        }
        results[count_slot] = running;
        // ---- THE STATS (§4.3/§10.3): one atomic per item per row. `candidates = culled + survivors` by
        // construction, which is what the host/probe checks. ----
        hzb_stat_add(results, results_lane_base, hzb_stat_candidates, src.candidate_count);
        hzb_stat_add(results, results_lane_base, hzb_stat_culled, src.candidate_count - running);
        hzb_stat_add(results, results_lane_base, hzb_stat_survivors, running);
    }
}
