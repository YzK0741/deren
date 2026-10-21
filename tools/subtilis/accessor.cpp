#include "document_internal.hpp"

namespace deren::tools::subtilis {
    namespace {
        std::span<byte const> buffer_data(fastgltf::Buffer const& b) {
            return std::visit([](auto const& data) -> std::span<byte const> {
                using T = std::remove_cvref_t<decltype(data)>;
                if constexpr (std::is_same_v<T, fastgltf::sources::Array> || std::is_same_v<T, fastgltf::sources::Vector> || std::is_same_v<T, fastgltf::sources::ByteView>)
                    return {data.bytes.data(), data.bytes.size()};
                else
                    return {};
            },
                              b.data);
        }
        size_t element_bytes(fastgltf::Accessor const& a) {
            auto width = fastgltf::getComponentByteSize(a.componentType);
            unsigned columns = 0;
            if (a.type == fastgltf::AccessorType::Mat2)
                columns = 2;
            if (a.type == fastgltf::AccessorType::Mat3)
                columns = 3;
            if (a.type == fastgltf::AccessorType::Mat4)
                columns = 4;
            return columns ? ((columns * width + 3) & ~size_t(3)) * columns : fastgltf::getElementByteSize(a.type, a.componentType);
        }
        bool window(size_t offset, size_t count, size_t stride, size_t width, size_t length) {
            return count && width && stride >= width && offset <= length && width <= length - offset && count - 1 <= (length - offset - width) / stride;
        }
        bool has_texture(yyjson_mut_val* material, bool& uv0_only) {
            bool found = false;
            auto check = [&](yyjson_mut_val* texture) {
                if (!texture)
                    return;
                found = true;
                auto* set = member(texture, "texCoord");
                if (set && (!yyjson_mut_is_uint(set) || yyjson_mut_get_uint(set) != 0))
                    uv0_only = false;
                auto* override = member(member(member(texture, "extensions"), "KHR_texture_transform"), "texCoord");
                if (override && (!yyjson_mut_is_uint(override) || yyjson_mut_get_uint(override) != 0))
                    uv0_only = false;
            };
            check(member(material, "normalTexture"));
            check(member(material, "occlusionTexture"));
            check(member(material, "emissiveTexture"));
            auto* pbr = member(material, "pbrMetallicRoughness");
            check(member(pbr, "baseColorTexture"));
            check(member(pbr, "metallicRoughnessTexture"));
            return found;
        }
    } // namespace
    result<void> preflight_references(document const& d) {
        auto const& a = d.state->parsed;
        // fastgltf 0.9 validate() dereferences these references before checking
        // their ranges. Keep upstream untouched; guard its preconditions here.
        for (auto const& acc : a.accessors) {
            if (acc.bufferViewIndex && *acc.bufferViewIndex >= a.bufferViews.size())
                return fail("accessor bufferView index");
            if (acc.sparse && (acc.sparse->indicesBufferView >= a.bufferViews.size() || acc.sparse->valuesBufferView >= a.bufferViews.size()))
                return fail("sparse bufferView index");
        }
        for (auto const& animation : a.animations) {
            for (auto const& sampler : animation.samplers)
                if (sampler.inputAccessor >= a.accessors.size() || sampler.outputAccessor >= a.accessors.size())
                    return fail("animation accessor index");
            for (auto const& channel : animation.channels)
                if (channel.samplerIndex >= animation.samplers.size() || (channel.nodeIndex && *channel.nodeIndex >= a.nodes.size()))
                    return fail("animation channel index");
        }
        for (auto const& node : a.nodes)
            for (auto id : node.children)
                if (id >= a.nodes.size())
                    return fail("node child index");
        for (auto const& skin : a.skins)
            for (auto id : skin.joints)
                if (id >= a.nodes.size())
                    return fail("skin joint index");
        for (auto const& mesh : a.meshes)
            for (auto const& primitive : mesh.primitives) {
                size_t count = 0;
                for (auto const& attr : primitive.attributes) {
                    if (attr.accessorIndex >= a.accessors.size())
                        return fail("attribute accessor index");
                    auto n = a.accessors[attr.accessorIndex].count;
                    if (count && n != count)
                        return fail("attribute count mismatch");
                    count = n;
                }
                for (auto const& target : primitive.targets)
                    for (auto const& attr : target)
                        if (attr.accessorIndex >= a.accessors.size() || a.accessors[attr.accessorIndex].count != count)
                            return fail("morph accessor index/count");
            }
        return {};
    }
    result<void> validate_accessors(document const& d) {
        auto const& a = d.state->parsed;
        auto const& lim = d.state->limits;
        if (a.accessors.size() > lim.max_records || a.bufferViews.size() > lim.max_records || a.meshes.size() > lim.max_records || a.nodes.size() > lim.max_records)
            return fail("glTF record limit", asset::error_code::capacity);
        for (auto const& b : a.buffers)
            if (b.byteLength > buffer_data(b).size())
                return fail("buffer data shorter than declared length");
        for (auto const& v : a.bufferViews) {
            if (v.bufferIndex >= a.buffers.size())
                return fail("bufferView buffer index");
            auto length = a.buffers[v.bufferIndex].byteLength;
            if (v.byteOffset > length || v.byteLength > length - v.byteOffset)
                return fail("bufferView range");
        }
        for (auto const& acc : a.accessors) {
            auto width = element_bytes(acc), component = fastgltf::getComponentByteSize(acc.componentType);
            if (!component || !width || !acc.count || acc.count > lim.max_records || acc.count > lim.max_input_bytes / width)
                return fail("accessor count/size limit");
            if (acc.byteOffset % component)
                return fail("accessor component alignment");
            if (acc.bufferViewIndex) {
                auto const& v = a.bufferViews[*acc.bufferViewIndex];
                auto stride = v.byteStride.value_or(width);
                if (!window(acc.byteOffset, acc.count, stride, width, v.byteLength) || (v.byteOffset + acc.byteOffset) % component || stride % component)
                    return fail("accessor exceeds bufferView window");
            } else if (acc.byteOffset)
                return fail("accessor without bufferView has offset");
            if (acc.sparse) {
                auto const& sparse = *acc.sparse;
                auto type = sparse.indexComponentType;
                if (type != fastgltf::ComponentType::UnsignedByte && type != fastgltf::ComponentType::UnsignedShort && type != fastgltf::ComponentType::UnsignedInt)
                    return fail("invalid sparse index type");
                auto index_width = fastgltf::getComponentByteSize(type);
                if (!sparse.count || sparse.count > acc.count || sparse.indicesBufferView >= a.bufferViews.size() || sparse.valuesBufferView >= a.bufferViews.size())
                    return fail("invalid sparse counts/views");
                auto const& iv = a.bufferViews[sparse.indicesBufferView];
                auto const& vv = a.bufferViews[sparse.valuesBufferView];
                if (iv.byteStride || vv.byteStride || iv.target || vv.target || !window(sparse.indicesByteOffset, sparse.count, index_width, index_width, iv.byteLength) || !window(sparse.valuesByteOffset, sparse.count, width, width, vv.byteLength) || (iv.byteOffset + sparse.indicesByteOffset) % index_width || (vv.byteOffset + sparse.valuesByteOffset) % component)
                    return fail("invalid sparse window/alignment");
                auto bytes = buffer_data(a.buffers[iv.bufferIndex]);
                uint64_t previous = 0;
                for (size_t i = 0; i < sparse.count; ++i) {
                    uint64_t id = 0;
                    for (size_t j = 0; j < index_width; ++j)
                        id |= uint64_t(std::to_integer<unsigned>(bytes[iv.byteOffset + sparse.indicesByteOffset + i * index_width + j])) << (j * 8);
                    if (id >= acc.count || (i && id <= previous))
                        return fail("sparse indices not strictly increasing/in range");
                    previous = id;
                }
            }
        }
        return {};
    }
    bool eligible(document const& d, primitive_key key, bool skinned) {
        auto const& a = d.state->parsed;
        if (key.mesh >= a.meshes.size() || key.primitive >= a.meshes[key.mesh].primitives.size() || skinned)
            return false;
        auto const& p = a.meshes[key.mesh].primitives[key.primitive];
        if (p.type != fastgltf::PrimitiveType::Triangles || !p.targets.empty() || p.dracoCompression)
            return false;
        bool position = false, normal = false, uv = false;
        for (auto const& attr : p.attributes) {
            if (attr.name == "POSITION")
                position = true;
            else if (attr.name == "NORMAL")
                normal = true;
            else if (attr.name == "TEXCOORD_0")
                uv = true;
            else
                return false;
            auto const& acc = a.accessors[attr.accessorIndex];
            if (acc.componentType == fastgltf::ComponentType::Double || (acc.bufferViewIndex && a.bufferViews[*acc.bufferViewIndex].meshoptCompression))
                return false;
        }
        if (!position || !normal)
            return false;
        if (p.materialIndex) {
            if (a.materials[*p.materialIndex].alphaMode != fastgltf::AlphaMode::Opaque)
                return false;
            auto* material = yyjson_mut_arr_get(member(root(d), "materials"), *p.materialIndex);
            bool uv0 = true;
            if (has_texture(material, uv0) && (!uv || !uv0))
                return false;
            auto* ext = member(material, "extensions");
            if (yyjson_mut_obj_size(ext) > size_t(member(ext, "KHR_materials_unlit") ? 1 : 0))
                return false;
        }
        auto* extensions = member(primitive_value(d, key), "extensions");
        // Unknown primitive extensions may change geometry or impose attribute constraints.
        if (yyjson_mut_obj_size(extensions) > size_t(member(extensions, "DEREN_subtilis") ? 1 : 0))
            return false;
        return true;
    }
    result<asset::source_geometry> extract_geometry(document const& d, primitive_key key) {
        if (!d.state || key.mesh >= d.state->parsed.meshes.size() || key.primitive >= d.state->parsed.meshes[key.mesh].primitives.size())
            return fail("primitive index outside document");
        auto const& a = d.state->parsed;
        auto const& p = a.meshes[key.mesh].primitives[key.primitive];
        if (p.type != fastgltf::PrimitiveType::Triangles)
            return fail("primitive is not TRIANGLES");
        auto position = p.findAttribute("POSITION"), normal = p.findAttribute("NORMAL"), uv = p.findAttribute("TEXCOORD_0");
        if (position == p.attributes.end() || normal == p.attributes.end() || a.accessors[position->accessorIndex].type != fastgltf::AccessorType::Vec3 || a.accessors[normal->accessorIndex].type != fastgltf::AccessorType::Vec3)
            return fail("POSITION/NORMAL vec3 required");
        asset::source_geometry out;
        auto count = a.accessors[position->accessorIndex].count;
        out.positions.reserve(count);
        out.normals.reserve(count);
        out.uv0.reserve(count);
        fastgltf::iterateAccessor<fastgltf::math::fvec3>(a, a.accessors[position->accessorIndex], [&](auto v) { out.positions.push_back({v[0], v[1], v[2]}); });
        fastgltf::iterateAccessor<fastgltf::math::fvec3>(a, a.accessors[normal->accessorIndex], [&](auto v) { out.normals.push_back({v[0], v[1], v[2]}); });
        if (uv != p.attributes.end()) {
            if (a.accessors[uv->accessorIndex].type != fastgltf::AccessorType::Vec2)
                return fail("UV0 must be vec2");
            fastgltf::iterateAccessor<fastgltf::math::fvec2>(a, a.accessors[uv->accessorIndex], [&](auto v) { out.uv0.push_back({v[0], v[1]}); });
        } else {
            bool uv0 = true;
            auto* material = p.materialIndex ? yyjson_mut_arr_get(member(root(d), "materials"), *p.materialIndex) : nullptr;
            if (has_texture(material, uv0))
                return fail("textured primitive has no UV0");
            out.uv0.resize(count, {0, 0});
        }
        if (p.indicesAccessor) {
            auto const& acc = a.accessors[*p.indicesAccessor];
            if (acc.type != fastgltf::AccessorType::Scalar || acc.normalized || (acc.componentType != fastgltf::ComponentType::UnsignedByte && acc.componentType != fastgltf::ComponentType::UnsignedShort && acc.componentType != fastgltf::ComponentType::UnsignedInt))
                return fail("invalid index accessor");
            out.indices.reserve(acc.count);
            fastgltf::iterateAccessor<uint32_t>(a, acc, [&](uint32_t id) { out.indices.push_back(id); });
        } else {
            if (count > UINT32_MAX)
                return fail("vertex index limit");
            out.indices.resize(count);
            std::iota(out.indices.begin(), out.indices.end(), 0u);
        }
        auto hash = asset::source_digest(out);
        if (!hash)
            return std::unexpected(hash.error());
        return out;
    }
} // namespace deren::tools::subtilis
