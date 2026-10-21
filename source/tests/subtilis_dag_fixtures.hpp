#pragma once
// Include after importing deren.asset.subtilis.dag; fixtures use public types only.
namespace subtilis_fixture {
    namespace sub = deren::asset::subtilis;
    struct input {
        sub::source_geometry source;
        sub::payload data;
    };
    inline input cross_dag() {
        input out;
        auto vertex = [&](float x, float y) {
            for (size_t i = 0; i < out.source.positions.size(); ++i)
                if (out.source.positions[i] == sub::array<float, 3>{x, y, 0})
                    return static_cast<uint32_t>(i);
            auto id = static_cast<uint32_t>(out.source.positions.size());
            out.source.positions.push_back({x, y, 0});
            out.source.normals.push_back({0, 0, 1});
            out.source.uv0.push_back({x, y});
            return id;
        };
        sub::array<sub::vector<uint32_t>, 4> coarse;
        for (unsigned y = 0; y < 2; ++y)
            for (unsigned x = 0; x < 2; ++x) {
                auto id = y * 2 + x;
                auto a = vertex(float(x), float(y)), b = vertex(float(x + 1), float(y)), c = vertex(float(x + 1), float(y + 1)), d = vertex(float(x), float(y + 1));
                auto m = vertex(x + .5f, y + .5f), n = vertex(x + .5f, y + .125f);
                coarse[id] = {a, b, m, b, c, m, c, d, m, d, a, m};
                sub::vector<uint32_t> fine{a, b, n, b, m, n, m, a, n, b, c, m, c, d, m, d, a, m};
                auto first = static_cast<uint32_t>(out.data.indices.size());
                out.data.indices.insert(out.data.indices.end(), fine.begin(), fine.end());
                out.source.indices.insert(out.source.indices.end(), fine.begin(), fine.end());
                out.data.clusters.push_back({first, 18, sub::none, y, {1, 1, 0, 2}});
                out.data.leaves.push_back(id);
            }
        for (uint32_t i = 0; i < 4; ++i) {
            auto first = static_cast<uint32_t>(out.data.indices.size());
            out.data.indices.insert(out.data.indices.end(), coarse[i].begin(), coarse[i].end());
            out.data.clusters.push_back({first, 12, i / 2, 2 + i % 2, {1, 1, 0, 2}});
        }
        for (uint32_t x = 0; x < 2; ++x) {
            sub::array<uint32_t, 6> rim{vertex(float(x), 0), vertex(float(x + 1), 0), vertex(float(x + 1), 1), vertex(float(x + 1), 2), vertex(float(x), 2), vertex(float(x), 1)};
            auto m = vertex(x + .5f, .5f);
            auto first = static_cast<uint32_t>(out.data.indices.size());
            for (unsigned i = 0; i < 6; ++i)
                out.data.indices.insert(out.data.indices.end(), {rim[i], rim[(i + 1) % 6], m});
            out.data.clusters.push_back({first, 18, 2 + x, sub::none, {1, 1, 0, 2}});
            out.data.roots.push_back(8 + x);
        }
        out.data.references = {0, 1, 4, 5, 2, 3, 6, 7, 4, 6, 8, 5, 7, 9};
        out.data.groups = {{0, 2, 2, 2, {1, 1, 0, 4}, .1f}, {4, 2, 6, 2, {1, 1, 0, 4}, .1f}, {8, 2, 10, 1, {1, 1, 0, 4}, .2f}, {11, 2, 13, 1, {1, 1, 0, 4}, .2f}};
        out.data.build_meta = "{}";
        out.data.source_hash = *sub::source_digest(out.source);
        out.data.build_hash = *sub::build_digest({});
        return out;
    }
} // namespace subtilis_fixture
