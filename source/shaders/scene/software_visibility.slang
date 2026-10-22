// shaders/scene/software_visibility.slang - THE SOFTWARE RASTER'S TILED COMPUTE STAGE (M4 Task 2 scaffolding).
//
// PRODUCES `software_visibility.comp.spv`. The hybrid visibility design (docs/superpowers/specs/2026-10-22-software-
// raster-design.md §3.2/§3.4) rasterizes the items the CPU classified as subpixel into TWO private R32_UINT images -
// the nearest software candidate's float depth BIT PATTERN and its packed 32-bit visibility id - so the resolve can
// pick between the hardware and the software candidate per pixel and the merge can write min(hw, sw) into the frame's
// depth. THIS STAGE LANDS THE ENTRY POINT AND ITS WORKGROUP SHAPE IN TASK 2; THE TILED WALK ITSELF IS TASK 3:
//
//   * one workgroup per 16x16 tile, 8x8 threads, four pixels per thread (tile_extent = 16 is the CSR list's tile),
//   * workgroup reads its CSR range `offsets[tile] .. offsets[tile+1]` out of the host-written `software_tiles`
//     buffer and walks every entry (the stream is sparse and load-balanced per tile, which is why the CPU bins it),
//   * per entry, projects the record's triangles and keeps the nearest `(depth_bits, id)` per owned pixel in
//     registers, then writes the whole 16x16 tile through to the software images - including empty tiles - so the
//     images are fully initialized with no clear pass.
//
// WHAT THIS FILE IS TODAY: a valid compute module with the DECLARED numthreads, no workgroup memory and no side
// effects. It exists so the pipeline can be created and the pass's declaration/behaviour can be validated by
// device-free tests before the GPU math lands; Task 3 replaces the body and adds the heap reads and the push block it
// needs (the two index lanes plus the tile dispatch coordinate, all through vkCmdPushDataEXT - this renderer has no
// pipeline layouts, so a heap-native stage composes its own block; see `common/heap_slot_constants.slang`).

// THE SAME EDGE AS `software_visibility_pass::group_size` (8), and the two are one contract: the pass dispatches one
// group per tile, and the shader's [numthreads] must equal the behaviour's declared group size.
[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
    // SCAFFOLDING: no image is written yet, so this stage cannot change a picture. The coordinate is declared because
    // Task 3's tile walk needs it (`tile = dispatch_thread_id.xy / 8`, the thread's pixel within the 16x16 tile), and an
    // empty body is what emits a valid module without reading anything today.
}
