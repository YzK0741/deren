// The demo's implementation: the two callbacks the runtime asks for, and nothing else. Every branch below is a
// STAGE of this application's frame - the same names the runtime's frame loop uses for its stage structs - and what
// it does is hand that stage's passes the frames only the runtime can build (its own recording machinery, and the
// leaf lists/camera it owns). The per-stage ORDERING RULES that used to run here are the composed graph's now: it
// declares those image hand-offs, so this preamble no longer touches an image.

module;

#include <cstddef>
#include <glm/glm.hpp>
#include <string_view>
// NO VULKAN HEADER: this demo hands passes their frames and runs the frame's ordering rules - every value it
// touches is the contract's or the renderer's own.

module deren.engine.render_start_demo;

import deren.utility;
import deren.engine.pass.visibility_raster;  // the visibility buffer's raster pass
import deren.engine.pass.visibility_resolve; // ... and its consumer, the compute resolve

namespace deren::engine {

    std::size_t render_start_demo::attach(runtime& self) noexcept {
        this->runtime_owner = &self;
        // ---- THE CHAIN, CONSTRUCTED HERE ----
        // The same passes the renderer used to construct for itself, in the order their create step must run in (the
        // chain's order IS that order). THE G-BUFFER DEBUG VIEW COMES FIRST, which used to be a CREATE-ORDER
        // constraint - the passes that read the G-buffer asked the owner for the G-buffer set's layout while they
        // were being created. That is gone with the layouts: a pass builds only its own pipeline now.
        this->chain.emplace<pass::scene_constants_pass>();
        this->chain.emplace<pass::scene_deformation_pass>();
        this->chain.emplace<pass::scene_tables_pass>();
        this->chain.emplace<pass::output_targets_pass>();
        this->chain.emplace<pass::gbuffer_debug_pass>();
        this->chain.emplace<pass::shadow_pass>();
        this->chain.emplace<pass::visibility_raster_pass>();
        // ... and the raster lane's DECLARED CONSUMER: the compute resolve that rebuilds the five G-buffer targets
        // from the ids (v0.4c). It is in the chain only so its pipeline is built at startup; whether a frame records
        // it is the graph lane's decision (and the frame's fallback), exactly like the raster pass above.
        this->chain.emplace<pass::visibility_resolve_pass>();
        this->chain.emplace<pass::scene_pass>();
        this->chain.emplace<pass::transparent_pass>();
        // ... AND THE GAUSSIAN SPLAT CLOUD, whose create step only builds its pipeline (it owns no descriptor
        // family of its own): it records between the lighting stage and the blended geometry, and draws nothing
        // at all until the app hands the runtime a cloud to draw.
        this->chain.emplace<pass::gaussian_splatting_pass>();
        // ... and the toon character stage, right after the blended geometry: it re-shades the OPAQUE leaves
        // over the lit frame, at depth-EQUAL, so it must come after the lighting stage that produced what it
        // overwrites and after the blends that composite over the same pixels.
        this->chain.emplace<pass::character_forward_pass>();
        // ... and the toon stage's SECOND rim: a fullscreen additive contour from the depth, right after the
        // surface it outlines and before the resolve, so the anti-aliasing sees it in the frame it belongs to.
        this->chain.emplace<pass::toon_screen_rim_pass>();
        // ... and the REWRITTEN chain's rim, in the SAME slot of the chain's order (right after the surface stage
        // it outlines, before the resolve) - the two are alternatives at run time, and their order only matters on
        // a frame neither records, where it does not.
        this->chain.emplace<pass::goo_rim_pass>();
        this->chain.emplace<pass::taa_pass>();
        this->chain.emplace<pass::rt_shadow_pass>();
        this->chain.emplace<pass::cluster_pass>();
        this->chain.emplace<pass::deferred_pass>();
        this->chain.emplace<pass::post_composite_pass>();
        // ... the bloom chain: FOUR instances of ONE class, one per level. The LEVEL is what differs - the target it
        // writes, the transition it declares, its extent and the `mode` lane of its push block - and the order IS
        // the chain: each level reads the one before it.
        this->chain.emplace<pass::post_bloom_pass>(0u);
        this->chain.emplace<pass::post_bloom_pass>(1u);
        this->chain.emplace<pass::post_bloom_pass>(2u);
        this->chain.emplace<pass::post_bloom_pass>(3u);
        this->chain.emplace<pass::fxaa_pass>();
        // ... and the RESOLVE, after it: the render chain's display-referred image onto the presented swapchain.
        // It is the frame's LAST writer on the frames the chain runs below the output size, and it is mutually
        // exclusive with the FXAA pass above (see runtime::post_fxaa_active), so the two are alternatives in the
        // frame loop rather than a stack.
        this->chain.emplace<pass::upscale_pass>();
        this->passes = &this->chain;
        // LOOKED UP BY THE NAME THE DECLARATION CARRIES, which is the only key a chain gives: a cast is what turns
        // the declaration's owner into the type whose frame it wants. A pass this build does not have (its
        // declaration missing, or a variant of this app) stays null and is simply never fed.
        this->cluster = this->find<pass::cluster_pass>("cluster");
        this->shadow = this->find<pass::shadow_pass>("shadow");
        this->scene = this->find<pass::scene_pass>("scene");
        this->transparent = this->find<pass::transparent_pass>("transparent");
        this->gaussian = this->find<pass::gaussian_splatting_pass>("gaussian");
        this->character_forward = this->find<pass::character_forward_pass>("character_forward");
        this->toon_screen_rim = this->find<pass::toon_screen_rim_pass>("toon_screen_rim");
        this->goo_rim = this->find<pass::goo_rim_pass>("goo_rim");
        this->rt_shadow = this->find<pass::rt_shadow_pass>("rt_shadow");
        this->deferred = this->find<pass::deferred_pass>("deferred");
        this->taa = this->find<pass::taa_pass>("taa");
        this->gbuffer_debug_pass = this->find<pass::gbuffer_debug_pass>("gbuffer-debug");
        this->composite = this->find<pass::post_composite_pass>("post_composite");
        this->fxaa = this->find<pass::fxaa_pass>("fxaa");
        this->upscale = this->find<pass::upscale_pass>("upscale");

        std::size_t found = 0;
        found += this->cluster != nullptr ? 1u : 0u;
        found += this->shadow != nullptr ? 1u : 0u;
        found += this->scene != nullptr ? 1u : 0u;
        found += this->transparent != nullptr ? 1u : 0u;
        found += this->gaussian != nullptr ? 1u : 0u;
        found += this->character_forward != nullptr ? 1u : 0u;
        found += this->rt_shadow != nullptr ? 1u : 0u;
        found += this->deferred != nullptr ? 1u : 0u;
        found += this->taa != nullptr ? 1u : 0u;
        found += this->gbuffer_debug_pass != nullptr ? 1u : 0u;
        found += this->composite != nullptr ? 1u : 0u;
        found += this->fxaa != nullptr ? 1u : 0u;
        if (found != 16) {
            // NOT a fatal error: this demo is one application's chain, and a build of it that lacks a pass (a
            // shader that did not compile is the usual reason - that pass's own create step says why) renders
            // without it. Saying so once at startup is what keeps "the pass did not run" from looking like a
            // rendering bug. NO FORMAT ARGUMENT, and that is not a style choice: `deren::utility::log` with one crashes
            // THIS translation unit's code generation (clang 22.1.8, `EmitBuiltinNewDeleteCall` - a toolchain bug
            // this branch has seen twice).
            deren::utility::log("render_start_demo: a pass this demo wires is missing from the chain - it is not fed a frame and will not record");
        }
        // ---- AND HAND IT OVER ----
        // `set_pass_chain` binds this chain into the runtime's own frame structure (the stage sequence, the marks,
        // the renderer's work between the stages) and takes this demo's wiring for everything the runtime does not
        // know about those passes: their frames, the stage preambles, the results, the feature table.
        self.set_pass_chain(this->chain, this->wiring());
        // ---- THE TWO HOOKS THE PASSES CARRY, INSTALLED ONCE ----
        // NEITHER IS A PER-FRAME VALUE, which is why neither belongs in a frame any more: the overlay's draw is a
        // property of this renderer and of WHICH pass is the frame's last writer (the composite and the FXAA pass
        // both hold it, and each frame's own decision says which of them uses it), and the reflection's recording
        // is this application's second signal through the temporal pass's one pipeline. `attach` is where both are
        // known, so they are set here and the frame loop never writes into a pass's frame again.
        if (this->composite != nullptr) {
            this->composite->set_overlay(self.overlay_draw());
        }
        if (this->fxaa != nullptr) {
            this->fxaa->set_overlay(self.overlay_draw());
        }
        // ... and the resolve holds it too: on a frame the render chain is smaller than the output, THIS is the
        // frame's last writer, so the overlay has to be drawn inside its instance (at the OUTPUT extent - the
        // other reason it belongs here and not in the composite, whose instance covers the render extent).
        if (this->upscale != nullptr) {
            this->upscale->set_overlay(self.overlay_draw());
        }
        return found;
    }

    void render_start_demo::prepare(void* const owner, runtime::frame_services const& services, std::string_view const stage) {
        render_start_demo& self = *static_cast<render_start_demo*>(owner);
        // THE FLAT RENDER MODE is the one fact the renderer's policy reads about a pass it no longer holds: a
        // screen-space effect is pointless on such a frame. Published per stage because it is one bool the app
        // can flip between frames.
        if (self.runtime_owner != nullptr && self.deferred != nullptr) {
            self.runtime_owner->set_scene_unlit(self.deferred->unlit());
        }
        // THE FRAMES ONLY THE RUNTIME CAN BUILD are handed over here (see frame_services): the scene, shadow and
        // transparent frames carry the runtime's own recording machinery (per-slot secondaries, per-cascade
        // secondaries, the draw-state factory), and the gaussian and character frames carry the leaf lists and the
        // camera the runtime owns. The ORDERING RULES those stages used to run in this preamble - whose first
        // sample publishes an image - are the composed graph's now (it declares the hand-offs), so nothing here
        // touches an image. Every other frame is the pass's own (see frame_pass::prepare_frame).
        if (stage == "shadow") {
            if (self.shadow != nullptr) {
                self.shadow->set_frame(services.make_shadow_frame(services.owner));
            }
        } else if (stage == "scene") {
            if (self.scene != nullptr) {
                self.scene->set_frame(services.make_scene_frame(services.owner));
            }
        } else if (stage == "transparent") {
            if (self.transparent != nullptr) {
                self.transparent->set_frame(services.make_transparent_frame(services.owner));
            }
        } else if (stage == "gaussian") {
            if (self.gaussian != nullptr) {
                if (services.gaussian_timing)
                    self.gaussian->set_timing(services.gaussian_timing(services.owner));
                auto gpu = services.make_gaussian_gpu_frame ? services.make_gaussian_gpu_frame(services.owner, self.gaussian->gpu_ready()) : nullptr;
                if (gpu)
                    self.gaussian->set_gpu_frame(std::move(gpu));
                else
                    self.gaussian->set_frame(services.make_gaussian_frame(services.owner));
            }
        } else if (stage == "character_forward") {
            if (self.character_forward != nullptr) {
                self.character_forward->set_frame(services.make_character_forward_frame(services.owner));
            }
        }
    }

    void render_start_demo::collect(void* const owner, std::string_view const stage, runtime::frame_results& out) {
        render_start_demo& self = *static_cast<render_start_demo*>(owner);
        if (stage == "gaussian") {
            out.gaussian_record_error = self.gaussian ? self.gaussian->last_error() : deren::promise::rhi::error::ok;
            return;
        }
        if (stage == "taa") {
            // The camera UBO's `prev_view_proj` is only advanced when the resolve actually wrote a history: a
            // resolve that bailed out (no descriptor set) must not claim one.
            out.taa_wrote_history = self.taa != nullptr && self.taa->wrote_history();
        }
    }

    // =================================================================================================
    // THE APP'S KNOBS: the flag half goes to the runtime (its policy), the value half to the pass
    // =================================================================================================
    // The CLAMPS travel with the values (they are the same fact), which is why the argument for each of them now
    // lives in the pass that owns it rather than in the setter below.

    void render_start_demo::set_taa(bool const enabled, float const blend_static, float const blend_min) noexcept {
        bool const turned_on = this->runtime_owner != nullptr && this->runtime_owner->set_taa_enabled(enabled);
        if (this->taa == nullptr) {
            return;
        }
        this->taa->set_blend(blend_static, blend_min);
        if (turned_on) {
            // THE PASS's HALF OF THE OFF -> ON EDGE: whether each image's history holds anything is the pass's own
            // state, and the renderer's half (the matrix history and the jitter index) is what its setter just reset.
            this->taa->reset_history();
        }
    }

    void render_start_demo::set_gbuffer_channel(int32_t const channel) noexcept {
        if (this->gbuffer_debug_pass != nullptr) {
            this->gbuffer_debug_pass->set_channel(channel);
        }
    }

    void render_start_demo::set_unlit(bool const unlit) noexcept {
        if (this->deferred != nullptr) {
            this->deferred->set_unlit(unlit);
        }
        // ... and the renderer's own policy reads it (see set_scene_unlit): publish it here as well as per stage, so
        // a toggle that arrives between two frames is not one frame late.
        if (this->runtime_owner != nullptr) {
            this->runtime_owner->set_scene_unlit(unlit);
        }
    }

    void render_start_demo::set_upscale_filter(std::string_view const name) noexcept {
        // ONE MAPPING from the config's spelling to the pass's filter, here and nowhere else. EASU is the
        // fallback for an empty or unrecognised value, and the unrecognised case is LOGGED rather than silently
        // absorbed: a config key that appears to select a filter while the frame is produced by another one is
        // exactly the kind of dead setting this repository deletes instead of documenting.
        pass::upscale_filter filter = pass::upscale_filter::easu;
        if (name == "linear") {
            filter = pass::upscale_filter::linear;
        } else if (!name.empty() && name != "easu") {
            deren::utility::log("render_start_demo: [render] upscale = \"{}\" is not a filter this renderer has (linear, easu); using easu", name);
        }
        if (this->upscale != nullptr) {
            this->upscale->set_filter(filter);
        }
    }

    void render_start_demo::set_ssao(bool const enabled, float const radius, float const intensity, uint32_t const samples) noexcept {
        // CPU-side only (the same rule as the other render-mode knobs): the values are pushed with the lighting stage
        // each frame, so they are safe to change mid-run. THE VALUES AND THEIR CLAMPS ARE THE PASS'S (see
        // deferred_pass::set_ssao); the diagnostic below is about the SESSION rather than about the pass, which is
        // why it asks the runtime's registry and reports through the runtime's once-per-session logger.
        if (this->deferred != nullptr) {
            this->deferred->set_ssao(enabled, radius, intensity, samples);
        }
        if (enabled && this->runtime_owner != nullptr && !this->runtime_owner->feature_active("deferred")) {
            this->runtime_owner->warn_missing_feature("ssao", "screen-space AO has no effect: the G-buffer pass or its lighting stage was not created (see the startup log)");
        }
    }

    // =================================================================================================
    // THE FEATURE TABLE (moved out of runtime::active_features / feature_active / feature_available)
    // =================================================================================================
    // Every substitution below is the same one: `this-><runtime member>` became a FIELD of the facts the runtime
    // hands over, and `this->pass_ready("name")` became a question to the typed reference this demo found - so the
    // composition is unchanged and the two inputs it is made of now sit on their own sides of the seam.

    bool render_start_demo::feature_active(void* const owner, runtime::feature_facts const& facts, std::string_view const name) {
        render_start_demo& self = *static_cast<render_start_demo*>(owner);
        // the composed answers, each read by more than one branch below
        bool const gbuffer_debug = facts.gbuffer_debug && facts.gbuffer_pipeline && self.gbuffer_debug_pass != nullptr && self.gbuffer_debug_pass->ready();
        bool const shaded_scene = !gbuffer_debug && self.deferred != nullptr && self.deferred->ready() && facts.gbuffer_pipeline;
        // THE FLAT RENDER FLAG AND THE SSAO SWITCH LIVE IN THE LIGHTING PASS (they are its parameters), so the table
        // ASKS it - the same shape the other "ask the pass" answers below use. One copy of each
        // value, and the pass that pushes them is the one that owns them.
        bool const unlit = self.deferred != nullptr && self.deferred->unlit();

        // THE TWO GATES THAT USED TO BE THE FIRST LINE OF A RESOLVER. Both passes were "always active" before S3,
        // with the renderer's resolver returning false to skip them; the skip is the same, but the question now has
        // one name and one answer. What is NOT in these answers is the OTHER half of those old gates - whether this
        // frame's target generation exists: that is what the resource table already says, so a frame whose images
        // are not there fails the pass's own resolution. Two mechanisms, two questions.
        if (name == "scene") {
            return facts.gbuffer_pass;
        }
        if (name == "transparent") {
            return facts.transparent_pending;
        }
        if (name == "gaussian") {
            // THE SPLAT STAGE'S GATE, and it is deliberately the pass being ready AND a cloud being set: without
            // a cloud the stage would resolve its targets and record nothing, and the frozen capture scenarios
            // must not see even that. The asset half is the runtime's answer (`gaussian_active`), so the gate
            // cannot drift from what the frame's snapshot would contain.
            return self.gaussian != nullptr && self.gaussian->ready() && self.runtime_owner != nullptr && self.runtime_owner->gaussian_active();
        }
        if (name == "character_forward") {
            // THE RUNTIME'S COMPOSED PREDICATE (the knob AND this frame's opaque leaf list, see
            // feature_facts::character_forward_pending) AND the pass being ready. This ONE gate answers for the
            // SURFACE pass; the screen-space rim asks under its own name below, because the rewritten toon chain
            // must not wear that contour as well as the Goo rim (see `toon_screen_rim_pass::feature`).
            return facts.character_forward_pending && self.character_forward != nullptr && self.character_forward->ready();
        }
        if (name == "toon_screen_rim") {
            // THE SAME PREDICATE AS THE STAGE ABOVE PLUS "THE REWRITTEN CHAIN IS NOT THE ONE DRAWING" - and that
            // second half is the whole reason this is a name of its own.
            //
            // WHY THE REWRITTEN CHAIN MUST SILENCE IT: this pass is a fullscreen ADDITIVE contour (the article's
            // second rim, `toon_screen_rim.slang`), and the chain the rewrite follows has no such contour - its
            // screen-space piece is `DepthRim`, which step 2 defers. Left running with `goo_toon` on, the frame
            // would carry the article's contour AND the reference's rim, which is the same "two rims" defect
            // step 2 suppresses inside `toon_diffuse`, one stage over.
            //
            // `goo_toon_active()` IS THE RUNTIME'S OWN ANSWER to "is the rewritten chain drawing THIS frame",
            // and it is deliberately the same predicate `make_character_forward_frame` uses to pick the pipeline
            // name (`goo_toon_on && the pipeline exists`): the two must not be able to disagree, or a knob turned
            // on without the pipeline would silence the article's rim and draw nothing in its place.
            bool const goo_toon_active = self.runtime_owner != nullptr && self.runtime_owner->goo_toon_active();
            return facts.character_forward_pending && self.toon_screen_rim != nullptr && self.toon_screen_rim->ready() && !goo_toon_active;
        }
        if (name == "goo_rim") {
            // THE REWRITTEN CHAIN'S RIM, and it is the SAME predicate as the branch above with the sign of the
            // last term flipped - which is the whole reason the two are one name each rather than one name with a
            // mode: `feature_active` is asked once per NAME, so "the article's contour is off" and "the Goo rim is
            // on" have to be two answers that cannot drift apart. Both would be wrong the other way: a Goo rim
            // drawn over the old chain's frame would double the rim the old chain already computes inside
            // `toon_diffuse`, and an article contour left on under `goo_toon` is the same defect one stage over.
            //
            // IT IS ALSO THE GATE THE FRAME LOOP ASKS BEFORE RECORDING THE STAGE AT ALL (see runtime.frames.cppm),
            // so with `[render] goo_toon = false` the pass is not resolved, not recorded, and its stage preamble
            // does not publish the G-buffer - which is what makes every pre-existing capture scenario byte
            // identical rather than "identical because the shader wrote zero".
            bool const goo_toon_active = self.runtime_owner != nullptr && self.runtime_owner->goo_toon_active();
            return facts.character_forward_pending && self.goo_rim != nullptr && self.goo_rim->ready() && goo_toon_active;
        }
        if (name == "gbuffer-debug") {
            return gbuffer_debug;
        }
        if (name == "taa") {
            return facts.taa && self.taa != nullptr && self.taa->ready() && shaded_scene;
        }
        if (name == "fxaa") {
            return facts.fxaa;
        }
        if (name == "upscale") {
            // THE RUNTIME'S COMPOSED PREDICATE (`post_upscale_active`: the render chain is smaller than the
            // output AND the pass built its pipeline), relayed exactly as `fxaa` is - the runtime is what knows
            // the render scale and the pass registry, and this owner is what knows the chain. Nothing of the
            // pass's own is added here: its `ready()` half is already inside the fact, so the record step's gate and
            // the composite's target choice cannot disagree about whether the resolve runs.
            return facts.upscale;
        }
        if (name == "shadow") {
            // The shadow map is only read by the shading stages. The flat render mode samples nothing, so recording
            // the pass would be pure waste - it measured 0.22 ms of a 0.5 ms frame.
            return facts.shadow && self.shadow != nullptr && self.shadow->ready() && !unlit;
        }
        if (name == "rt_shadow") {
            // THE PASS'S GATE, in full: the knob and the extension PLUS "this frame's structure is built for the
            // slot". The second half is a FACT rather than a resource-table entry because an acceleration structure
            // is not a `resolved_binding` - it has a device address and no view, buffer or image.
            return facts.rt_shadow && self.rt_shadow != nullptr && self.rt_shadow->ready() && facts.structures_ready;
        }
        if (name == "clustered") {
            // Same argument as the shadow's: flat shading reads no light list, and with no active punctual light
            // there is nothing to sort in the first place.
            return facts.clustered && self.cluster != nullptr && self.cluster->ready() && facts.punctual_lights > 0.5f && !unlit;
        }
        if (name == "ssao") {
            return self.deferred != nullptr && self.deferred->ssao_enabled() && shaded_scene; // shader-side gate
        }
        if (name == "bloom") {
            return facts.bloom && self.composite != nullptr && self.composite->ready() && !gbuffer_debug;
        }
        if (name == "deferred") {
            // THE LIGHTING STAGE'S OWN GATE, and it is deliberately the SAME predicate the frame loop's branch uses:
            // the record step asks this before resolving the pass, and the frame loop asks it before recording the stage,
            // so one answer means the two cannot disagree about whether the lighting runs this frame.
            return facts.deferred_lit;
        }
        if (name == "unlit") {
            return unlit;
        }
        return false;
    }

    bool render_start_demo::feature_available(void* const owner, runtime::feature_facts const& facts, std::string_view const name) {
        // "CAN this feature run at all this session", which the overlay's menu and `log_feature_status()` ask - the
        // different question from `feature_active` above, and the one whose absent branch was a bound bug (the SSAO
        // group was never offered because `deferred` answered false).
        render_start_demo& self = *static_cast<render_start_demo*>(owner);
        if (name == "gbuffer-debug") {
            return facts.gbuffer_pipeline && self.gbuffer_debug_pass != nullptr && self.gbuffer_debug_pass->ready();
        }
        if (name == "deferred") {
            return self.deferred != nullptr && self.deferred->ready();
        }
        if (name == "taa") {
            return self.taa != nullptr && self.taa->ready();
        }
        if (name == "fxaa") {
            return self.fxaa != nullptr && self.fxaa->ready();
        }
        if (name == "character_forward") {
            // CAN it run at all, which is a different question from whether it is on: the overlay offers the
            // switch only when the chain has the pass AND the renderer registered the pipeline it binds. That
            // second half is the one that can be false - the pipeline needs the mesh stage, so a device without
            // VK_EXT_mesh_shader creates none - and an offered switch that would do nothing is exactly the bug
            // this function's header records.
            return self.character_forward != nullptr && self.character_forward->ready() && self.runtime_owner != nullptr && self.runtime_owner->character_forward_ready();
        }
        if (name == "shadow") {
            return self.shadow != nullptr && self.shadow->ready();
        }
        if (name == "clustered") {
            return self.cluster != nullptr && self.cluster->ready();
        }
        if (name == "visibility") {
            // CAN the visibility buffer run at all: both passes exist AND built their pipelines (the raster needs
            // VK_EXT_mesh_shader, and the resolve a compute pipeline the device refused only for a shader error).
            // The two typed pointers are the runtime's (this demo does not hold them): whether the path is ON is the
            // switch, and whether THIS frame takes it is the admission verdict - three questions, three answers.
            return self.runtime_owner != nullptr && self.runtime_owner->visibility_available();
        }
        return false;
    }

    // =================================================================================================

    void render_start_demo::recreated([[maybe_unused]] void* const owner) {
        // This demo keeps no descriptor family of its own: every pass in the chain retires its own, which the
        // record step does through recreate_stage, so there is nothing left here to reset.
    }

} // namespace deren::engine
