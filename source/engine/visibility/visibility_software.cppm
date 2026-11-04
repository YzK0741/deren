/**
 * @file visibility_software.cppm
 * @defgroup engine_visibility_software M4 subpixel software raster: the CPU front half
 * @brief The conservative subpixel-cluster criterion, the stable [hardware prefix | software suffix] split and
 *        the 16x16 tile binning of the M4 hybrid visibility producer (spec §3.1/§3.3/§3.4).
 *
 * WHY THIS IS PURE CPU, AND WHY IT IS A MODULE OF ITS OWN. The M4 hybrid decides ON THE HOST, in the M1c planning
 * phase, which of a cut item's selected meshlet records the hardware raster will no longer dispatch and which ones a
 * tiled software raster would have to produce instead. That decision is arithmetic over the same view/projection/
 * model matrices the cut already used, and its failure mode is a HOLE: moving a cluster to the software path when
 * its sphere is not PROVABLY wholly in front of the near plane (the v1 software raster has no clipper) or not
 * provably smaller than the threshold loses visible geometry on a frame the class didn't cover. Keeping the
 * arithmetic free of Vulkan, of a device and of the shader compiler is what lets the headless tests pin the
 * conservative form: a too-eager classification is therefore a test failure, not a picture nobody understood.
 *
 * THE CONSERVATIVE CRITERION (spec §3.1), exactly:
 *
 *   world_center   = model * record.center
 *   model_scale    = ||model's linear 3x3 part||_F      (Frobenius norm, >= the operator norm -> conservative)
 *   world_radius   = record.radius * model_scale       (record.radius is already an outward-rounded bound)
 *   w              = -(view * world_center).z          (depth in front of the eye; standard perspective/ortho view)
 *   front_of_near  = world_radius < 0.98 * (w - near_plane)   AND   (w - world_radius) > 0
 *   focal          = max(|proj[0][0]| * width, |proj[1][1]| * height) * 0.5
 *   diameter_upper = 2 * world_radius * focal / (w - world_radius)      (perspective)
 *                  = world_radius * max(|proj[0][0]|*width, |proj[1][1]|*height)   (orthographic)
 *   subpixel       = front_of_near AND diameter_upper < threshold_px
 *
 * Every term is an UPPER bound on the true projected diameter: the sphere radius is scaled by the linear part's
 * Frobenius norm, and the perspective diameter uses the sphere's NEAREST point (w - radius), where the projection is
 * largest. Any error therefore leaves the cluster on the hardware path, which is the safe direction. The strict `<`
 * is deliberate: a cluster exactly at the threshold is not provably under it, so it stays hardware.
 *
 * THE SPLIT (spec §3.1/§3.3) is stable: hardware records keep their relative order as the run's PREFIX, software
 * records keep theirs as the SUFFIX. The packed 32-bit visibility id's `meshlet_in_item` field then needs no new
 * decode - the software record at suffix index k is called `hardware_count + k`, i.e. its lane position in the same
 * run the hardware ids index. The hardware dispatch becomes `hardware_count` groups (the prefix), which is the
 * "holes control" Task 1 measures.
 *
 * THE TILE BINNING (spec §3.4) is deterministic and never silently truncates: a record is filed under every 16x16
 * tile its conservative screen-space bounding box covers (clamped to the frame), in item order then record order,
 * with a fixed per-tile capacity. The MEASUREMENT entry point reports an over-capacity tile exactly (counts, max,
 * overflow flag) so Task 1 can size the default capacity from data; the PRODUCTION entry point refuses the whole
 * frame instead, because the hybrid must fall back to the complete hardware path rather than write a partial list.
 */

module;

export module deren.engine.visibility.software;

// The public surface is unchanged: the production domains (classification,
// tiling, merge) and the test-only reference rasterizer are re-exported here.
import deren.engine.visibility;
import deren.engine.meshlet;
import deren.utility.shared;

export import :classification;
export import :tiling;
export import :merge;
export import :reference;