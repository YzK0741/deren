/**
 * @file source/app/app_pipelines.cpp
 * @brief the demo pipelines' creation and the registration of every shader module the frame's
 *        passes and jobs build their own pipelines from (see app_pipelines.cppm for the contract).
 */
module;

#include <cstddef>
#include <cstdint>
// NO GLOBAL-MODULE-FRAGMENT INCLUDE, deliberately, and it is a measured decision rather than an
// oversight: source/app/app_pipelines.cpp keeps a textual <glm/glm.hpp> in its GMF to dodge the
// ambiguous-operator-new trap (with -fno-exceptions and the vendored std module, an impl unit that
// instantiates deren::utility::vessel::vector can see TWO 'operator new(size_t, align_val_t)'
// declarations - module std's and the textual libc++ copy baked into utility:data_block.pcm - and
// the instantiation then fails at allocate.h). This unit instantiates the very same vessel vectors
// and compiles clean with -Werror on this toolchain, both -fsyntax-only and to an object file, so
// the include is not load-bearing here. If a future allocator instantiation starts failing with
// 'call to operator new is ambiguous', re-add <glm/glm.hpp> as the first thing in this fragment -
// that is still the fix.

module deren.app.pipelines;

import deren.vstd;
import deren.utility;
import deren.app.config;
import deren.engine.runtime;
import deren.engine.gaussian_splatting.probe;

namespace deren::app {
    // Load a fragment/mesh SPIR-V pair for a named pipeline and create it via runtime; panic on failure. THE VERTEX
    // FILE IS GONE (docs/mesh_shaders.md step 4): a named geometry pipeline is built from a MESH stage and the
    // meshlet entry beside it, and the runtime refuses a name with no mesh module rather than falling back.
    void load_and_create_pipeline(deren::engine::runtime& runtime,
                                  std::filesystem::path const& shaders_dir,
                                  std::string_view const pipeline_name,
                                  std::string_view const fragment_file,
                                  std::string_view const mesh_file,
                                  std::string_view const meshlet_file) {
        deren::utility::vessel::vector<std::uint8_t> fragment_code;
        deren::utility::vessel::vector<std::uint8_t> mesh_code;
        deren::utility::vessel::vector<std::uint8_t> meshlet_code;
        load_shader(shaders_dir, fragment_file, fragment_code);
        load_shader(shaders_dir, mesh_file, mesh_code);
        if (!meshlet_file.empty()) {
            load_shader(shaders_dir, meshlet_file, meshlet_code);
        }

        std::expected<void, deren::utility::vessel::string> const result = runtime.make_pipeline(pipeline_name, fragment_code, mesh_code, meshlet_code);
        if (!result) {
            deren::utility::panic(std::source_location::current(), "failed to create pipeline '{}': {}", pipeline_name, result.error());
        }
        deren::utility::log("SUCCESS: pipeline '{}' created and cached in the runtime", pipeline_name);
    }

    // Create the demo pipelines up front: the standard PBR pipeline (used by the imported scene)
    // and the directional shadow pass (depth-only). The legacy triangle demo pipeline is
    // deliberately not created here - nothing draws it anymore.
    void setup_pipeline(deren::engine::runtime& runtime, std::filesystem::path const& shaders_dir) {
        // Standard PBR pipeline: the imported scene's primitives bind to it (the FIRST pipeline
        // created becomes the runtime's implicit default)
        load_and_create_pipeline(runtime, shaders_dir, "pbr", "pbr.frag.spv", "pbr.mesh.spv", "pbr.meshlet.spv");
        // Non-PBR "unlit" pipeline: flat base color, no lighting/shadows/IBL (see unlit.frag).
        // Registered as a SECOND named pipeline - the scene tree's default-semantics leaves draw
        // with whatever the runtime default is, so switching set_default_pipeline() between
        // "pbr" and "unlit" (gui "render mode") re-shades the whole scene without re-baking.
        // It shares pbr's GEOMETRY (pbr.vert.spv or its pbr.mesh.spv form) and differs only in its fragment
        // stage, so it gets a mesh form of its own from the same file.
        load_and_create_pipeline(runtime, shaders_dir, "unlit", "unlit.frag.spv", "pbr.mesh.spv", "pbr.meshlet.spv");

        // ---- THE VISIBILITY BUFFER'S RASTER PIPELINE (visibility-buffer v0.3b scaffolding) ----
        // The two modules from scene/visibility.slang: a mesh stage and the id-only fragment over ONE R32_UINT
        // target. NOTHING BINDS IT YET (the raster lane that dispatches through it is the next step), so this
        // changes no picture by construction; what it buys today is that the pipeline is CREATED, and a refusal
        // is a startup panic with the reason - where every other pipeline's failure is reported - instead of
        // surfacing in the middle of a frame.
        {
            deren::utility::vessel::vector<std::uint8_t> visibility_mesh_code;
            deren::utility::vessel::vector<std::uint8_t> visibility_fragment_code;
            load_shader(shaders_dir, "visibility.mesh.spv", visibility_mesh_code);
            load_shader(shaders_dir, "visibility.frag.spv", visibility_fragment_code);
            // the pass fetches the same two modules through `pass_context::shader`, so they must be REGISTERED
            runtime.register_shader("visibility.mesh.spv", visibility_mesh_code);
            runtime.register_shader("visibility.frag.spv", visibility_fragment_code);
            std::expected<void, deren::utility::vessel::string> const built = runtime.make_visibility_pipeline(visibility_mesh_code, visibility_fragment_code);
            if (!built) {
                deren::utility::panic(std::source_location::current(), "failed to create the visibility pipeline: {}", built.error());
            }
        }

        // ---- THE VISIBILITY RESOLVE'S COMPUTE MODULE (visibility-buffer v0.4c) ----
        // The resolve pass builds its OWN pipeline at create time from this module (through `pass_context::shader`),
        // so the app's job is to load and REGISTER it like the raster pair above: a name the pass asks for and
        // nobody registered would leave the pass disabled, and the capture lane consumerless again - silently.
        {
            deren::utility::vessel::vector<std::uint8_t> visibility_resolve_code;
            load_shader(shaders_dir, "visibility_resolve.comp.spv", visibility_resolve_code);
            runtime.register_shader("visibility_resolve.comp.spv", visibility_resolve_code);
            deren::utility::vessel::vector<std::uint8_t> visibility_feedback_code;
            load_shader(shaders_dir, "visibility_feedback.comp.spv", visibility_feedback_code);
            runtime.register_shader("visibility_feedback.comp.spv", visibility_feedback_code);
        }

        // ---- THE SOFTWARE RASTER'S MODULES (M4 Task 2, docs/superpowers/specs/2026-10-22-software-raster-
        //      design.md): the tiled compute stage and the depth-only merge's vertex/fragment pair. Task 4 appends the
        //      passes on a gated frame; Task 2 lands them WITH their pipelines, so the host-side job is the same as the
        //      resolve's above - load and REGISTER. A name nobody registered leaves a pass disabled with one log line,
        //      and a later wiring would look broken for a reason no frame shows. The merge is ONE .slang file with two
        //      entries and therefore TWO .spv names, exactly like the probe and the post chain.
        {
            deren::utility::vessel::vector<std::uint8_t> software_visibility_code;
            load_shader(shaders_dir, "software_visibility.comp.spv", software_visibility_code);
            runtime.register_shader("software_visibility.comp.spv", software_visibility_code);
            deren::utility::vessel::vector<std::uint8_t> software_atomic_code;
            load_shader(shaders_dir, "software_visibility_atomic.comp.spv", software_atomic_code);
            runtime.register_shader("software_visibility_atomic.comp.spv", software_atomic_code);
            deren::utility::vessel::vector<std::uint8_t> software_merge_vertex_code;
            deren::utility::vessel::vector<std::uint8_t> software_merge_fragment_code;
            load_shader(shaders_dir, "software_merge.vert.spv", software_merge_vertex_code);
            load_shader(shaders_dir, "software_merge.frag.spv", software_merge_fragment_code);
            runtime.register_shader("software_merge.vert.spv", software_merge_vertex_code);
            runtime.register_shader("software_merge.frag.spv", software_merge_fragment_code);
        }

        // ---- THE HZB BUILD MODULE (M3 phase A2, docs/superpowers/specs/2026-10-20-hzb-occlusion-design.md) ----
        // The runtime creates its own build pass lazily on the first frame after `create_passes()` (the shader
        // cannot be registered before `setup_pipeline()` runs, because that is this function), and that pass asks
        // for the module through `pass_context::shader` - so loading and registering it here is the whole host-side
        // contract. THE SAME GATE THE RUNTIME READS (`DEREN_HZB_BUILD`, default off) also guards the LOAD, so an OFF
        // run does not even read the file and its log carries no HZB line: "gate off means the frame, the
        // allocations AND the log are today's" is the strict form this lane is accepted under.
        if (std::getenv("DEREN_HZB_BUILD") != nullptr) {
            deren::utility::vessel::vector<std::uint8_t> hzb_build_code;
            load_shader(shaders_dir, "hzb_build.comp.spv", hzb_build_code);
            runtime.register_shader("hzb_build.comp.spv", hzb_build_code);
        }
        // ---- THE HZB CULL MODULE (M3 phase B1, docs/superpowers/specs/2026-10-20-hzb-occlusion-design.md) ----
        // The runtime creates its own cull pass lazily, exactly as it does the build pass, and asks for the module
        // through `pass_context::shader` - so loading and registering it here is the whole host-side contract. THE
        // SAME TWO GATES THE RUNTIME READS guard the load (the cull lane requires the build lane: it tests against
        // the history pyramid the build lane produces), so an A2-only or OFF run does not even read the file.
        if (std::getenv("DEREN_HZB_BUILD") != nullptr && std::getenv("DEREN_HZB_CULL") != nullptr) {
            deren::utility::vessel::vector<std::uint8_t> hzb_cull_code;
            load_shader(shaders_dir, "hzb_cull.comp.spv", hzb_cull_code);
            runtime.register_shader("hzb_cull.comp.spv", hzb_cull_code);
        }

        {
            // THE TOON CHARACTER STAGE'S PIPELINE, and it is registered through a DIFFERENT entry point on
            // purpose: `runtime::make_pipeline` above builds the forward family's pipeline (swapchain format,
            // src-alpha blending, depth LESS_OR_EQUAL), and this stage needs the opposite of all three - one
            // HDR target, no blending, depth compare EQUAL - so it goes through
            // `runtime::make_character_forward_pipeline` (see `core::make_character_forward_pipeline` for why
            // each of the three is forced).
            //
            // THE GEOMETRY IS pbr's, unchanged: it is the same leaves drawn a second time, so the varying
            // block the fragment stage reads (location 0/1/2 = world position, normal, uv) is the one
            // gbuffer.slang already reads, and the two passes cannot disagree about a surface's shape.
            //
            // The name is the one the character-forward pass binds through `render_environment::default_name`;
            // the pass is handed it rather than hardcoding it (see pass::character_forward_frame).
            deren::utility::vessel::vector<std::uint8_t> character_fragment_code;
            deren::utility::vessel::vector<std::uint8_t> character_mesh_code;
            deren::utility::vessel::vector<std::uint8_t> character_meshlet_code;
            load_shader(shaders_dir, "character_forward.frag.spv", character_fragment_code);
            load_shader(shaders_dir, "pbr.mesh.spv", character_mesh_code);
            load_shader(shaders_dir, "pbr.meshlet.spv", character_meshlet_code);
            runtime.register_shader("character_forward.frag.spv", character_fragment_code);
            auto const character_result = runtime.make_character_forward_pipeline("character_forward", character_fragment_code, character_mesh_code, character_meshlet_code);
            if (!character_result) {
                deren::utility::log("WARNING: the character-forward pipeline was not created ({}); the toon character stage will draw nothing", character_result.error());
            }
        }

        {
            // THE GAME-SHADER TOON MODEL'S PIPELINE (`game_toon.slang`), the THIRD shading model for the
            // SAME character-forward pass. Same reasoning as the goo block above: the same geometry, the
            // same pass, the same frame and the same forced state; only the fragment module differs.
            // `[render] game_toon` selects it and it is OFF by default, so with the flag off this block
            // adds one startup pipeline and no pixel.
            //
            // PHASE 1 IS A PLUMBING MILESTONE: the shader still calls the shared body, so a run with
            // game_toon on and goo_toon off reproduces the old chain's pixels until the cloth port lands.
            //
            // A FAILURE IS NOT FATAL, like goo_toon's: `make_character_forward_frame` falls back to the
            // old pipeline name and `set_game_toon` reports the fallback once, when the knob is on.
            deren::utility::vessel::vector<std::uint8_t> game_fragment_code;
            deren::utility::vessel::vector<std::uint8_t> game_mesh_code;
            deren::utility::vessel::vector<std::uint8_t> game_meshlet_code;
            load_shader(shaders_dir, "game_toon.frag.spv", game_fragment_code);
            load_shader(shaders_dir, "pbr.mesh.spv", game_mesh_code);
            load_shader(shaders_dir, "pbr.meshlet.spv", game_meshlet_code);
            runtime.register_shader("game_toon.frag.spv", game_fragment_code);
            auto const game_result = runtime.make_character_forward_pipeline("game_toon", game_fragment_code, game_mesh_code, game_meshlet_code);
            if (!game_result) {
                deren::utility::log("WARNING: the game toon pipeline was not created ({}); [render] game_toon will draw the previous chain", game_result.error());
            }
        }

        {
            // THE OVERLAY CHANNEL'S PIPELINE (`overlay.slang`): the article's two framebuffer multiplies, drawn by
            // the character-forward stage's overlay group. It is registered through a THIRD entry point because
            // its states are a third set again - ONE HDR target (like the toon stage), `dst = src * dst`
            // (like nothing else in the renderer) and depth compare LESS_OR_EQUAL (the toon stage's is EQUAL, and
            // an overlay mesh sits in FRONT of the surface it darkens, so EQUAL would reject it) - see
            // `core::make_overlay_pipeline`, which derives the blend from the article's `BlendOp Multiply` in core
            // Vulkan factors rather than reaching for a blend extension.
            //
            // THE GEOMETRY IS pbr's, unchanged, for the same reason the toon stage's is: an overlay is an ordinary
            // mesh (skinned and morphed like the face it is registered to), so the two passes cannot disagree about
            // a surface's shape.
            //
            // A FAILURE IS NOT FATAL, and that is a deliberate difference from the toon pipeline above: the pass
            // takes an EMPTY overlay name to mean "draw no overlay" (see character_forward_frame), so a device that
            // refuses this pipeline renders the character without its two masks - which is exactly the frame the
            // model had before they were merged into the asset - rather than drawing them as shaded quads.
            deren::utility::vessel::vector<std::uint8_t> overlay_fragment_code;
            deren::utility::vessel::vector<std::uint8_t> overlay_mesh_code;
            deren::utility::vessel::vector<std::uint8_t> overlay_meshlet_code;
            load_shader(shaders_dir, "overlay.frag.spv", overlay_fragment_code);
            load_shader(shaders_dir, "pbr.mesh.spv", overlay_mesh_code);
            load_shader(shaders_dir, "pbr.meshlet.spv", overlay_meshlet_code);
            runtime.register_shader("overlay.frag.spv", overlay_fragment_code);
            auto const overlay_result = runtime.make_overlay_pipeline("overlay", overlay_fragment_code, overlay_mesh_code, overlay_meshlet_code);
            if (!overlay_result) {
                deren::utility::log("WARNING: the overlay pipeline was not created ({}); the character's eye and hair shadows will not be multiplied", overlay_result.error());
            }
        }

        {
            // THE ARTICLE'S ① OUTLINE (INVERTED HULL) PIPELINE (`outline.slang` + `pbr.slang`'s outline mesh
            // entry), drawn by the character-forward stage's outline group. It goes through a FOURTH entry point
            // because its states are a fourth set: ONE HDR target (like the toon stage), an OPAQUE blend - the
            // article's `MyZmdOutlineShader` states no `Blend` at all - and depth compare LESS_OR_EQUAL (the toon
            // stage's is EQUAL, which would reject the whole outer ring, since a pushed-out hull is not the
            // surface the G-buffer recorded) - see `core::make_outline_pipeline`, which states each of those.
            //
            // THE GEOMETRY IS THE OUTLINE'S OWN MESH STAGE, not pbr's: pushing each vertex outward in clip space
            // is the one thing that turns a second copy of a mesh into an outline, and it lives in the
            // `outline_mesh_main` entry of `pbr.slang` (see `source/shaders/toon/outline.slang` for why the fragment entry is
            // `frag_main`).
            //
            // THERE IS NO MESHLET FORM, AND THAT IS DELIBERATE RATHER THAN AN OMISSION:
            // `build-release-clang64/shaders/outline.meshlet.spv` is residue from an earlier experiment that no
            // build rule produces (`outline_plan.md` §10), so wiring it would register a stage this build never
            // compiled. A leaf that would otherwise dispatch per meshlet falls back to the mesh form (see the
            // bind in `runtime.frames.cppm`), which costs throughput and not correctness.
            //
            // A FAILURE IS NOT FATAL, on the same terms as the overlay's: the pass takes an EMPTY outline name to
            // mean "draw no outline" (see character_forward_frame), so a device that refuses this pipeline
            // renders exactly the frame this port produced before ① landed - rather than drawing hulls with the
            // toon pipeline, which neither culls front faces nor tests with LESS_OR_EQUAL and would paint whole
            // shaded surfaces over the character.
            deren::utility::vessel::vector<std::uint8_t> outline_fragment_code;
            deren::utility::vessel::vector<std::uint8_t> outline_mesh_code;
            load_shader(shaders_dir, "outline.frag.spv", outline_fragment_code);
            load_shader(shaders_dir, "outline.mesh.spv", outline_mesh_code);
            runtime.register_shader("outline.frag.spv", outline_fragment_code);
            auto const outline_result = runtime.make_outline_pipeline("outline", outline_fragment_code, outline_mesh_code);
            if (!outline_result) {
                deren::utility::log("WARNING: the outline pipeline was not created ({}); the character will be drawn without its ① outline", outline_result.error());
            }
        }

        {
            // THE SCREEN-SPACE DEPTH RIM'S STAGE. Only the two shaders are registered here: the pass builds its
            // own pipeline in its `create` step (it is a fullscreen stage with one additive target and no depth
            // attachment, which is nothing `runtime::make_pipeline`'s forward family describes), so there is no
            // named pipeline to create and no registration path of its own - the same arrangement the deferred
            // and TAA stages have.
            deren::utility::vessel::vector<std::uint8_t> rim_fragment_code;
            load_shader(shaders_dir, "toon_screen_rim.frag.spv", rim_fragment_code);
            runtime.register_shader("toon_screen_rim.frag.spv", rim_fragment_code);
        }

        {
            // THE GAME CHAIN'S DEPTH RIM (`game_toon_depth_rim.slang`). Registered exactly like the article's
            // contour above and for the same reason: it is a fullscreen stage with ONE additive target and no depth
            // attachment, so its pass builds its own pipeline in `create` and there is no named pipeline here. The
            // two rims are alternatives at run time (the post-graph lane records this one only while
            // `game_toon_active()`), which is why both are registered unconditionally and neither is a fallback for
            // the other.
            deren::utility::vessel::vector<std::uint8_t> game_rim_fragment_code;
            load_shader(shaders_dir, "game_toon_depth_rim.frag.spv", game_rim_fragment_code);
            runtime.register_shader("game_toon_depth_rim.frag.spv", game_rim_fragment_code);
        }

        {
            // THE RESOLVE'S STAGE. Only the fragment shader is registered here, for the same reason as the rim's
            // above: the pass builds its own pipeline in its `create` step, from post.vert.spv (the synthetic
            // triangle, registered with the post chain below) and upscale.frag.spv - and that has to have happened
            // before create_passes(), which is where this registration sits.
            deren::utility::vessel::vector<std::uint8_t> upscale_fragment_code;
            load_shader(shaders_dir, "upscale.frag.spv", upscale_fragment_code);
            runtime.register_shader("upscale.frag.spv", upscale_fragment_code);
        }

        {
            // The post chain is a PASS PAIR now (deren.engine.pass.post): the composite owns the chain's two pipelines,
            // and the four bloom levels record with them. So the app REGISTERS the two shaders the pass builds
            // from (post.vert's synthetic triangle and post.frag, whose `mode` lane selects the stage) and the pass
            // does the rest in create_passes() below.
            //
            // THE SAMPLERS ARE THE DEVICE ROOT'S (`core::create_samplers`), so there is nothing to create here any
            // more - the app registers the two shaders the post chain's passes build from.
            deren::utility::vessel::vector<std::uint8_t> vertex_code;
            deren::utility::vessel::vector<std::uint8_t> fragment_code;
            load_shader(shaders_dir, "post.vert.spv", vertex_code);
            load_shader(shaders_dir, "post.frag.spv", fragment_code);
            runtime.register_shader("post.vert.spv", vertex_code);
            runtime.register_shader("post.frag.spv", fragment_code);
            // ... and FXAA's, whose pass builds its own pipeline from its fragment shader and post.vert - which is
            // why this registration sits HERE, before create_passes(), and not after it: the pass is created with
            // every other pass now, and the ordering constraint the old make_fxaa_pipeline() call needed is gone
            // with the call.
            load_shader(shaders_dir, "fxaa.frag.spv", fragment_code);
            runtime.register_shader("fxaa.frag.spv", fragment_code);
        }

        {
            // The shadow pass is a PASS (deren.engine.pass.shadow): the app REGISTERS its two shaders and the pass builds
            // the depth-only pipeline itself, from them and the context's depth format - which is why there is no
            // make_* here any more. Optional: without the pipeline the scene simply renders without shadows.
            deren::utility::vessel::vector<std::uint8_t> fragment_code;
            deren::utility::vessel::vector<std::uint8_t> mesh_code;
            load_shader(shaders_dir, "shadow.frag.spv", fragment_code);
            runtime.register_shader("shadow.frag.spv", fragment_code);
            // ... and the MESH stage, which is the pass's ONLY geometry stage since step 4 (docs/mesh_shaders.md):
            // `shadow.vert.spv` is neither compiled nor registered any more, so the two below are what the pass
            // builds from.
            load_shader(shaders_dir, "shadow.mesh.spv", mesh_code);
            runtime.register_shader("shadow.mesh.spv", mesh_code);
            // ... and the MESHLET form of the same pass (docs/mesh_shaders.md step 3): one workgroup per meshlet.
            load_shader(shaders_dir, "shadow.meshlet.spv", mesh_code);
            runtime.register_shader("shadow.meshlet.spv", mesh_code);
        }

        {
            // Clustered light culling compute pass (M5): one dispatch per frame sorts the punctual
            // lights into the screen-tile x depth-slice grid the shading stage then reads. Optional -
            // without it (or with [render] clustered_lights = false) shade_surface() loops every
            // active light, which is the brute-force reference the clustered path is verified on.
            // IT IS A PASS: the app registers the shader and the pass builds its own compute pipeline from it (see
            // deren.engine.pass.cluster) - create_passes() below runs that step, and
            // the pass logs its own outcome. It has to be registered HERE, before that call, because a pass
            // created before its shader exists builds nothing and says so.
            deren::utility::vessel::vector<std::uint8_t> compute_code;
            load_shader(shaders_dir, "light_cluster.comp.spv", compute_code);
            runtime.register_shader("light_cluster.comp.spv", compute_code);
        }

        {
            // G-buffer pair (the deferred path's first half): the surface-writing pipeline the
            // opaque pass binds when it writes the G-buffer, and the fullscreen debug view that
            // turns one stored channel into a visible image. Both optional - without them
            // runtime::set_gbuffer_debug() has no effect and the opaque pass shades into the HDR target directly.
            deren::utility::vessel::vector<std::uint8_t> fragment_code;
            deren::utility::vessel::vector<std::uint8_t> mesh_code;
            load_shader(shaders_dir, "pbr.mesh.spv", mesh_code); // the G-buffer pass's geometry stage, and the only one it has
            load_shader(shaders_dir, "gbuffer.frag.spv", fragment_code);
            // ... and the MESHLET form (docs/mesh_shaders.md step 3): one workgroup per meshlet, camera-culled. The
            // runtime prefers it and falls back to the mesh form, which is now the pass's REQUIREMENT - its vertex
            // form went with the rest of the vertex geometry path (step 4).
            deren::utility::vessel::vector<std::uint8_t> meshlet_code;
            load_shader(shaders_dir, "pbr.meshlet.spv", meshlet_code);
            auto const gbuffer_result = runtime.make_gbuffer_pipeline(fragment_code, mesh_code, meshlet_code);

            if (!gbuffer_result) {
                deren::utility::log("gbuffer pipeline disabled: {}", gbuffer_result.error());
            } else {
                // The debug view is a PASS (deren.engine.pass.geometry_buffer_debug): the app registers its two shaders and the
                // pass builds its pipeline, which is all it owns. The samplers those declarations
                // choose between are the device root's now (`core::create_samplers`).
                // ... AND THIS IS `post.vert.spv`, NOT A GEOMETRY STAGE: a synthetic fullscreen triangle, which is a
                // vertex stage by nature and stays one (docs/mesh_shaders.md step 4 is about the geometry path).
                deren::utility::vessel::vector<std::uint8_t> vertex_code;
                load_shader(shaders_dir, "post.vert.spv", vertex_code);
                load_shader(shaders_dir, "gbuffer_debug.frag.spv", fragment_code);
                runtime.register_shader("post.vert.spv", vertex_code);
                runtime.register_shader("gbuffer_debug.frag.spv", fragment_code);
                // The deferred lighting stage is a PASS (deren.engine.pass.deferred): the app REGISTERS the two
                // shaders it builds from and the pass builds its own pipeline from them, which is why one
                // register call for each replaces the old `make_deferred_pipeline(vertex_code, fragment_code)`
                // call here.
                load_shader(shaders_dir, "post.vert.spv", vertex_code);
                load_shader(shaders_dir, "deferred.frag.spv", fragment_code);
                runtime.register_shader("post.vert.spv", vertex_code);
                runtime.register_shader("deferred.frag.spv", fragment_code);
                // TAA resolve (deferred-only). IT IS A PASS TOO: the app registers the two shaders and the
                // pass builds its own pipeline from them (see deren.engine.pass.taa) - the create_passes() call
                // below is what runs that step, for every pass at once. The vertex
                // stage is post.vert's synthetic triangle, the same one the debug view and the post chain use.
                //
                // NOTE WHERE THESE TWO REGISTRATIONS SIT: they used to be in the ELSE branch of the deferred
                // pipeline's creation, so a machine where that failed silently lost TAA as well. They are
                // gated on the DEBUG pipeline now, which is what they actually need (post.vert), and the
                // lighting stage's own shaders are registered above it.
                load_shader(shaders_dir, "taa.frag.spv", fragment_code);
                runtime.register_shader("taa.frag.spv", fragment_code);
                deren::utility::log("SUCCESS: gbuffer pipelines created (surface write, debug view)");
            }
        }

        {

            // Ray-traced sun shadows: one ray per pixel against the scene's acceleration structures. IT IS A
            // PASS, so its shaders have to be registered BEFORE create_passes() below - the pass builds its own
            // ray-tracing pipeline from them (see deren.engine.pass.ray_traced_shadow), and a pass created
            // before its shaders exist builds nothing and says so. Optional, and the builder refuses on a device
            // without a ray-tracing pipeline: without it the cascaded shadow maps keep running.
            deren::utility::vessel::vector<std::uint8_t> rt_shadow_raygen_code;
            load_shader(shaders_dir, "rt_shadow.rgen.spv", rt_shadow_raygen_code);
            runtime.register_shader("rt_shadow.rgen.spv", rt_shadow_raygen_code);
            deren::utility::vessel::vector<std::uint8_t> rt_shadow_closest_hit_code;
            load_shader(shaders_dir, "rt_shadow.rchit.spv", rt_shadow_closest_hit_code);
            runtime.register_shader("rt_shadow.rchit.spv", rt_shadow_closest_hit_code);
            deren::utility::vessel::vector<std::uint8_t> rt_shadow_miss_code;
            load_shader(shaders_dir, "rt_shadow.rmiss.spv", rt_shadow_miss_code);
            runtime.register_shader("rt_shadow.rmiss.spv", rt_shadow_miss_code);
            // The any-hit stage: the second stage of the SAME hit group, and the only place an alphaMode MASK
            // surface can be told apart from its bounding triangles (see source/shaders/rt_shadow.rahit).
            deren::utility::vessel::vector<std::uint8_t> rt_shadow_any_hit_code;
            load_shader(shaders_dir, "rt_shadow.rahit.spv", rt_shadow_any_hit_code);
            runtime.register_shader("rt_shadow.rahit.spv", rt_shadow_any_hit_code);

            // Ray-traced reflections: one reflection ray per pixel into the per-frame-slot target the deferred
            // lighting stage samples. Also a PASS, registered before create_passes() for the same reason, and
            // optional: on a device without a ray-tracing pipeline the deferred stage adds no traced reflection
            // term at all.
            deren::utility::vessel::vector<std::uint8_t> rt_reflection_raygen_code;
            load_shader(shaders_dir, "rt_reflection.rgen.spv", rt_reflection_raygen_code);
            runtime.register_shader("rt_reflection.rgen.spv", rt_reflection_raygen_code);
            deren::utility::vessel::vector<std::uint8_t> rt_reflection_closest_hit_code;
            load_shader(shaders_dir, "rt_reflection.rchit.spv", rt_reflection_closest_hit_code);
            runtime.register_shader("rt_reflection.rchit.spv", rt_reflection_closest_hit_code);
            // The reflection's alpha-mask any-hit: the second stage of the same hit group, so a masked surface
            // reflects its cut-out shape instead of its bounding triangles.
            deren::utility::vessel::vector<std::uint8_t> rt_reflection_any_hit_code;
            load_shader(shaders_dir, "rt_reflection.rahit.spv", rt_reflection_any_hit_code);
            runtime.register_shader("rt_reflection.rahit.spv", rt_reflection_any_hit_code);
            deren::utility::vessel::vector<std::uint8_t> rt_reflection_miss_code;
            load_shader(shaders_dir, "rt_reflection.rmiss.spv", rt_reflection_miss_code);
            runtime.register_shader("rt_reflection.rmiss.spv", rt_reflection_miss_code);

            // The AMD FidelityFX reflection denoiser stages. Registered unconditionally with the other shaders;
            // the passes only run when [render] rt_reflections is on, and each fails to create - leaving its
            // pointer null - when the device or the SPIR-V cannot support the chain.
            for (std::string_view const denoise_name : {"reflection_reproject.comp.spv", "reflection_prefilter.comp.spv", "reflection_resolve.comp.spv", "reflection_copy_history.comp.spv"}) {
                deren::utility::vessel::vector<std::uint8_t> denoise_code;
                load_shader(shaders_dir, denoise_name, denoise_code);
                runtime.register_shader(denoise_name, denoise_code);
            }

            // The alphaMode MASK bake (source/shaders/geometry/mask_bake.slang) and the compute skinning job
            // (source/shaders/geometry/compute_skin.slang) are TWO JOBS rather than frame passes - one runs once inside the
            // command buffer that builds the bottom level structures (without it a MASK surface is solid to
            // a ray), the other re-skins the casters and refits their structures per frame (without it a
            // skinned caster's traced shadow is cast by its BIND POSE). Both are built by `create_passes()`
            // below, from the SAME context and the same resource channel every pass gets (see
            // deren.engine.pass.mask_bake and deren.engine.pass.compute_skin), so their shaders are registered here too.
            deren::utility::vessel::vector<std::uint8_t> mask_bake_code;
            load_shader(shaders_dir, "mask_bake.comp.spv", mask_bake_code);
            runtime.register_shader("mask_bake.comp.spv", mask_bake_code);
            deren::utility::vessel::vector<std::uint8_t> compute_skin_code;
            load_shader(shaders_dir, "compute_skin.comp.spv", compute_skin_code);
            runtime.register_shader("compute_skin.comp.spv", compute_skin_code);
            deren::utility::vessel::vector<std::uint8_t> subtilis_cut_code;
            load_shader(shaders_dir, "subtilis_cut.comp.spv", subtilis_cut_code);
            runtime.register_shader("subtilis_cut.comp.spv", subtilis_cut_code);

            // The HEAP-NATIVE PROBE (source/shaders/debug/heap_probe_comp.slang): registered like the jobs above, and for a purpose
            // of the same kind - it is not part of any frame, it runs once at scene setup in a command buffer of
            // its own (runtime::run_heap_probe) and its answer is a log line. It exists because the migration's
            // four assumptions about the native path (a heap-flagged pipeline with NO layout, `descriptor_heap`
            // declarations, a sampler taken from the sampler heap, parameters through vkCmdPushDataEXT) cannot be
            // tested by a picture until the whole frame is converted.
            deren::utility::vessel::vector<std::uint8_t> heap_probe_code;
            load_shader(shaders_dir, "heap_probe.comp.spv", heap_probe_code);
            runtime.register_shader("heap_probe.comp.spv", heap_probe_code);
            // ... and its GRAPHICS half: the same read through a graphics pipeline, which is a different question
            // (a fragment stage reading the heap, and a pipeline created with the flag and no layout).
            deren::utility::vessel::vector<std::uint8_t> gaussian_vertex_code;
            deren::utility::vessel::vector<std::uint8_t> gaussian_fragment_code;
            load_shader(shaders_dir, "gaussian_raster.vert.spv", gaussian_vertex_code);
            load_shader(shaders_dir, "gaussian_raster.frag.spv", gaussian_fragment_code);
            runtime.register_shader("gaussian_raster.vert.spv", gaussian_vertex_code);
            runtime.register_shader("gaussian_raster.frag.spv", gaussian_fragment_code);
            deren::utility::vessel::array<deren::utility::vessel::vector<std::uint8_t>, 3> gaussian_compute_code;
            deren::utility::vessel::array<std::string_view, 3> const gaussian_compute_names = {"gaussian_reset.comp.spv", "gaussian_prepare.comp.spv", "gaussian_finalize.comp.spv"};
            for (std::size_t i = 0; i < gaussian_compute_names.size(); ++i) {
                load_shader(shaders_dir, gaussian_compute_names[i].data(), gaussian_compute_code[i]);
                runtime.register_shader(gaussian_compute_names[i], gaussian_compute_code[i]);
            }
            if (auto const* enabled = std::getenv("DEREN_GPU_SORT_PROBE"); enabled != nullptr && std::string_view{enabled} == "1") {
                auto report = deren::engine::gaussian_splatting::run_gpu_sort_probe(runtime.rhi_face());
                if (report) {
                    deren::utility::log("GPU sort probe: PASS {} cases", *report);
                } else {
                    deren::utility::log("GPU sort probe: FAIL {}", report.error().message);
                }
            }
            if (auto const* enabled = std::getenv("DEREN_GAUSSIAN_PROBE"); enabled != nullptr && std::string_view{enabled} == "1") {
                auto report = deren::engine::gaussian_splatting::run_raster_probe(runtime.rhi_face(), gaussian_vertex_code, gaussian_fragment_code, {}, false, runtime.device_root());
                if (report) {
                    deren::utility::log("Gaussian raster probe: PASS {} cases / {} pixels, maximum RGB error {}", report->cases, report->pixels, report->maximum_error);
                } else {
                    deren::utility::log("Gaussian raster probe: FAIL {}", report.error().message);
                }
            }
            if (auto const* enabled = std::getenv("DEREN_GAUSSIAN_GPU_PROBE"); enabled != nullptr && std::string_view{enabled} == "1") {
                deren::utility::vessel::array<std::span<std::uint8_t const>, 3> const compute = {gaussian_compute_code[0], gaussian_compute_code[1], gaussian_compute_code[2]};
                auto report = deren::engine::gaussian_splatting::run_raster_probe(runtime.rhi_face(), gaussian_vertex_code, gaussian_fragment_code, compute, true, runtime.device_root());
                if (report) {
                    deren::utility::log("Gaussian GPU raster probe: PASS {} cases / {} pixels, maximum RGB error {}, depth max {} ULP, double-order differences {}", report->cases, report->pixels, report->maximum_error, report->maximum_depth_ulp, report->depth_order_differences);
                } else {
                    deren::utility::log("Gaussian GPU raster probe: FAIL {}", report.error().message);
                }
            }
            deren::utility::vessel::vector<std::uint8_t> heap_probe_vertex_code;
            load_shader(shaders_dir, "heap_probe.vert.spv", heap_probe_vertex_code);
            runtime.register_shader("heap_probe.vert.spv", heap_probe_vertex_code);
            deren::utility::vessel::vector<std::uint8_t> heap_probe_fragment_code;
            load_shader(shaders_dir, "heap_probe.frag.spv", heap_probe_fragment_code);
            runtime.register_shader("heap_probe.frag.spv", heap_probe_fragment_code);
            // ... and its MESH half (docs/mesh_shaders.md step 0): the same triangle, emitted by a mesh stage
            // through a heap-native pipeline. A mesh stage is a different stage type, so this binary has a name
            // of its own rather than replacing the vertex one.
            deren::utility::vessel::vector<std::uint8_t> heap_probe_mesh_code;
            load_shader(shaders_dir, "heap_probe.mesh.spv", heap_probe_mesh_code);
            runtime.register_shader("heap_probe.mesh.spv", heap_probe_mesh_code);

            // ... and now that every pass's and every job's shaders are registered, the pipelines can be built.
            // This block is where the SHADERS come from and nothing else: the ONE create step that builds what the
            // passes and the jobs own (a pipeline each) runs in main(), after the application has handed its chain
            // over (`render_start_demo`) - see `runtime::create_passes`, which logs each object's own outcome.
        }
    }
} // namespace deren::app
