// THE SLOT GRID'S CONSTANTS, in a file BOTH shading languages can read.
//
// WHY THIS FILE EXISTS. The grid's numbers were part of heap_slots.slang, which also declares GLSL heap
// arrays and the fetch helper - syntax no other language can parse. The migration to Slang needs the
// SAME numbers without a second copy: `tests/test_render_resources.cpp` parses this file's constants and
// asserts they equal `core::heap_slots` in vulkan/core/core.declarations.cppm, so a duplicate on the
// Slang side would defeat the one test that exists to catch a grid drift. Hence: constants here,
// language-specific declarations where they belong.
//
// THE GRID, stated once. A shader that declares `layout(descriptor_heap) uniform texture2D t[];` can only
// say `t[i]`, and GL_EXT_descriptor_heap resolves that to `heapBase + i * stride` - every such array
// aliases the heap from offset 0, so AN INDEX IS A BYTE OFFSET DIVIDED BY THAT KIND'S STRIDE, and the only
// way for a shader to find its data at a compile-time-known index is for the data to sit on a grid with a
// FIXED base. Both heaps start that grid at 1 MiB (past any reserved window a driver reports here: 94 KiB
// of resource heap, 64 KiB of sampler heap), the stride is fixed at 64 B - the smallest power of two
// covering the device's largest descriptor, 32 B - and each array below is a CONTIGUOUS RUN OF SLOTS.
//
// THE SAME TWO NUMBERS REACH SLANG FROM ITS COMMAND LINE, not from a declaration: `slangc` gets
// `-spirv-resource-heap-stride 64` and `-spirv-sampler-heap-stride 32`, because Slang's default is the
// size of the resource type rather than a flat grid. See docs/slang_migration.md.
//
// THESE NUMBERS MUST EQUAL vulkan/core/core.declarations.cppm's `core::heap_slots`, which is where the host
// reserves them; the host logs its own table at startup (`descriptor heap: slot grid ...`), which is how a
// drift is seen. See docs/descriptor_heap_migration.md.

#ifndef HEAP_SLOT_CONSTANTS_SLANG
#define HEAP_SLOT_CONSTANTS_SLANG

// the grid's base, in slots: 1 MiB / 64 B
static const uint heap_slot_base = 16384u;
// what a declaration must pass as its layout qualifier (see the note above)
static const uint heap_slot_stride = 64u;
// per-swapchain-image arrays are given 8 slots: more than any swapchain this renderer creates (3-4 in practice)
static const uint heap_image_capacity = 8u;

// the bindless texture array: binding 1, one slot per scene texture. Its run (0..1023) IS the array size - the host
// derives `scene_texture_capacity` as `heap_slots_materials - heap_slots_textures`, and the runtime refuses past it
static const uint heap_slots_textures = heap_slot_base + 0u;
// the material table (binding 5), and the top level structure (binding 16) as a TWO-SLOT array: the TLAS is
// rebuilt every frame, so one slot would hold one frame's structure while the other is still in flight (slot 1025,
// where it first was, is left unused rather than half-filled; see core.cppm's heap_slots).
static const uint heap_slots_materials = heap_slot_base + 1024u;
static const uint heap_slots_tlas = heap_slot_base + 1215u;
// per frame slot (2 of them), the scene set's frame-varying half
static const uint heap_slots_scene_camera = heap_slot_base + 1026u;
static const uint heap_slots_scene_light = heap_slot_base + 1028u;
static const uint heap_slots_cluster_counts = heap_slot_base + 1030u;
static const uint heap_slots_cluster_indices = heap_slot_base + 1032u;
static const uint heap_slots_instance_transforms = heap_slot_base + 1034u;
static const uint heap_slots_previous_transforms = heap_slot_base + 1036u;
static const uint heap_slots_skin_matrices = heap_slot_base + 1038u;
static const uint heap_slots_morph_data = heap_slot_base + 1040u;
// the SAME joint blocks one frame ago, per frame slot: what makes a DEFORMING vertex's motion vector carry
// its deformation instead of only its node's rigid motion (see runtime::advance_motion_deformations). Its
// number is at the END of the used region rather than beside the current family above, because growing an
// array in place would renumber every array after it (the rule the TLAS and the storage twins follow too).
static const uint heap_slots_skin_matrices_previous = heap_slot_base + 1255u;
// the MESHLET TABLE (docs/mesh_shaders.md step 3): one 48-byte record per meshlet, written once at scene
// import. ONE descriptor, not a per-frame pair - see core::heap_slots::meshlets for why that is safe.
static const uint heap_slots_meshlets = heap_slot_base + 1257u;
// the MESH CULLING COUNTERS (docs/mesh_shaders.md step 3, "what the culling buys"): a small RW buffer the mesh
// entries add to and the host reads back once, at shutdown - one descriptor with a per-frame lane inside it, like
// every other per-frame buffer (see core::heap_slots::meshlet_stats).
static const uint heap_slots_meshlet_stats = heap_slot_base + 1258u;
// THE HOST-CULLED MESHLET TABLE (docs/mesh_shaders.md step 3, the culling's cheapest stage): a per-frame copy of the
// primitives a CULLED session draws, compacted to the meshlets the host's frustum test kept - so the dispatch's
// group count IS the survivor count and a workgroup is never launched for a meshlet nobody will see. One descriptor
// with a per-frame lane, like every other per-frame buffer (see core::heap_slots::meshlet_culled).
static const uint heap_slots_meshlet_culled = heap_slot_base + 1259u;
// THE VISIBILITY ITEM TABLE (visibility-buffer v0.2b): a per-frame lane of draw_item records (112 B each), one
// per visible draw item, written by the host and read by the resolve compute pass (one descriptor, per-frame lane).
static const uint heap_slots_visibility_items = heap_slot_base + 1267u;
// THE TOON LANES BESIDE THE MATERIAL RECORD: one `uvec4` per material - x is that material's `_SDFLightmap`
// texture-array index, y its `_MetallicGlossMap`'s, and z and w are reserved - where 0 in a component is "do not
// read", the same contract `material_record::toon_indices` uses for its four. Lanes 0..3 of `deren::vulkan::toon_slot`
// live in the record; every lane from `sdf_lightmap` on lives here.
//
// WHY A BUFFER OF ITS OWN RATHER THAN A FIFTH COMPONENT THERE, and it is a measurement rather than a preference:
// the material record is INLINE IN THE PER-DRAW PUSH BLOCK (`surface.slang`: 96 B of material fields plus two
// heap lanes; `shadow.slang`: 96 + 3 lanes, with the cascade pinned at offset 96), so a word added to it moves
// every one of those offsets and grows a push block that is already near its limit. The naive version compiled
// and then crashed the renderer.
//
// ONE DESCRIPTOR, NOT A PER-FRAME PAIR, for the same reason `meshlets` is one: the values are fixed at scene
// import and never rewritten, so there is no frame in flight that could read a buffer the next frame is writing
// - which is the whole reason the frame-varying arrays above are pairs.
static const uint heap_slots_toon_lanes = heap_slot_base + 1260u;
// THE HEAD FRAME the face SDF shades against: three vectors per frame slot, written every frame.
//
// A PAIR, unlike the lane table above, and the difference is the whole rule: the lane table holds values fixed
// at import, while these CAN change every frame on a model whose head turns. So this is the frame-in-flight
// arrangement every other per-frame array here uses - one buffer per frame slot, so a frame being rendered
// never reads a buffer the next frame is rewriting.
//
// IT IS A BLOCK OF ITS OWN RATHER THAN A FIELD OF THE CAMERA'S, which would have been smaller: the head frame
// is a property of the CHARACTER and not of the eye looking at it, and a renderer that put it in the camera UBO
// would have a second thing to move the day a scene holds two characters facing different ways.
static const uint heap_slots_scene_head = heap_slot_base + 1261u;
// THE TOON LIGHT RIG: the character stage's global numbers (the sun/head-light split, their shadow-side colours,
// the chain's scalars), written once from the application's config - see `deren::vulkan::toon_rig`.
//
// 1263 AND NOT 1262, because `heap_slots_scene_head` above is a PER-FRAME-SLOT array: with two frames in flight it
// occupies 1261 and 1262, and a rig at 1262 is overwritten by the second slot's head frame every frame - which reads
// as a rig that simply has the head frame's numbers in it, with no error anywhere.
static const uint heap_slots_toon_rig = heap_slot_base + 1263u;
// THE MATERIAL COLOURS: `toon_colour_lane::count` `vec4`s per material, `material_index`-addressed. Read through
// the stage's own `character_toon_colour_lanes`, which must match `deren::vulkan::toon_colour_lane::count` EXACTLY - all
// four of the spellings of it (`character_forward.slang`, `pbr.slang`, `goo_rim.slang` and `outline.slang` through
// its `#include` of `pbr.slang`) are pinned against both the host enum and each other, most recently at 29 by step
// 13 (two `RS EFF` lanes, `_GooRSScalars` / `_GooRSTint`), before that at 27 by step
// 12 (`_GooAnisoRough`, the anisotropic lobe's two roughnesses). A copy left at an older count reads ANOTHER
// MATERIAL's lanes rather than failing.
static const uint heap_slots_toon_colours = heap_slot_base + 1264u;
// THE ARTICLE'S POST LUT (`ZmdLutPost.shader`'s `_LutTex`): a `1024x32` strip of 32 `32x32` tiles, i.e. a `32^3` colour
// cube, read by the post chain's composite AFTER the tonemap and the display encode. 1265 is the next free slot after
// the toon lanes' block (1260/1261/1263/1264 - see the note on 1263 for why 1262 is not one of them). The application bakes
// a NEUTRAL cube into it at startup (`main.cpp`'s post-LUT bake), so the lookup is the identity until an artist's cube
// replaces it, and `test_render_resources` holds this spelling against `core::heap_slots::post_lut`.
static const uint heap_slots_post_lut = heap_slot_base + 1265u;
// THE GOO REFERENCE'S PRE-INTEGRATED FGD LOOKUP TABLE (`PreIntegratedFGD_GGXDisneyDiffuse.png`, 64x64 RGBA8,
// UNORM): the screenshot's `specularFGD` / `diffuseFGD` / `reflectivity` are three READS OF THIS ONE IMAGE, so it
// is a GLOBAL texture rather than a per-material lane - the reference's `图像纹理` is one data-block shared by the
// Base, Face and Hair containers, and its sample coordinate is computed from shading parameters
// (`sqrt(NoV)`, `perceptualRoughness`, `fresnel0`) rather than from a material's uv or a material's map (spec
// `goo_step5_specular_spec.md` §3.4). The application uploads it once at startup (`runtime::set_goo_fgd_lut` from
// `main.cpp`), and 1266 is the next free slot after the post LUT above - 1262 is the head frame's second frame slot
// (see the note on 1263) and is NOT free. `test_render_resources` holds this spelling against
// `core::heap_slots::goo_fgd_lut`.
static const uint heap_slots_goo_fgd_lut = heap_slot_base + 1266u;
static const uint heap_slots_mask_instances = heap_slot_base + 1042u;
// frame-invariant images the shading stage samples
static const uint heap_slots_env_cube = heap_slot_base + 1044u;
static const uint heap_slots_irradiance_cube = heap_slot_base + 1045u;
static const uint heap_slots_brdf_lut = heap_slot_base + 1046u;
// per swapchain image: the shadow map, the ray-traced visibility, and every G-buffer/post image
static const uint heap_slots_shadow_map = heap_slot_base + 1047u;
static const uint heap_slots_rt_visibility = heap_slot_base + 1055u;
// the SAME image as a STORAGE descriptor (the visibility pass writes it, the lighting stage samples it, and no
// single heap descriptor is both): see core.cppm's heap_slots
static const uint heap_slots_rt_visibility_storage = heap_slot_base + 1223u;
// the visibility buffer (V0.1b): one R32_UINT id per render pixel, per swapchain image. 1231..1238 is the range the
// removed MegaLights storage twins left free, which is what keeps every existing per-image array's number.
static const uint heap_slots_visibility = heap_slot_base + 1231u;
// 1239..1246 stay RESERVED for a private capture depth the v1 software raster may want; the constant went
// with the family v0.4b retired from the frame (nothing in the shader tree reads it any more).

// ---- THE VISIBILITY RESOLVE'S FIVE OUTPUTS (visibility-buffer v0.4b): 1268..1307, appended at the END of the used
// ---- region like every array that cannot move into a free window (the rule the TLAS, the storage twins and
// ---- `skin_matrices_previous` follow: growing an array in place would renumber every array after it).
//
// THE SAME IMAGES AS THE G-BUFFER FAMILY, as STORAGE descriptors instead of sampled/attachment ones: the resolve is
// a COMPUTE stage, so its only route to the five G-buffer targets is a storage image in the heap - and no single
// heap descriptor is both, exactly the arrangement `heap_slots_rt_visibility_storage` already has for the ray-traced
// visibility image. Each entry is a per-swapchain-image array (`heap_image_capacity` slots), like the sampled target
// it twins.
static const uint heap_slots_gbuffer_albedo_storage = heap_slot_base + 1268u;
static const uint heap_slots_gbuffer_normal_storage = heap_slot_base + 1276u;
static const uint heap_slots_gbuffer_material_storage = heap_slot_base + 1284u;
static const uint heap_slots_gbuffer_velocity_storage = heap_slot_base + 1292u;
/// the scene colour target (`output_targets_pass::ids[5]` / the TAA working image): the resolve writes the emissive
/// term there, and the old path CLEARS it to zero before the G-buffer writes - see gbuffer_body.slang.
static const uint heap_slots_scene_color_storage = heap_slot_base + 1300u;

// ---- THE HZB OCCLUSION CULLING'S SIX TABLES (M3 phase A1; docs/superpowers/specs/2026-10-20-hzb-occlusion-design.md
// ---- §5.1). Appended after the used region's last slot, the rule every array that cannot move into a free window
// ---- follows (see the note on the resolve's five outputs above).
//
// ONE DESCRIPTOR WITH AN INTERNAL PER-FRAME LANE, NOT A PER-FRAME PAIR: these buffers' sizes are fixed per frame slot
// (hzb_levels' lane stride depends on the render extent, which the build/cull shaders learn from the header), so the
// accessor is `heap_at<...>(heap_slots_hzb_<table>)` and the INDEX is `frame_slot * stride + index` - NEVER
// `heap_slots_hzb_<table> + heap_frame_slot`. The second spelling reads another frame's lane, which is a wrong
// picture rather than an error (§5.3 of the spec says this warning belongs on the slots themselves).
// the max pyramid the build pass writes and the cull pass reads: per frame slot `pyramid_words(extent)` floats,
// level 0 (a copy of the depth image) then one 2x2-max level per step until 1x1
static const uint heap_slots_hzb_levels = heap_slot_base + 1308u;
// one HzbHeader per frame slot (magic/extent/level count + view, proj, camera position), written by the build pass in
// the same dispatch as the pyramid so the HZB and the matrices that built it can never disagree
static const uint heap_slots_hzb_header = heap_slot_base + 1309u;
// per-item candidate descriptions, HOST-written before recording: {meshlet_base, candidate_count, source, pad}
static const uint heap_slots_hzb_items = heap_slot_base + 1310u;
// the cull pass's results: flags[65536] (u32 per candidate: 0 = survivor, 1 = culled) + pass1_count[4096] +
// pass2_count[4096] + the stats block the host reads once at shutdown
static const uint heap_slots_hzb_results = heap_slot_base + 1311u;
// the compacted survivor records the cull pass writes, per-frame lane of meshlet_table_capacity records; the mesh
// stage reads them exactly as it reads the host-culled table
static const uint heap_slots_hzb_survivors = heap_slot_base + 1312u;
// an ALIAS of the mesh indirect command buffer, not a new buffer: the HOST-written heap descriptor at this slot points
// at it so the cull compute stage can write each item's surviving group count into the command word the draw later reads
static const uint heap_slots_hzb_commands = heap_slot_base + 1313u;
// ---- THE SOFTWARE RASTER'S OWN SURFACE (M4 Task 2): the two per-swapchain-image R32_UINT results of the
// tile-rasterized software pass, and the host-written CSR tile-list buffer per frame lane. The slot numbers mirror
// deren.engine.render_layout::heap_slots; test_render_resources.cpp compares the two files (names AND values).
// software_tiles is a per-FRAME array and is indexed with frame_slot * <lane stride>; the images are per-image
// arrays indexed with heap_image_index. See docs/superpowers/specs/2026-10-22-software-raster-design.md §3.2/§4.2.
static const uint heap_slots_software_depth = heap_slot_base + 1239u;
static const uint heap_slots_software_id = heap_slot_base + 1314u;
static const uint heap_slots_software_tiles = heap_slot_base + 1322u;
static const uint heap_slots_gbuffer_albedo = heap_slot_base + 1063u;
static const uint heap_slots_gbuffer_normal = heap_slot_base + 1071u;
static const uint heap_slots_gbuffer_material = heap_slot_base + 1079u;
static const uint heap_slots_gbuffer_depth = heap_slot_base + 1087u;
static const uint heap_slots_gbuffer_velocity = heap_slot_base + 1095u;
// the trace and the resolve as STORAGE descriptors (their compute passes write them; no single heap descriptor is
// both a sampled and a storage image): see core.cppm's heap_slots
static const uint heap_slots_taa_current = heap_slot_base + 1135u;
static const uint heap_slots_taa_history = heap_slot_base + 1143u;
static const uint heap_slots_post_color = heap_slot_base + 1151u;
static const uint heap_slots_bloom_l0 = heap_slot_base + 1159u;
static const uint heap_slots_bloom_l1 = heap_slot_base + 1167u;
static const uint heap_slots_bloom_l2 = heap_slot_base + 1175u;
static const uint heap_slots_bloom_l3 = heap_slot_base + 1183u;
static const uint heap_slots_display_color = heap_slot_base + 1207u;
// one past the last array: the highest slot any array above uses is base + 1323 (software_tiles' second frame
// lane; 1322..1323), and the grid reserves 2048 - room for the next families without moving one
static const uint heap_slot_count = 2048u;

// THE SAMPLER HEAP IS A SECOND GRID, and its own base: the sampler heap is a separate heap in this API, and the
// API caps it at 128 KiB, so it CANNOT use the resource grid's 1 MiB - 64 KiB, the reserved window the
// embedded-sampler path requires, is the largest base it can have, and the host refuses the heap path if a
// device's reserved window were larger or its sampler stride were not 32 B (core.cpp checks both).
// Index 0..4 are the five shared samplers the scene set's declarations choose between, in the order
// core::create_samplers makes them.
static const uint heap_sampler_base = 2048u; // 64 KiB / 32 B
static const uint heap_sampler_stride = 32u;
// In the order core::create_samplers makes them (see core.cppm's shared_sampler_infos), which is also the order
// they are written onto this grid.
static const uint heap_sampler_texture = heap_sampler_base + 0u;        // linear, repeat, mips to 12
static const uint heap_sampler_post = heap_sampler_base + 1u;           // linear, clamp, one mip
static const uint heap_sampler_gbuffer = heap_sampler_base + 2u;        // nearest, clamp
static const uint heap_sampler_post_nearest = heap_sampler_base + 3u;   // nearest, clamp (the same sampler twice)
static const uint heap_sampler_taa = heap_sampler_base + 4u;            // linear mag / nearest min, clamp
static const uint heap_sampler_shadow = heap_sampler_base + 5u;         // depth compare, clamp
// NO SEVENTH SAMPLER. The IBL images (env, irradiance, LUT) are sampled through `heap_sampler_texture`, the one
// with LINEAR filtering and the full mip chain; a `heap_sampler_env` name lived here and pointed at a slot the
// host never writes (core writes exactly the six above), which is what the grid contract test caught.

// THE SLOTS A CONVERTED STAGE ACTUALLY INDEXES. They are MACROS rather than constants because the index they add
// is a push-constant lane, i.e. not a compile-time value - this keeps the ~forty use sites unchanged (they read
// `heap_camera_slot` as an expression) instead of turning each one into a function call. THE INDEX RULE, stated
// once: per-frame arrays add heap_frame_slot, per-swapchain-image arrays add heap_image_index. Getting that
// backwards is silent - the read lands on another frame's or another image's descriptor.
//
// THE TWO INDICES ARE PUSH-CONSTANT LANES, and they live INSIDE each stage's own block: a heap pipeline has no
// pipeline layout, so vkCmdPushConstants has nothing to push to, but vkCmdPushDataEXT supplies exactly the same
// bytes and the shader reads them exactly as it always read push constants (the extension proposal says so in
// those words, and the heap-native probe proves it). A push_constant BLOCK may not carry an `offset` qualifier -
// measured, glslang says "only applies to block members" - so there is no shared block to put them in: each stage
// appends `uint frame_slot; uint image_index;` to ITS OWN block (at the end, so every existing field keeps its
// offset) and then aliases them to the two names below:
//
//     #define heap_frame_slot (push.frame_slot)
//     #define heap_image_index (push.image_index)
//
// (with `push` the stage's own block instance name). A stage that uses neither pays 8 bytes of push block.
#define heap_camera_slot (heap_slots_scene_camera + heap_frame_slot)
#define heap_light_slot (heap_slots_scene_light + heap_frame_slot)
#define heap_cluster_count_slot (heap_slots_cluster_counts + heap_frame_slot)
#define heap_cluster_index_slot (heap_slots_cluster_indices + heap_frame_slot)
#define heap_instance_slot (heap_slots_instance_transforms) // ONE descriptor, not a per-frame array
#define heap_previous_slot (heap_slots_previous_transforms + heap_frame_slot)
#define heap_skin_slot (heap_slots_skin_matrices + heap_frame_slot)
#define heap_skin_previous_slot (heap_slots_skin_matrices_previous + heap_frame_slot)
#define heap_morph_slot (heap_slots_morph_data + heap_frame_slot)
#define heap_meshlet_slot (heap_slots_meshlets) // ONE descriptor, not a per-frame array
#define heap_shadow_slot (heap_slots_shadow_map + heap_frame_slot)
#define heap_rt_visibility_slot (heap_slots_rt_visibility + heap_frame_slot)
#define heap_rt_visibility_storage_slot (heap_slots_rt_visibility_storage + heap_frame_slot)
#define heap_tlas_slot (heap_slots_tlas + heap_frame_slot)
#define heap_mask_instance_slot (heap_slots_mask_instances + heap_frame_slot)
#define heap_env_slot (heap_slots_env_cube)
#define heap_irradiance_slot (heap_slots_irradiance_cube)
#define heap_lut_slot (heap_slots_brdf_lut)
#define heap_material_slot (heap_slots_materials)
#define heap_texture_base (heap_slots_textures) // the bindless array's first slot: + the texture's own index
#define heap_image_slot(base) ((base) + heap_image_index) // for the per-swapchain-image arrays

#endif // HEAP_SLOT_CONSTANTS_SLANG
