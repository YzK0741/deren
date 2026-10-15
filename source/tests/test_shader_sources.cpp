// Headless unit tests: THE SHADER PIPELINE'S SOURCES OF TRUTH ================================
// One .spv is named in FOUR places, and nothing in the build compares them:
//
//   1. CMakeLists.txt's `VR_SLANG_SOURCES` - the canonical list, and what `cmake --build` compiles;
//   2. source/shaders/compile_shaders.ps1 - the escape hatch for a machine without CMake;
//   3. source/shaders/compile_shaders.sh - its POSIX twin;
//   4. the C++ that LOADS the .spv by name at runtime (chores.cpp registers them, and a pass or the
//      runtime asks for one by name).
//
// A drift between 1 and 2/3 is silent: the build produces the file, the script does not, and the
// escape hatch quietly builds one shader fewer - which is exactly what was found the moment this test
// was written (`debug/heap_probe.slang:mesh_main:mesh:heap_probe.mesh.spv` was in CMakeLists.txt and in
// neither script, so a machine using the scripts would have lost the mesh probe). A drift between 1
// and 4 is louder (a load logs "no shader"), but it is the same class of mistake and it is checkable
// here too.
//
// THIS IS THE MESH MIGRATION'S CI GATE, and it is deliberately able to FAIL: the list must contain the
// three MESH entries (a mesh stage is a different stage type, so its .spv name is new - deleting one is
// how "the mesh path" would silently stop being built), every `mesh_main` a .slang file declares must
// have an entry, and every entry's source must exist. The capture gate cannot run in CI at all (its
// references are tied to one machine's driver); this can.
#include "vk_test.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

import deren.utility.shared;

namespace {
    /// the lines of a file, or an empty vector when it cannot be read (the CHECKs below report that)
    deren::utility::vessel::vector<deren::utility::vessel::string> read_lines(std::filesystem::path const& path) {
        deren::utility::vessel::vector<deren::utility::vessel::string> lines;
        std::ifstream file(path);
        CHECK_MSG(file.is_open(), path.string().c_str());
        deren::utility::vessel::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
        }
        return lines;
    }

    deren::utility::vessel::string trim(deren::utility::vessel::string text) {
        auto const not_space = [](uint8_t const c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
        text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
        text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
        return text;
    }

    /// Is @p text one of the `<source>:<entry>:<stage>:<output>` entries? Four non-empty fields, a .slang
    /// source and a .spv output - the SHAPE is the contract, so a line that merely mentions a shader (a
    /// comment, a log message) cannot pass for an entry.
    bool is_entry(deren::utility::vessel::string const& text, deren::utility::vessel::string* const output = nullptr) {
        deren::utility::vessel::vector<deren::utility::vessel::string> fields;
        std::size_t start = 0;
        while (true) {
            std::size_t const colon = text.find(':', start);
            fields.push_back(text.substr(start, colon == deren::utility::vessel::string::npos ? deren::utility::vessel::string::npos : colon - start));
            if (colon == deren::utility::vessel::string::npos) {
                break;
            }
            start = colon + 1;
        }
        if (fields.size() != 4u || fields[0].empty() || fields[1].empty() || fields[2].empty() || fields[3].empty()) {
            return false;
        }
        if (fields[0].find(".slang") != fields[0].size() - 6u) {
            return false;
        }
        if (fields[3].find(".spv") != fields[3].size() - 4u) {
            return false;
        }
        if (output != nullptr) {
            *output = fields[3];
        }
        return true;
    }

    /// The whole of a file, or an empty string when it cannot be read (the CHECKs report that elsewhere).
    deren::utility::vessel::string read_text(std::filesystem::path const& path) {
        deren::utility::vessel::string text;
        for (deren::utility::vessel::string const& line : read_lines(path)) {
            text += line;
            text += '\n';
        }
        return text;
    }

    /// Every entry in @p path that is a quoted PowerShell list element or a bare list line, i.e. both scripts'
    /// spellings, with the trailing comma/quote stripped.
    deren::utility::vessel::set<deren::utility::vessel::string> entries_in_script(std::filesystem::path const& path) {
        deren::utility::vessel::set<deren::utility::vessel::string> entries;
        for (deren::utility::vessel::string line : read_lines(path)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') {
                continue;
            }
            if (line.front() == '"') {
                line.erase(line.begin());
            }
            if (!line.empty() && line.back() == ',') {
                line.pop_back();
            }
            if (!line.empty() && line.back() == '"') {
                line.pop_back();
            }
            line = trim(line);
            if (is_entry(line)) {
                entries.insert(line);
            }
        }
        return entries;
    }

    /// The entries of CMakeLists.txt's `set(VR_SLANG_SOURCES ...)` block: the block's own lines, from the
    /// `set(` to the closing `)`.
    deren::utility::vessel::set<deren::utility::vessel::string> cmake_entries(std::filesystem::path const& path) {
        deren::utility::vessel::set<deren::utility::vessel::string> entries;
        bool inside = false;
        for (deren::utility::vessel::string line : read_lines(path)) {
            deren::utility::vessel::string const text = trim(line);
            if (!inside) {
                if (text.find("set(VR_SLANG_SOURCES") != deren::utility::vessel::string::npos) {
                    inside = true;
                }
                continue;
            }
            if (!text.empty() && text.front() == ')') {
                break;
            }
            if (is_entry(text)) {
                entries.insert(text);
            }
        }
        return entries;
    }

    /// The .spv names the runtime's app-side loader asks for, as `std::string_view`-style literals in
    /// chores.cpp (`load_shader(shaders_dir, "x.spv", ...)`, `register_shader("x.spv", ...)`).
    deren::utility::vessel::set<deren::utility::vessel::string> loaded_spv_names(std::filesystem::path const& path) {
        deren::utility::vessel::set<deren::utility::vessel::string> names;
        for (deren::utility::vessel::string const& line : read_lines(path)) {
            for (std::string_view const marker : {".spv\"", ".spv\","}) {
                std::size_t at = line.find(marker);
                while (at != deren::utility::vessel::string::npos) {
                    // walk back to the opening quote of the literal
                    std::size_t const open = line.rfind('"', at);
                    if (open != deren::utility::vessel::string::npos) {
                        deren::utility::vessel::string const name = line.substr(open + 1u, at + 4u - open - 1u);
                        if (name.size() > 4u && name.find(".spv") != deren::utility::vessel::string::npos) {
                            names.insert(name);
                        }
                    }
                    at = line.find(marker, at + 1u);
                }
            }
        }
        return names;
    }
} // namespace

int32_t main() {
    deren::utility::vessel::string const root = deren::utility::vessel::string(VR_TEST_SOURCE_DIR);

    deren::utility::vessel::set<deren::utility::vessel::string> const cmake = cmake_entries(root + "/CMakeLists.txt");
    deren::utility::vessel::set<deren::utility::vessel::string> const ps1 = entries_in_script(root + "/source/shaders/compile_shaders.ps1");
    deren::utility::vessel::set<deren::utility::vessel::string> const sh = entries_in_script(root + "/source/shaders/compile_shaders.sh");

    // ---- 1. The canonical list is non-empty and every source file it names exists ----
    CHECK_MSG(!cmake.empty(), "VR_SLANG_SOURCES parsed out of CMakeLists.txt");
    for (deren::utility::vessel::string const& entry : cmake) {
        deren::utility::vessel::string const source = entry.substr(0, entry.find(':'));
        CHECK_MSG(std::filesystem::exists(root + "/source/shaders/" + source), entry.c_str());
    }

    // ---- 2. The two scripts compile EXACTLY the canonical list (this is the drift that was found) ----
    auto const report_difference = [](char const* who, deren::utility::vessel::set<deren::utility::vessel::string> const& expected, deren::utility::vessel::set<deren::utility::vessel::string> const& actual) {
        for (deren::utility::vessel::string const& entry : expected) {
            CHECK_MSG(actual.contains(entry), (deren::utility::vessel::string(who) + " is missing " + entry).c_str());
        }
        for (deren::utility::vessel::string const& entry : actual) {
            CHECK_MSG(expected.contains(entry), (deren::utility::vessel::string(who) + " has an entry the build does not: " + entry).c_str());
        }
    };
    report_difference("compile_shaders.ps1", cmake, ps1);
    report_difference("compile_shaders.sh", cmake, sh);

    // ---- 3. THE MESH ENTRIES EXIST, and every `mesh_main` a source declares has one ----
    // A mesh stage is a different stage type, so its output name is NEW: deleting one of these is how the
    // mesh path would stop being built without anything failing. Both directions are checked - the names
    // this renderer's mesh passes load, and any `mesh_main` a .slang file grows in the future.
    deren::utility::vessel::set<deren::utility::vessel::string> outputs;
    for (deren::utility::vessel::string const& entry : cmake) {
        deren::utility::vessel::string output;
        CHECK(is_entry(entry, &output));
        outputs.insert(output);
        if (entry.find(":mesh_main:mesh:") != deren::utility::vessel::string::npos) {
            CHECK_MSG(entry.find(".mesh.spv") != deren::utility::vessel::string::npos, entry.c_str());
        }
    }
    for (deren::utility::vessel::string const& required : {"shadow.mesh.spv", "pbr.mesh.spv", "heap_probe.mesh.spv"}) {
        CHECK_MSG(outputs.contains(required), required.c_str());
    }
    for (auto const& file : std::filesystem::recursive_directory_iterator(root + "/source/shaders")) {
        if (!file.is_regular_file() || file.path().extension() != ".slang") {
            continue;
        }
        bool declares_mesh_entry = false;
        for (deren::utility::vessel::string const& line : read_lines(file.path().string())) {
            if (line.find("void mesh_main(") != deren::utility::vessel::string::npos) {
                declares_mesh_entry = true;
            }
        }
        if (!declares_mesh_entry) {
            continue;
        }
        deren::utility::vessel::string const source(file.path().lexically_relative(std::filesystem::path(root + "/source/shaders")).generic_string());
        std::size_t const entries_for_source = static_cast<std::size_t>(std::count_if(cmake.begin(), cmake.end(), [&source](deren::utility::vessel::string const& entry) {
            return entry.rfind(source + ":mesh_main:mesh:", 0) == 0;
        }));
        CHECK_MSG(entries_for_source == 1u, ("declares mesh_main with no (or a duplicated) VR_SLANG_SOURCES entry: " + source).c_str());
    }

    // ---- 4. Every .spv the APP loads by name is produced by the list ----
    // The names chores.cpp hands to load_shader/register_shader are the ones the runtime answers, so one that
    // no rule produces is a shader that is silently empty at startup - which is what a renamed output looks
    // like from the app's side.
    for (deren::utility::vessel::string const& name : loaded_spv_names(root + "/source/app/chores.cpp")) {
        CHECK_MSG(outputs.contains(name), ("chores.cpp loads a .spv no VR_SLANG_SOURCES entry produces: " + name).c_str());
    }

    // ---- 5. EVERY #include A LEAF PULLS IN IS A BUILD DEPENDENCY OF THE RULE THAT COMPILES IT ----
    // This is the check that was missing when the mesh geometry fetch arrived: `geometry/mesh_geometry.slang` was included
    // by every geometry leaf and absent from CMakeLists' VR_SHADER_INCLUDES, so editing it left STALE SPIR-V in
    // the build tree - the two halves of one pipeline declared different push blocks, and nothing but a byte
    // comparison against the escape-hatch scripts' output showed it. The list is the transitive closure by hand,
    // so the set of includes found in source/shaders/ must be a SUBSET of it (a leaf's own include graph is what the
    // build cannot see).
    {
        deren::utility::vessel::set<deren::utility::vessel::string> listed;
        // The CMake variable's spelling, as a named marker rather than a hand-counted offset: the first version of
        // this skipped 18 characters of a 19-character marker, so every name in the set carried a leading '/' and
        // the check reported all eight includes as missing. A test whose own parser is off by one is a test that
        // says "the build is wrong" about the build being right.
        constexpr std::string_view include_marker = "VR_SHADER_SRC_DIR}/";
        for (deren::utility::vessel::string const& line : read_lines(root + "/CMakeLists.txt")) {
            std::size_t const marker = line.find(include_marker);
            if (marker == deren::utility::vessel::string::npos) {
                continue;
            }
            deren::utility::vessel::string const name = line.substr(marker + include_marker.size());
            std::size_t const end = name.find('"');
            if (end != deren::utility::vessel::string::npos) {
                listed.insert(name.substr(0, end));
            }
        }
        CHECK_MSG(listed.contains("common/heap_access.slang"), "VR_SHADER_INCLUDES lists common/heap_access.slang");
        deren::utility::vessel::set<deren::utility::vessel::string> found;
        for (auto const& file : std::filesystem::recursive_directory_iterator(root + "/source/shaders")) {
            if (!file.is_regular_file()) {
                continue;
            }
            auto const extension = file.path().extension().string();
            if (extension != ".slang" && extension != ".glsl") {
                continue;
            }
            for (deren::utility::vessel::string const& line : read_lines(file.path().string())) {
                std::size_t const include = line.find("#include \"");
                if (include == deren::utility::vessel::string::npos) {
                    continue;
                }
                deren::utility::vessel::string const name = line.substr(include + 10u);
                std::size_t const end = name.find('"');
                if (end != deren::utility::vessel::string::npos) {
                    found.insert(name.substr(0, end));
                }
            }
        }
        CHECK_MSG(!found.empty(), "source/shaders/ declares #includes to check");
        for (deren::utility::vessel::string const& include : found) {
            // ... and the include exists, which a rename would otherwise leave as a compile error in the build
            // and (worse) as a silently skipped file in this test's own scan
            CHECK_MSG(std::filesystem::exists(root + "/source/shaders/" + include), include.c_str());
            CHECK_MSG(listed.contains(include), ("an included shader is not in CMakeLists' VR_SHADER_INCLUDES, so editing it leaves stale SPIR-V: " + include).c_str());
        }
    }

    // ---- 6. THE HEAP PUSH LANE CONTRACT: a pass that pushes a block loads only stages that declare the lanes ----
    // runtime.cpp's push_with_lanes() appends index lanes AFTER the block a pass hands over (two for
    // push_index_block, three for push_stage_block, whose third is the post chain's own source slot), and the heap
    // accessors reach the per-frame slot arrays only through them. A stage that pushes a block and declares no lanes
    // still compiles and still renders - until it reads one, and then the read is past the block it declared, so the
    // producer and the consumer disagree by 8 or 12 bytes with nothing to catch it. That is a live finding, not a
    // hypothetical: gaussian/gaussian_raster.slang was the single converted stage without them (the raster batch's own
    // independent review found it), while declared-versus-transmitted size is exactly what nothing in the build
    // compares. The rule is derived rather than tabulated - a pass source names the .spv it loads, the canonical list
    // names the .slang that produces it, and that source is the one that must declare the lanes.
    {
        deren::utility::vessel::vector<std::pair<deren::utility::vessel::string, deren::utility::vessel::string>> source_of; // .spv -> .slang
        for (deren::utility::vessel::string const& entry : cmake) {
            std::size_t const first = entry.find(':');
            std::size_t const last = entry.rfind(':');
            if (first == deren::utility::vessel::string::npos || last == deren::utility::vessel::string::npos || first == last) {
                continue;
            }
            source_of.emplace_back(entry.substr(last + 1u), entry.substr(0u, first));
        }
        CHECK_MSG(!source_of.empty(), "the canonical list gives a .slang source for every .spv");

        deren::utility::vessel::set<deren::utility::vessel::string> lane_sources;
        for (auto const& file : std::filesystem::directory_iterator(root + "/source/engine/pass")) {
            if (!file.is_regular_file() || file.path().extension() != ".cpp") {
                continue;
            }
            deren::utility::vessel::string const pass_text = read_text(file.path().string());
            if (pass_text.find("push_block(") == deren::utility::vessel::string::npos) {
                continue; // a pass that pushes nothing has no lanes to declare
            }
            for (deren::utility::vessel::string const& output : loaded_spv_names(file.path().string())) {
                for (auto const& [name, source] : source_of) {
                    if (name != output) {
                        continue;
                    }
                    deren::utility::vessel::string const source_path = root + "/source/shaders/" + source;
                    deren::utility::vessel::string const source_text = read_text(source_path);
                    if (source_text.find("[[vk::push_constant]]") == deren::utility::vessel::string::npos) {
                        continue; // a stage of the pass that declares no block at all
                    }
                    lane_sources.insert(source);
                    for (deren::utility::vessel::string const& lane : {"frame_slot", "image_index"}) {
                        CHECK_MSG(source_text.find(lane) != deren::utility::vessel::string::npos,
                                  (source + " is pushed by a pass that appends index lanes but does not declare " + lane)
                                      .c_str());
                    }
                }
            }
        }
        // THE COVERAGE IS WHAT THE TREE MAKES DERIVABLE, and it is asserted rather than assumed: a pass that names
        // the .spv it loads binds its stage here, while the stages whose shaders the APP registers (most of them -
        // chores.cpp) are bound too far away for this scan and are covered by section 4's registration check. The
        // gaussian raster stage is the one this batch added, so the contract must at least see it; when another
        // converted pass names its shader inline, it joins this rule for free.
        CHECK_MSG(lane_sources.contains("gaussian/gaussian_raster.slang"), "the derived lane contract covers gaussian/gaussian_raster.slang");
    }

    // Runtime sources are entirely Slang. GLSL originals live in source/reference/shaders/.
    for (auto const& file : std::filesystem::recursive_directory_iterator(root + "/source/shaders")) {
        if (!file.is_regular_file()) {
            continue;
        }
        auto const extension = file.path().extension().string();
        CHECK_MSG(extension != ".vert" && extension != ".frag" && extension != ".comp" && extension != ".mesh" && extension != ".glsl",
                  ("a GLSL source is in the runtime shader directory: " + file.path().filename().string()).c_str());
    }

    for (deren::utility::vessel::string const& path : {root + "/CMakeLists.txt", root + "/source/shaders/compile_shaders.ps1", root + "/source/shaders/compile_shaders.sh"}) {
        for (deren::utility::vessel::string const& raw : read_lines(path)) {
            deren::utility::vessel::string const line = trim(raw);
            if (!line.empty() && line.front() != '#') {
                CHECK_MSG(line.find("-allow-glsl") == deren::utility::vessel::string::npos, ("shader compilation still enables GLSL compatibility: " + path).c_str());
            }
        }
    }

    // The forward path must not feed undefined control lanes into shared lighting.
    deren::utility::vessel::string forward;
    for (auto const& line : read_lines(deren::utility::vessel::string(VR_TEST_SOURCE_DIR) + "/source/shaders/scene/pbr.slang"))
        forward += line + "\n";
    CHECK(forward.find("si.shadow_override = -1.0;") != deren::utility::vessel::string::npos);
    CHECK(forward.find("si.punctual_replaced = 0.0;") != deren::utility::vessel::string::npos);
    // The shared light-count lane is a float on the host and in the compute prefix. Reading its
    // bits as uint turns 32 lights into 0x42000000 and makes the traversal visit inactive slots.
    auto const shading = read_text(deren::utility::vessel::string(VR_TEST_SOURCE_DIR) + "/source/shaders/lighting/shading.slang");
    CHECK(shading.find("float light_count;") != deren::utility::vessel::string::npos);
    // The host/shader validity lane must gate the history texture fetch, not only its image layout.
    auto const temporal = read_text(root + "/source/shaders/lighting/megalights_temporal.slang");
    CHECK(temporal.find("pc.extents.w > 0.5") != deren::utility::vessel::string::npos);
    auto const temporal_host = read_text(root + "/source/engine/pass/megalights_temporal.cpp");
    CHECK(temporal_host.find("this->pass_frame.history_valid ? 1.0f : 0.0f") != deren::utility::vessel::string::npos);
    return deren::vk_test::finish("test_shader_sources");
}
