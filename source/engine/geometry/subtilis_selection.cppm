module;
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <glm/glm.hpp>
#include <limits>
#include <memory>
#include <span>
#include <utility>
export module deren.engine.subtilis.selection;
import deren.engine.primitive;
import deren.engine.meshlet;
import deren.engine.subtilis;
import deren.engine.subtilis.builder;
import deren.engine.subtilis.upload;
import deren.engine.subtilis.lod;
import deren.promise.rhi;
import deren.utility.shared;

export namespace deren::engine::subtilis {
    struct item_policy {
        std::uint32_t material_flags = 0;
        bool exact_depth_redraw = false;
    };
    struct item_view {
        view_parameters selection;
        std::array<float, 16> clip_from_object{};
    };
    [[nodiscard]] inline item_view make_item_view(glm::mat4 const& model, glm::mat4 const& camera,
                                                  glm::mat4 const& projection, std::uint32_t width, std::uint32_t height) noexcept {
        item_view result;
        auto& p = result.selection;
        std::memcpy(p.model.data(), &model, sizeof(model));
        std::memcpy(p.view.data(), &camera, sizeof(camera));
        std::memcpy(p.projection.data(), &projection, sizeof(projection));
        auto clip = projection * camera * model; // preserve the legacy LOD0 multiplication order
        std::memcpy(result.clip_from_object.data(), &clip, sizeof(clip));
        p.width = width;
        p.height = height;
        bool perspective = projection[2][3] == -1 && projection[3][3] == 0;
        bool orthographic = projection[2][3] == 0 && projection[3][3] == 1;
        p.kind = perspective ? projection_kind::perspective : projection_kind::orthographic;
        p.near_plane = std::numeric_limits<float>::quiet_NaN();
        bool standard_depth = projection[0][2] == 0 && projection[1][2] == 0 && projection[2][2] < 0;
        bool standard_perspective = perspective && projection[2][2] <= -1 && projection[3][0] == 0 && projection[3][1] == 0;
        if ((standard_perspective || orthographic) && standard_depth) {
            double near = double(projection[3][2]) / projection[2][2];
            p.near_plane = static_cast<float>(near);
            if (double(p.near_plane) < near) {
                p.near_plane = std::nextafter(p.near_plane, std::numeric_limits<float>::infinity());
            }
        }
        return result;
    }
    /// Check both allocation identity and its retaining control block: raw pointer
    /// equality alone would accept a non-owning alias with an unrelated lifetime.
    [[nodiscard]] inline bool matches_generation(primitive const& leaf) noexcept {
        auto const& generation = leaf.subtilis_geometry;
        if (!generation || leaf.vertex_buffer.get() != generation->vertices() || leaf.index_buffer.get() != generation->indices()) {
            return false;
        }
        auto same_owner = [&](std::shared_ptr<void const> const& lease) {
            return lease.get() == generation.get() && !lease.owner_before(generation) && !generation.owner_before(lease);
        };
        auto const& source = generation->source();
        if (!same_owner(leaf.vertex_buffer.lease()) || !same_owner(leaf.index_buffer.lease()) ||
            leaf.index_type != deren::promise::rhi::index_type::uint32 || leaf.draw_index_count != source.lod0_index_count() ||
            leaf.vertex_count != source.vertex_count() || leaf.vertex_stride != source.vertex_stride() ||
            leaf.vertex_buffer->size() < source.vertices().size() || leaf.index_buffer->size() < source.indices().size_bytes() ||
            leaf.meshlet_count != source.hierarchy().lod0_count() || leaf.meshlets.size() != leaf.meshlet_count) {
            return false;
        }
        std::uint64_t cursor = 0;
        for (auto const& record : leaf.meshlets) {
            if (record.first_index != cursor || record.base_vertex != 0 || !meshlet_record_sound(record, source.lod0_index_count())) {
                return false;
            }
            cursor += record.index_count;
        }
        return cursor == source.lod0_index_count();
    }
    [[nodiscard]] inline bool coarse_allowed(primitive const& leaf, item_policy const& policy) noexcept {
        if (!leaf.subtilis_geometry) {
            return false;
        }
        auto const& source = leaf.subtilis_geometry->source();
        auto const& layout = source.statistics();
        return source.vertex_stride() == 64 && layout.normal_offset == 12 && layout.uv_offset == 24 &&
               !(policy.material_flags & (16u | 32u)) && !policy.exact_depth_redraw &&
               !leaf.transparent && !leaf.overlay_kind && leaf.pipeline_name.empty() &&
               !(leaf.push.flags & 1u) && !leaf.push.skin_base && !leaf.push.morph_targets;
    }
    template <class Publish>
    [[nodiscard]] visible_run prepare_item(primitive const& leaf, item_policy const& policy, item_view const& frame,
                                           deren::utility::vessel::vector<meshlet>& scratch,
                                           std::span<reservation const> published, Publish&& publish) {
        reservation space{leaf.meshlet_base, leaf.meshlet_count};
        if (!leaf.subtilis_geometry) {
            return prepare_lod0(leaf.meshlets, space, frame.clip_from_object, scratch, published, std::forward<Publish>(publish));
        }
        if (!matches_generation(leaf)) {
            scratch.clear();
            return shared_run(space, fallback_reason::source_mismatch);
        }
        auto view = frame.selection;
        view.force_finest = view.force_finest || !coarse_allowed(leaf, policy);
        return prepare_hierarchy(leaf.subtilis_geometry->source().hierarchy(), space, view, scratch, published, std::forward<Publish>(publish));
    }
} // namespace deren::engine::subtilis
