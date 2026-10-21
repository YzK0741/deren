module;
#include <cstring>
#include <yyjson.h>
module deren.asset.subtilis.codec;

namespace deren::asset::subtilis {
    namespace {
        constexpr uint32_t strides[]{4, 32, 48, 4, 4, 4, 1};
        constexpr uint64_t directory_end = 352;
        struct section {
            uint64_t count = 0, offset = 0, length = 0;
        };
        auto failure(error_code code, char const* context) {
            return std::unexpected(error{code, context, {}, {}});
        }
        uint64_t read(std::span<byte const> bytes, size_t at, size_t width) {
            uint64_t value = 0;
            for (size_t i = 0; i < width; ++i)
                value |= uint64_t(std::to_integer<unsigned>(bytes[at + i])) << (8 * i);
            return value;
        }
        float read_float(std::span<byte const> bytes, size_t at) {
            return std::bit_cast<float>(uint32_t(read(bytes, at, 4)));
        }
        void put(std::span<byte> bytes, size_t at, uint64_t value, size_t width) {
            for (size_t i = 0; i < width; ++i)
                bytes[at + i] = byte((value >> (8 * i)) & 255);
        }
        void put_float(std::span<byte> bytes, size_t at, float value) {
            put(bytes, at, std::bit_cast<uint32_t>(value), 4);
        }
        bool zero(std::span<byte const> bytes) {
            return std::all_of(bytes.begin(), bytes.end(), [](byte b) { return b == byte{0}; });
        }
        bool valid_sphere(sphere s) {
            return std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.z) && std::isfinite(s.radius) && s.radius >= 0;
        }
        sphere read_sphere(std::span<byte const> b, size_t at) {
            return {read_float(b, at), read_float(b, at + 4), read_float(b, at + 8), read_float(b, at + 12)};
        }
        void put_sphere(std::span<byte> b, size_t at, sphere s) {
            put_float(b, at, s.x);
            put_float(b, at + 4, s.y);
            put_float(b, at + 8, s.z);
            put_float(b, at + 12, s.radius);
        }
        bool valid_meta(std::span<byte const> bytes) {
            yyjson_alc allocator{
                [](void*, size_t size) -> void* { return utility::memory::allocate(size); },
                [](void*, void* old, size_t old_size, size_t size) -> void* {
                    auto* next = utility::memory::allocate(size);
                    if (old)
                        std::memcpy(next, old, std::min(old_size, size));
                    utility::memory::deallocate(old);
                    return next;
                },
                [](void*, void* ptr) { utility::memory::deallocate(ptr); }, nullptr};
            auto* doc = yyjson_read_opts(reinterpret_cast<char*>(const_cast<byte*>(bytes.data())), bytes.size(), 0, &allocator, nullptr);
            if (!doc)
                return false;
            bool const object = yyjson_is_obj(yyjson_doc_get_root(doc));
            yyjson_doc_free(doc);
            return object;
        }
        result<array<section, 7>> layout(payload const& p, limits const& lim) {
            array<uint64_t, 7> counts{p.indices.size(), p.clusters.size(), p.groups.size(), p.references.size(), p.leaves.size(), p.roots.size(), p.build_meta.size()};
            array<section, 7> sections{};
            uint64_t cursor = directory_end;
            for (size_t i = 0; i < 7; ++i) {
                if (counts[i] > lim.max_records || counts[i] > UINT32_MAX || ((i == 1 || i == 2) && counts[i] == none))
                    return failure(error_code::capacity, "section count exceeds limit");
                auto const length = counts[i] * strides[i];
                if (cursor > lim.max_payload_bytes || length > lim.max_payload_bytes - cursor || cursor + length > SIZE_MAX - 7)
                    return failure(error_code::capacity, "payload exceeds byte limit");
                sections[i] = {counts[i], cursor, length};
                cursor = (cursor + length + 7) & ~uint64_t(7);
            }
            if (cursor > lim.max_payload_bytes)
                return failure(error_code::capacity, "payload padding exceeds byte limit");
            return sections;
        }
    } // namespace
    result<vector<byte>> encode(payload const& p, limits const& lim) {
        auto sections = layout(p, lim);
        if (!sections)
            return std::unexpected(sections.error());
        for (auto const& c : p.clusters)
            if (!valid_sphere(c.geometry))
                return failure(error_code::invalid_payload, "invalid geometry sphere");
        for (auto const& g : p.groups)
            if (!valid_sphere(g.lod) || !std::isfinite(g.error) || g.error < 0)
                return failure(error_code::invalid_payload, "invalid LOD sphere or error");
        if (!valid_meta(std::as_bytes(std::span(p.build_meta))))
            return failure(error_code::invalid_payload, "BUILD_META must be a strict UTF-8 JSON object");
        auto const total = (sections->back().offset + sections->back().length + 7) & ~uint64_t(7);
        vector<byte> bytes(static_cast<size_t>(total), byte{0});
        put(bytes, 0, 0x00474144534e5244ULL, 8);
        put(bytes, 8, 1, 2);
        put(bytes, 12, 128, 4);
        put(bytes, 16, total, 8);
        put(bytes, 24, 7, 4);
        std::copy(p.source_hash.begin(), p.source_hash.end(), bytes.begin() + 32);
        std::copy(p.build_hash.begin(), p.build_hash.end(), bytes.begin() + 64);
        for (size_t i = 0; i < 7; ++i) {
            size_t const d = 128 + i * 32;
            auto const s = (*sections)[i];
            put(bytes, d, i + 1, 4);
            put(bytes, d + 4, strides[i], 4);
            put(bytes, d + 8, s.count, 8);
            put(bytes, d + 16, s.offset, 8);
            put(bytes, d + 24, s.length, 8);
        }
        auto write_ids = [&](size_t id, vector<uint32_t> const& ids) { size_t at = (*sections)[id].offset; for (auto n : ids) { put(bytes, at, n, 4); at += 4; } };
        write_ids(0, p.indices);
        write_ids(3, p.references);
        write_ids(4, p.leaves);
        write_ids(5, p.roots);
        size_t at = (*sections)[1].offset;
        for (auto const& c : p.clusters) {
            put(bytes, at, c.first_index, 4);
            put(bytes, at + 4, c.index_count, 4);
            put(bytes, at + 8, c.producer, 4);
            put(bytes, at + 12, c.consumer, 4);
            put_sphere(bytes, at + 16, c.geometry);
            at += 32;
        }
        at = (*sections)[2].offset;
        for (auto const& g : p.groups) {
            put(bytes, at, g.fine_first, 4);
            put(bytes, at + 4, g.fine_count, 4);
            put(bytes, at + 8, g.coarse_first, 4);
            put(bytes, at + 12, g.coarse_count, 4);
            put_sphere(bytes, at + 16, g.lod);
            put_float(bytes, at + 32, g.error);
            at += 48;
        }
        std::memcpy(bytes.data() + (*sections)[6].offset, p.build_meta.data(), p.build_meta.size());
        return bytes;
    }
    result<payload> decode(std::span<byte const> bytes, limits const& lim) {
        if (bytes.size() > lim.max_payload_bytes)
            return failure(error_code::capacity, "payload exceeds byte limit");
        if (bytes.size() < directory_end)
            return failure(error_code::invalid_payload, "truncated header or directory");
        if (read(bytes, 0, 8) != 0x00474144534e5244ULL)
            return failure(error_code::invalid_payload, "invalid magic");
        if (read(bytes, 8, 2) != 1 || read(bytes, 10, 2) != 0)
            return failure(error_code::unsupported_version, "unsupported payload version");
        if (read(bytes, 12, 4) != 128 || read(bytes, 16, 8) != bytes.size() || read(bytes, 24, 4) != 7 || read(bytes, 28, 4) != 0 || !zero(bytes.subspan(96, 32)))
            return failure(error_code::invalid_payload, "invalid header fields");
        array<section, 7> sections{};
        array<bool, 7> seen{};
        for (size_t i = 0; i < 7; ++i) {
            size_t const d = 128 + i * 32;
            auto const type = read(bytes, d, 4);
            if (type < 1 || type > 7 || seen[type - 1])
                return failure(error_code::invalid_payload, "unknown or duplicate section");
            size_t const slot = type - 1;
            seen[slot] = true;
            auto const stride = read(bytes, d + 4, 4), count = read(bytes, d + 8, 8), offset = read(bytes, d + 16, 8), length = read(bytes, d + 24, 8);
            if (stride != strides[slot])
                return failure(error_code::invalid_payload, "invalid section stride");
            if (count > lim.max_records || count > UINT32_MAX || ((slot == 1 || slot == 2) && count == none))
                return failure(error_code::capacity, "section count exceeds limit");
            if (length != count * stride || offset < directory_end || offset % 8 || offset > bytes.size() || length > bytes.size() - offset)
                return failure(error_code::invalid_payload, "invalid section range");
            sections[slot] = {count, offset, length};
        }
        auto sorted = sections;
        std::sort(sorted.begin(), sorted.end(), [](section a, section b) { return a.offset < b.offset || (a.offset == b.offset && a.length < b.length); });
        uint64_t cursor = directory_end;
        for (auto s : sorted) {
            if (s.offset < cursor || !zero(bytes.subspan(cursor, s.offset - cursor)))
                return failure(error_code::invalid_payload, "overlapping sections or nonzero padding");
            cursor = s.offset + s.length;
        }
        if (!zero(bytes.subspan(cursor)))
            return failure(error_code::invalid_payload, "nonzero trailing padding");
        // All scalar records are validated before allocating decoded arrays.
        for (uint64_t i = 0, at = sections[1].offset; i < sections[1].count; ++i, at += 32)
            if (!valid_sphere(read_sphere(bytes, at + 16)))
                return failure(error_code::invalid_payload, "invalid cluster sphere");
        for (uint64_t i = 0, at = sections[2].offset; i < sections[2].count; ++i, at += 48) {
            auto const value = read_float(bytes, at + 32);
            if (!valid_sphere(read_sphere(bytes, at + 16)) || !std::isfinite(value) || value < 0 || !zero(bytes.subspan(at + 36, 12)))
                return failure(error_code::invalid_payload, "invalid group record");
        }
        auto meta = bytes.subspan(sections[6].offset, sections[6].length);
        if (!valid_meta(meta))
            return failure(error_code::invalid_payload, "invalid BUILD_META JSON");
        payload p;
        std::copy_n(bytes.begin() + 32, 32, p.source_hash.begin());
        std::copy_n(bytes.begin() + 64, 32, p.build_hash.begin());
        auto read_ids = [&](size_t id, vector<uint32_t>& ids) { auto s = sections[id]; ids.resize(s.count); for (size_t i = 0; i < ids.size(); ++i) ids[i] = uint32_t(read(bytes, s.offset + i * 4, 4)); };
        read_ids(0, p.indices);
        read_ids(3, p.references);
        read_ids(4, p.leaves);
        read_ids(5, p.roots);
        p.clusters.reserve(sections[1].count);
        for (uint64_t i = 0, at = sections[1].offset; i < sections[1].count; ++i, at += 32)
            p.clusters.push_back({uint32_t(read(bytes, at, 4)), uint32_t(read(bytes, at + 4, 4)), uint32_t(read(bytes, at + 8, 4)), uint32_t(read(bytes, at + 12, 4)), read_sphere(bytes, at + 16)});
        p.groups.reserve(sections[2].count);
        for (uint64_t i = 0, at = sections[2].offset; i < sections[2].count; ++i, at += 48)
            p.groups.push_back({uint32_t(read(bytes, at, 4)), uint32_t(read(bytes, at + 4, 4)), uint32_t(read(bytes, at + 8, 4)), uint32_t(read(bytes, at + 12, 4)), read_sphere(bytes, at + 16), read_float(bytes, at + 32)});
        p.build_meta.assign(reinterpret_cast<char const*>(meta.data()), meta.size());
        return p;
    }
} // namespace deren::asset::subtilis
