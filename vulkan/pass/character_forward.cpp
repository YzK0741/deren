// The character-forward pass's implementation: the two hand-off barriers, the LOAD instance, and the
// session state that makes `ZTest Equal` + `ZWrite Off` true rather than merely intended.
//
// MODELLED ON transparent.cpp AND DELIBERATELY SIMPLER: that pass records into a per-slot SECONDARY
// command buffer because the renderer fans its leaves out across the task pool, and this one draws
// directly into the primary. A character has hundreds of leaves, not the scene's thousands, and the
// secondary's inheritance plumbing (the heaps, the rendering info, the begin/end pair) buys nothing at
// that size. If the leaf count ever argues for it, this is the file that changes.

module;

#include <array>
#include <cstdint>
#include <span>
// NO VULKAN HEADER: every barrier, scope and handle in this file is the contract's now (the boundary
// sweep names the remaining files that still need one, and why).

module deren.vulkan.pass.character_forward;

import deren.promise.rhi; // the record series (abi 20): the barriers and the rendering scope
import deren.vulkan.render_resource;
import deren.utility;

namespace rhi = deren::promise::rhi;

namespace deren::vulkan::pass {

    render_resource::pass_io const& character_forward_pass::io() const noexcept {
        return render_resource::character_forward_io;
    }

    deren::vulkan::pass::behaviour const& character_forward_pass::behaviour() const noexcept {
        return pass_behaviour;
    }

    std::string_view character_forward_pass::feature() const noexcept {
        // THE RENDERER'S GATE, as a feature name (see the scene pass): a scene with no toon character is a
        // scene this pass is INACTIVE on - which is what the runner checks before it resolves anything, so
        // the pass neither records nor resolves and costs nothing on those frames. That is also what keeps
        // every capture-gate scenario byte-identical while the feature is off.
        //
        // THE OVERLAY GROUP RIDES THE SAME GATE, and that is a decision rather than an accident of sharing a
        // pass: an overlay mask is part of the article's toon character stage (it multiplies what THIS stage
        // wrote), so a frame that does not run the toon stage has nothing for a mask to modify. The
        // consequence a reader should know: with the feature off, the two mask meshes are drawn by NOTHING -
        // they are out of the opaque, transparent and shadow lists (see `frame_overlay`) and this pass does not
        // run - which at their default `_Color` of white is also what drawing them would have looked like.
        return "character_forward";
    }

    void character_forward_pass::create(pass_context const&) {
        // Nothing to build: no own set (every slot these leaves read comes from the frame's heaps) and no
        // pipeline (the renderer registers the named one, and the leaves bind it through `default_name`).
    }

    void character_forward_pass::on_swapchain_recreated(pass_host const&) {
        // Nothing is per-image here: the targets belong to the frame loop and the leaves own their buffers.
    }

    void character_forward_pass::set_frame(character_forward_frame const& frame) noexcept {
        this->pass_frame = frame;
    }

    void character_forward_pass::record(resolved_io const& io) {
        // BOTH LEAF LISTS ARE THE GATE, not just the toon one: a frame whose only character geometry is the
        // article's two masks still has something to multiply, and returning on `leaves.empty()` alone would
        // silently drop it. Either list being non-empty is the pass having work to do.
        if (this->pass_frame.make_environment == nullptr || this->pass_frame.pipeline_name.empty() || io.targets.size() < 2 || io.list == nullptr ||
            (this->pass_frame.leaves.empty() && this->pass_frame.overlay_leaves.empty() && this->pass_frame.outline_leaves.empty())) {
            return; // the runner resolves all of this or skips the pass (see make_character_forward_frame)
        }

        // TWO HAND-OFFS, and both are needed for the same reason the transparent pass needs them: the
        // lighting stage (or the transparent pass after it) left the depth in SHADER_READ_ONLY, so it has
        // to go back to the attachment layout before this instance can test against it; and the scene
        // colour's last store has to be published before this instance LOADs the same image, because
        // dynamic rendering inserts no dependency between two instances.
        // THE BATCH RIDES THE CONTRACT (abi 20): the same two pairs, in the same order, in ONE barrier call.
        std::array<rhi::image_barrier, 2> const barriers = {
            rhi::image_barrier{.resource = io.targets[1].image_handle,
                               .from = rhi::image_use::shader_read,
                               .to = rhi::image_use::depth_attachment,
                               .range = {}},
            rhi::image_barrier{.resource = io.targets[0].image_handle,
                               .from = rhi::image_use::color_attachment,
                               .to = rhi::image_use::color_attachment,
                               .range = {}},
        };
        if (io.list->barrier(rhi::barrier_group{.images = barriers}) != rhi::error::ok) {
            return; // a refused barrier would leave an attachment in a state nobody declared
        }

        // LOAD on both attachments: the lit frame and the opaque surface are what this pass draws OVER.
        // THE SCOPE RIDES THE CONTRACT TOO (abi 20): one colour attachment and the depth, both LOAD + STORE, and
        // no `secondary_contents` bit - this pass records straight into the primary, which is the whole point of
        // its own note (see the file's header).
        std::array<rhi::color_attachment, 1> const colors = {
            rhi::color_attachment{.view = io.targets[0].view_handle, .load = rhi::load_op::load, .store = rhi::store_op::store, .clear = {}},
        };
        rhi::depth_attachment const depth = {.view = io.targets[1].view_handle,
                                             .load = rhi::load_op::load, // make_load_depth_attachment_info's loadOp
                                             .store = rhi::store_op::store,
                                             .read_only = false,
                                             .has_stencil = false,
                                             .clear_depth = 1.0f,
                                             .clear_stencil = 0};
        rhi::rendering_info const rendering_info{
            .struct_size = sizeof(rhi::rendering_info),
            .area = {.offset_x = 0, .offset_y = 0, .width = this->pass_frame.extent.width, .height = this->pass_frame.extent.height},
            .layer_count = 1, // the raw make_rendering_info's layerCount, which is 1 at every site in this engine
            .colors = colors,
            .depth = depth,
            .has_depth = true,
            .secondary_contents = false, // the raw call passed no flags: this pass records into the primary
        };
        if (io.list->begin_rendering(rendering_info) != rhi::error::ok) {
            return; // a refused scope would leave the attachments in a state nobody declared
        }

        render_environment env = this->pass_frame.make_environment(this->pass_frame.owner, io.cmd, /*gbuffer=*/false);
        // THE TWO THINGS THIS PASS STATES ABOUT ITSELF, neither of which the renderer could know:
        //
        //   WHICH PIPELINE. Every primitive's draw() calls `bind_default()`, so a session's default name is
        //   what its leaves bind - there is no redirect flag and no per-leaf pipeline name to set. The
        //   renderer registered the character pipeline under `pipeline_name`; from here on the leaves are
        //   drawn by it.
        //
        //   DEPTH WRITE OFF, THEN LOCKED. The set comes first (the state starts "unknown", so it is really
        //   emitted), and the lock comes second because every primitive's draw() sets its OWN depth write a
        //   few instructions later - without the lock this pass's `ZWrite Off` would be a statement of
        //   intent that the very next leaf undoes. See render_environment::depth_write_locked.
        env.default_name = this->pass_frame.pipeline_name;
        env.set_depth_write(false);
        env.depth_write_locked = true;

        for (primitive const* const leaf : this->pass_frame.leaves) {
            leaf->draw(env);
        }

        // ---- AND THE OUTLINE GROUP, AFTER THE LEAVES AND BEFORE THE OVERLAYS ----
        //
        // THE ARTICLE'S ① 描边 (`MyZmdOutlineShader`): the character's own leaves drawn a second time as an
        // INVERTED HULL - front faces culled, vertices pushed outward in the VERTEX stage - so the only fragments
        // that survive the depth test are the ring just outside each silhouette. It is a third list for the same
        // reason `overlay_leaves` is a second one: its own pipeline (`outline_pipeline_name`, Cull Front +
        // LESS_OR_EQUAL), its own order, and its own per-material gate (the material's `_OutlineWidth`, which is 0
        // for a material the game itself says has no outline - chen's `cloth_02`).
        //
        // WHY IT IS BEFORE THE OVERLAYS: they multiply what this stage wrote, so the hull is part of what they
        // multiply. WHY THE DEPTH TEST IS ENOUGH: the leaves have just re-shaded the character onto the depth
        // this pass LOADed, so every hull fragment inside a silhouette fails LESS_OR_EQUAL against its own
        // surface and only the outside ring is left.
        //
        // WHAT IS NOT REPRODUCED, and it is the one state the article's shader states that this pass cannot: its
        // `ZWrite On`. Depth write is off and LOCKED for the whole instance (see above), which for a single hull
        // drawn with front-face culling is equivalent - the nearest back face wins the depth test either way - and
        // whose only visible consequence is that the hull does not occlude what is drawn after it (the overlays
        // and the post chain, neither of which is behind a silhouette).
        if (!this->pass_frame.outline_pipeline_name.empty() && !this->pass_frame.outline_leaves.empty()) {
            env.default_name = this->pass_frame.outline_pipeline_name;
            // CULL FRONT FOR THIS GROUP ONLY, and it has to be stated HERE rather than in the pipeline: every
            // leaf's draw() calls `set_cull_mode` with its own double-sided flag (see the field's own note), so a
            // pipeline-level Cull Front would be undone by the first hull. Cleared immediately after, so the
            // overlay group below records its own culling exactly as before.
            env.forced_cull_front = true;
            for (primitive const* const leaf : this->pass_frame.outline_leaves) {
                leaf->draw(env);
            }
            env.forced_cull_front = false;
        }

        // ---- AND THE OVERLAY GROUP, AFTER THEM AND WITH ITS OWN PIPELINE ----
        //
        // THE ORDER IS THE MECHANISM (see `character_forward_frame::overlay_leaves`): the multiply has to see
        // the toon-SHADED pixel, so it cannot be interleaved with the leaves whose result it multiplies. The
        // pipeline is swapped by moving the session's DEFAULT NAME rather than by binding after the fact,
        // because every leaf's draw() calls `bind_default()` and would otherwise put the toon pipeline straight
        // back a few instructions later - the same shape as the depth-write lock above.
        //
        // DEPTH TEST stays on, with the compare the overlay pipeline carries (LESS_OR_EQUAL): the quads sit a
        // little in front of the surface they darken, so the test confines each mask to the face it was authored
        // over. THE ARTICLE'S `Stencil { Ref 1 Comp Equal }` ON THE HAIR SHADOW IS NOT REPRODUCED and cannot be
        // here: this renderer's dynamic rendering info declares NO stencil attachment
        // (`stencilAttachmentFormat` is VK_FORMAT_UNDEFINED, see vulkan/constant_init), so there is no stencil
        // plane for a Ref test - the geometry's own coverage is what stands in for it, which is why the hair
        // shadow is a mesh shaped around the forehead rather than a full-screen quad.
        if (!this->pass_frame.overlay_pipeline_name.empty() && !this->pass_frame.overlay_leaves.empty()) {
            env.default_name = this->pass_frame.overlay_pipeline_name;
            for (primitive const* const leaf : this->pass_frame.overlay_leaves) {
                leaf->draw(env);
            }
        }

        io.list->end_rendering();

        // Hand the depth back to the layout everything downstream samples it in. TWO later stages read this
        // image (the resolve's disocclusion guard and the composite's edge test), and this pass is what took
        // it out of SHADER_READ - so leaving it as an attachment would make every read after it a layout
        // error. The colour target needs no hand-back: the composite transitions it for itself.
        if (io.list->barrier(rhi::image_barrier{.resource = io.targets[1].image_handle,
                                                .from = rhi::image_use::depth_attachment,
                                                .to = rhi::image_use::shader_read,
                                                .range = {}}) != rhi::error::ok) {
            return; // a refused barrier would leave the depth in a state nobody declared
        }
    }

} // namespace deren::vulkan::pass
