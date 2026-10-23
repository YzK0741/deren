#ifndef DEREN_SOFTWARE_ROUTE_SLANG
#define DEREN_SOFTWARE_ROUTE_SLANG
#include "common/mesh_fetch.slang"
// HZB compaction's optional software lane. Original candidate IDs remain immutable.
bool software_gpu_route_active() { return (push.mode & 2u) != 0u; }
uint software_route_lane()
{
    uint tiles=((push.expected_width+15u)/16u)*((push.expected_height+15u)/16u);
    return push.frame_slot*(6u+tiles*3u+1u+65536u*2u);
}
uint software_route_entries()
{
    uint tiles=((push.expected_width+15u)/16u)*((push.expected_height+15u)/16u);
    return software_route_lane()+6u+tiles*3u+1u;
}
bool software_gpu_candidate(uint item_index, HzbItem src, uint index, MeshletRecord record)
{
    if (!software_gpu_route_active()) return false;
    StructuredBuffer<uint> metadata=heap_at<StructuredBuffer<uint>>(heap_slots_software_route_metadata+push.frame_slot);
    float edge=asfloat(metadata[8u+src.meshlet_base+index]);
    float threshold=asfloat(metadata[0]), near_plane=asfloat(metadata[1]), focal=asfloat(metadata[2])*0.5;
    uint kind=metadata[3];
    if (!isfinite(edge) || edge<0.0 || !isfinite(threshold) || threshold<=0.0 ||
        !isfinite(near_plane) || near_plane<=0.0 || !isfinite(focal) || focal<=0.0 ||
        (kind!=1u && kind!=2u) || record.index_count==0u || record.index_count>255u || record.index_count%3u!=0u) return false;
    VisibilityDrawItem item=heap_at<StructuredBuffer<VisibilityDrawItem>>(heap_slots_visibility_items)[push.frame_slot*visibility_item_capacity+item_index];
    float squared=0.0;
    [unroll] for(uint r=0u;r<3u;++r)
    [unroll] for(uint c=0u;c<3u;++c) squared+=item.model[r][c]*item.model[r][c];
    float scale=sqrt(squared)*1.0001;
    float radius=record.radius*scale;
    float4 world=visibility_world(record.center,item.model);
    float4 view=mul(world,visibility_camera.view);
    float depth=-view.z;
    if (!all(isfinite(view)) || !isfinite(radius) || radius<0.0 || !(depth-radius>0.0) ||
        !(radius<0.98*(depth-near_plane))) return false;
    float bound=edge*scale*focal;
    if(kind==1u) {
        float nearest=depth-radius;
        float off_axis=(length(view.xy)+radius)/nearest;
        bound=bound/nearest*sqrt(1.0+off_axis*off_axis);
    }
    return isfinite(bound) && bound<threshold*0.9999;
}
// One bounded reservation per item/phase. Failure leaves the entire item's visible run on HW.
bool software_gpu_reserve(uint count,out uint start)
{
    start=0u;if(count==0u)return true;
    RWStructuredBuffer<uint> words=heap_at<RWStructuredBuffer<uint>>(heap_slots_software_tiles);
    StructuredBuffer<uint> metadata=heap_at<StructuredBuffer<uint>>(heap_slots_software_route_metadata+push.frame_slot);
    uint budget=min(metadata[4],65536u),current;
    InterlockedAdd(words[software_route_lane()+2u],0u,current);
    [loop] for(;;) {
        if(current>budget || count>budget-current)return false;
        uint observed;
        InterlockedCompareExchange(words[software_route_lane()+2u],current,current+count,observed);
        if(observed==current) {
            start=current;
            uint total=current+count,ignored;
            RWStructuredBuffer<uint> args=heap_at<RWStructuredBuffer<uint>>(heap_slots_software_route_arguments+push.frame_slot);
            InterlockedMax(args[0],min(total,65535u),ignored);
            InterlockedMax(args[1],(total+65534u)/65535u,ignored);
            InterlockedMax(words[software_route_lane()+4u],total,ignored);
            InterlockedMax(words[software_route_lane()+5u],total,ignored);
            return true;
        }
        current=observed;
    }
}
uint software_gpu_compact(uint item,HzbItem src,bool recovery,out uint visible)
{
    RWStructuredBuffer<uint> results=heap_at<RWStructuredBuffer<uint>>(heap_slots_hzb_results);
    RWStructuredBuffer<MeshletRecord> survivors=heap_at<RWStructuredBuffer<MeshletRecord>>(heap_slots_hzb_survivors);
    RWStructuredBuffer<uint> words=heap_at<RWStructuredBuffer<uint>>(heap_slots_software_tiles);
    uint lane=hzb_results_lane_base(push.frame_slot),flags=lane+hzb_results_flags+src.meshlet_base;
    uint target=recovery?3u:0u,wanted=0u;
    visible=0u;
    for(uint i=0u;i<src.candidate_count;++i) {
        uint flag=results[flags+i];
        if((flag&3u)==target) {++visible;if((flag&4u)!=0u)++wanted;}
    }
    uint start;
    bool reserved=software_gpu_reserve(wanted,start);
    uint hardware=0u,software=0u;
    uint output=push.frame_slot*hzb_candidate_capacity+src.meshlet_base+
        (recovery?results[lane+hzb_results_pass1_count+item]:0u);
    for(uint i=0u;i<src.candidate_count;++i) {
        uint flag=results[flags+i];
        if((flag&3u)==target) {
            if(reserved && (flag&4u)!=0u) {
                uint word=software_route_entries()+(start+software++)*2u;
                words[word]=item;words[word+1u]=i;
            } else survivors[output+hardware++]=culled_meshlet_at(push.frame_slot,src.meshlet_base+i);
        }
        results[flags+i]=flag&3u; // external contract remains exactly 0/1/3
    }
    return hardware;
}
#endif
