// Headless unit tests: THE DOCUMENTS ARE PART OF THE BUILD =====================================
// The durable reasoning of this project lives in `docs/*.md`, and since the mesh migration those
// files are also doxygen INPUT: they are rendered into the HTML manual and, through docs/latex, into
// refman.pdf. That makes them a BUILD INPUT whose failure modes nobody sees in a diff - which is what
// this test is for. Five invariants for the documents, plus one for the CI workflow, and each one is
// a mistake that has actually happened here or would be silent:
//
//  1. EVERY `docs/*.md` IS IN Doxyfile's INPUT, or named in the exception list below with a reason.
//     MEASURED: nine of twelve were outside it until commit 0ba6d22 - mesh_shaders.md (the mesh
//     migration's own record), slang_migration.md and seven more were simply not in the manual, and
//     nothing anywhere said so. A directory input is not the fix: `docs/` also holds doxygen's own
//     output and two sets of third-party material, which is why the list is explicit.
//  2. EVERY INPUT DOCUMENT IS ASCII. The manual's LaTeX runs these files through pdflatex, and a
//     non-ASCII character aborts or corrupts that build (the rule docs/shaders.md states). The four
//     documents added in 0ba6d22 had to be converted first - em dashes, arrows, section signs, a
//     box-drawing tree and U+1D40 - and `docs/mainpage.md` still had three em dashes. CI has no
//     LaTeX, so the checkable half of that rule is checked here, by byte.
//  3. EVERY `](#anchor)` LINK RESOLVES to a heading in the SAME file. doxygen accepts an anchor it
//     cannot resolve without a warning of its own, so renaming a heading leaves a link that quietly
//     goes nowhere - and this repo's documents cross-reference themselves by section heavily.
//  4. CODE FENCES AND `@code`/`@endcode` ARE BALANCED. A truncated document, or a diff that eats one
//     fence, changes what doxygen thinks is code: the manual is mangled rather than the build failing.
//  5. EVERY `heap_slots_<name>` THE DOCUMENTS NAME IS A DECLARED SLOT, in the host's grid or in the
//     shaders' constants. A renamed slot leaves the documents describing a resource that does not
//     exist, and a heap has no reflection to notice: those constants ARE the source of truth (which is
//     the same reason test_render_resources compares the two files that hold them).
//  6. THE CI WORKFLOW'S TEST LIST EQUALS CMakeLists' `VR_TEST_TARGETS`. The workflow says so in a
//     comment and the comment is right about why: a target missing from the `--target` list is an
//     executable ctest cannot find, and the job goes red as "Not Run". MEASURED: it had already
//     drifted - ten targets in CMake, nine in the ASan job (`test_meshlet` missing) - which is the
//     same failure the comment describes happening twice before this list existed.
//
// NOT CHECKED HERE, deliberately: whether a link's TARGET FILE exists (the documents link to code
// they describe, and several name files that were deliberately removed and marked as history - a rule
// for that needs the history to be machine-readable first), and anything about the rendered manual,
// which needs doxygen and pdflatex and is therefore the build_docs step's business rather than CI's.
#include "vk_test.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

import deren.utility.shared;

namespace {
    /// `docs/*.md` that is deliberately NOT in Doxyfile's INPUT, with the reason it is not. Empty on
    /// purpose: every document on disk is in the manual today, and adding a name here is the explicit
    /// decision that it should not be - which is the point of the list being here rather than implied.
    struct exception_entry {
        char const* name;
        char const* reason;
    };
    // `deren::utility::vessel::array<..., 0>`, not `exception_entry[]`: the list is empty BY DESIGN above, and a zero-length array is
    // an MSVC error (C2466 + C2131) where clang accepts it as an extension. The range-for below reads it unchanged.
    constexpr deren::utility::vessel::array<exception_entry, 0> not_in_manual = {};

    /// `heap_slots_<name>` spellings the documents use that are NOT slot names, with the reason.
    struct identifier_exception {
        char const* name;
        char const* reason;
    };
    constexpr identifier_exception not_a_slot[] = {
        {"heap_slots_bloom_lN", "a placeholder for the four bloom levels in a table, not a declaration"},
    };

    deren::utility::vessel::string read_text(std::filesystem::path const& path) {
        std::ifstream file(path, std::ios::binary);
        CHECK_MSG(file.is_open(), path.string().c_str());
        deren::utility::vessel::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    deren::utility::vessel::vector<deren::utility::vessel::string> read_lines(std::filesystem::path const& path) {
        deren::utility::vessel::vector<deren::utility::vessel::string> lines;
        deren::utility::vessel::istringstream stream(read_text(path));
        deren::utility::vessel::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
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

    /// THE ANCHOR A MARKDOWN HEADING GETS: lowercase, everything that is not a letter, a digit, a space
    /// or a hyphen dropped, spaces turned into hyphens. "## 8. Animations" answers `#8-animations`.
    deren::utility::vessel::string heading_anchor(deren::utility::vessel::string heading) {
        deren::utility::vessel::string anchor;
        for (char const c : trim(std::move(heading))) {
            auto const byte = static_cast<uint8_t>(c);
            if (byte >= 'A' && byte <= 'Z') {
                anchor.push_back(static_cast<char>(byte - 'A' + 'a'));
            } else if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-') {
                anchor.push_back(c);
            } else if (byte == ' ' || byte == '\t') {
                anchor.push_back('-');
            }
        }
        return anchor;
    }

    /// every `heap_slots_<name>` spelling inside @p text, deduplicated. The name is read as [A-Za-z0-9_]
    /// rather than as the lowercase convention, so that a document's METAVARIABLE (`heap_slots_bloom_lN`,
    /// where the capital N stands for a level) is read whole and can be named in the exception list
    /// exactly as the document spells it.
    deren::utility::vessel::set<deren::utility::vessel::string> slots_named_in(deren::utility::vessel::string const& text) {
        deren::utility::vessel::set<deren::utility::vessel::string> names;
        std::size_t at = text.find("heap_slots_");
        while (at != deren::utility::vessel::string::npos) {
            std::size_t end = at + deren::utility::vessel::string("heap_slots_").size();
            while (end < text.size()) {
                char const c = text[end];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
                    ++end;
                } else {
                    break;
                }
            }
            names.insert(text.substr(at, end - at));
            at = text.find("heap_slots_", end);
        }
        return names;
    }

    /// the lines of Doxyfile's INPUT value, continuations joined: every token it lists
    deren::utility::vessel::vector<deren::utility::vessel::string> doxygen_input_tokens(deren::utility::vessel::string const& doxyfile_text) {
        deren::utility::vessel::vector<deren::utility::vessel::string> tokens;
        deren::utility::vessel::istringstream stream(doxyfile_text);
        deren::utility::vessel::string line;
        bool in_input = false;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!in_input) {
                deren::utility::vessel::string const trimmed = trim(line);
                std::size_t const equals = trimmed.find('=');
                if (equals == deren::utility::vessel::string::npos || trim(trimmed.substr(0, equals)) != "INPUT") {
                    continue; // not the INPUT tag (INPUT_ENCODING and friends are different tags)
                }
                in_input = true;
                line = trimmed.substr(equals + 1u);
            }
            bool continues = false;
            deren::utility::vessel::string const trimmed = trim(line);
            if (!trimmed.empty() && trimmed.back() == '\\') {
                continues = true;
                line = trimmed.substr(0, trimmed.size() - 1u);
            } else if (!trimmed.empty()) {
                line = trimmed;
            }
            deren::utility::vessel::istringstream values(line);
            deren::utility::vessel::string token;
            while (values >> token) {
                tokens.push_back(token);
            }
            if (!continues) {
                break;
            }
        }
        return tokens;
    }

    /// the `test_*` targets CMakeLists' VR_TEST_TARGETS declares
    deren::utility::vessel::set<deren::utility::vessel::string> vr_test_targets(deren::utility::vessel::string const& cmake_text) {
        deren::utility::vessel::set<deren::utility::vessel::string> targets;
        deren::utility::vessel::istringstream stream(cmake_text);
        deren::utility::vessel::string line;
        while (std::getline(stream, line)) {
            std::size_t const at = line.find("VR_TEST_TARGETS");
            if (at == deren::utility::vessel::string::npos || line.find("set(", at == 0 ? 0u : at - 4u) == deren::utility::vessel::string::npos) {
                continue;
            }
            std::size_t token = line.find("test_", at);
            while (token != deren::utility::vessel::string::npos) {
                std::size_t end = token;
                while (end < line.size()) {
                    char const c = line[end];
                    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
                        ++end;
                    } else {
                        break;
                    }
                }
                targets.insert(line.substr(token, end - token));
                token = line.find("test_", end);
            }
            if (!targets.empty()) {
                break;
            }
        }
        return targets;
    }

    /// the `test_*` targets the CI workflow builds: EVERY `--target` block's value, merged. Merging rather than
    /// taking the first is the fix for a measured own-goal: the workflow's FIRST `--target` is the clang-format
    /// job's (`cmake --build build --target clang-format-check`), whose value is on its own line and ends the
    /// block - so a first-only parser read an empty list and reported every target as missing.
    deren::utility::vessel::set<deren::utility::vessel::string> ci_built_targets(deren::utility::vessel::string const& workflow_text) {
        deren::utility::vessel::set<deren::utility::vessel::string> targets;
        deren::utility::vessel::vector<deren::utility::vessel::string> const lines = [&workflow_text] {
            deren::utility::vessel::vector<deren::utility::vessel::string> out;
            deren::utility::vessel::istringstream stream(workflow_text);
            deren::utility::vessel::string line;
            while (std::getline(stream, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                out.push_back(line);
            }
            return out;
        }();
        auto collect = [&targets](deren::utility::vessel::string const& text) {
            std::size_t token = text.find("test_");
            while (token != deren::utility::vessel::string::npos) {
                std::size_t end = token;
                while (end < text.size()) {
                    char const c = text[end];
                    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
                        ++end;
                    } else {
                        break;
                    }
                }
                targets.insert(text.substr(token, end - token));
                token = text.find("test_", end);
            }
        };
        for (std::size_t i = 0; i < lines.size(); ++i) {
            std::size_t const marker = lines[i].find("--target");
            if (marker == deren::utility::vessel::string::npos) {
                continue;
            }
            collect(lines[i].substr(marker + deren::utility::vessel::string("--target").size())); // a value on the same line
            // ... and the value continues on the following lines until the YAML block ends: a blank line, a
            // comment, or a line that is a `key:` at any indent (the next step of the job)
            for (std::size_t j = i + 1u; j < lines.size(); ++j) {
                deren::utility::vessel::string const trimmed = trim(lines[j]);
                if (trimmed.empty() || trimmed[0] == '#' || trimmed.find(':') != deren::utility::vessel::string::npos) {
                    break;
                }
                collect(trimmed);
            }
        }
        return targets;
    }

    /// the `test_*` executables CMakeLists creates with a literal name (the VR_TEST_TARGETS loop's
    /// `add_executable(${test_target} ...)` is not a name and is not matched).
    deren::utility::vessel::set<deren::utility::vessel::string> literal_test_executables(deren::utility::vessel::string const& cmake_text) {
        deren::utility::vessel::set<deren::utility::vessel::string> targets;
        deren::utility::vessel::istringstream stream(cmake_text);
        deren::utility::vessel::string line;
        while (std::getline(stream, line)) {
            std::size_t const at = line.find("add_executable(");
            if (at == deren::utility::vessel::string::npos) {
                continue;
            }
            std::size_t const start = at + deren::utility::vessel::string("add_executable(").size();
            std::size_t end = start;
            while (end < line.size() && line[end] != ' ' && line[end] != ')') {
                ++end;
            }
            deren::utility::vessel::string const name = line.substr(start, end - start);
            if (name.find("test_") == 0u) {
                targets.insert(name);
            }
        }
        return targets;
    }

    /// the device lane's targets: CMakeLists' `set(VR_DEVICE_TEST_TARGETS ...)` list (one item per line).
    deren::utility::vessel::set<deren::utility::vessel::string> device_test_targets(deren::utility::vessel::string const& cmake_text) {
        deren::utility::vessel::set<deren::utility::vessel::string> targets;
        deren::utility::vessel::istringstream stream(cmake_text);
        deren::utility::vessel::string line;
        bool in_list = false;
        while (std::getline(stream, line)) {
            if (!in_list) {
                in_list = line.find("set(VR_DEVICE_TEST_TARGETS") != deren::utility::vessel::string::npos;
                if (!in_list) {
                    continue;
                }
            }
            std::size_t token = line.find("test_");
            while (token != deren::utility::vessel::string::npos) {
                std::size_t end = token;
                while (end < line.size()) {
                    char const c = line[end];
                    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
                        ++end;
                    } else {
                        break;
                    }
                }
                targets.insert(line.substr(token, end - token));
                token = line.find("test_", end);
            }
            if (line.find("set(VR_DEVICE_TEST_TARGETS") == deren::utility::vessel::string::npos &&
                line.find(')') != deren::utility::vessel::string::npos) {
                break;
            }
        }
        return targets;
    }

    /// the relative targets a document's `](...)` links and images point at, with fenced code blocks removed:
    /// a link inside a ``` sample is a sample, not a promise that the file exists.
    deren::utility::vessel::vector<deren::utility::vessel::string> markdown_link_targets(deren::utility::vessel::string const& text) {
        deren::utility::vessel::vector<deren::utility::vessel::string> targets;
        deren::utility::vessel::istringstream stream(text);
        deren::utility::vessel::string line;
        bool in_fence = false;
        while (std::getline(stream, line)) {
            if (line.rfind("```", 0) == 0 || line.rfind("~~~", 0) == 0) {
                in_fence = !in_fence;
                continue;
            }
            if (in_fence) {
                continue;
            }
            std::size_t at = 0;
            while ((at = line.find("](", at)) != deren::utility::vessel::string::npos) {
                std::size_t const start = at + 2u;
                std::size_t end = start;
                while (end < line.size() && line[end] != ')' && line[end] != ' ' && line[end] != '\t') {
                    ++end;
                }
                if (end > start) {
                    targets.push_back(line.substr(start, end - start));
                }
                at = end;
            }
        }
        return targets;
    }

    /// true when a link target is not a path inside the repository: an anchor, or any scheme-qualified target
    /// (`https:`, `mailto:`, ... - a colon before the first slash).
    bool is_external_link(deren::utility::vessel::string const& target) {
        if (target.empty() || target[0] == '#') {
            return true;
        }
        std::size_t const colon = target.find(':');
        std::size_t const slash = target.find('/');
        return colon != deren::utility::vessel::string::npos &&
               (slash == deren::utility::vessel::string::npos || colon < slash);
    }

    bool is_exception(deren::utility::vessel::string const& name) {
        for (exception_entry const& entry : not_in_manual) {
            if (name == entry.name) {
                return true;
            }
        }
        return false;
    }

    bool is_not_a_slot(deren::utility::vessel::string const& name) {
        for (identifier_exception const& entry : not_a_slot) {
            if (name == entry.name) {
                return true;
            }
        }
        return false;
    }
} // namespace

int32_t main() {
    std::filesystem::path const root = VR_TEST_SOURCE_DIR;
    std::filesystem::path const docs = root / "docs";

    // ---- 1. every document is in the manual, and everything in the manual exists ----
    deren::utility::vessel::vector<deren::utility::vessel::string> const inputs = doxygen_input_tokens(read_text((root / "Doxyfile").string()));
    deren::utility::vessel::set<deren::utility::vessel::string> input_docs;
    for (deren::utility::vessel::string const& token : inputs) {
        deren::utility::vessel::string const prefix = "./docs/";
        if (token.rfind(prefix, 0) == 0 && token.ends_with(".md")) {
            input_docs.insert(token.substr(prefix.size()));
        }
    }
    CHECK_MSG(!input_docs.empty(), "Doxyfile's INPUT lists at least one ./docs/*.md");
    deren::utility::vessel::vector<deren::utility::vessel::string> on_disk;
    for (auto const& file : std::filesystem::directory_iterator(docs)) {
        if (file.is_regular_file() && file.path().extension() == ".md") {
            on_disk.emplace_back(file.path().filename().string());
        }
    }
    CHECK_MSG(!on_disk.empty(), "docs/ holds markdown documents");
    for (deren::utility::vessel::string const& name : on_disk) {
        if (is_exception(name)) {
            continue;
        }
        CHECK_MSG(input_docs.contains(name), ("a document is not in Doxyfile's INPUT, so it is not in the manual (add it, or add it to this test's exception list with a reason): docs/" + name).c_str());
    }
    for (deren::utility::vessel::string const& name : input_docs) {
        CHECK_MSG(std::filesystem::exists(docs / name), ("Doxyfile's INPUT names a document that does not exist: docs/" + name).c_str());
    }

    // ---- 2..5 are per document ----
    std::size_t checked_anchors = 0;
    for (deren::utility::vessel::string const& name : input_docs) {
        std::filesystem::path const path = docs / name;
        if (!std::filesystem::exists(path)) {
            continue; // reported above
        }
        deren::utility::vessel::string const text = read_text(path.string());
        deren::utility::vessel::string const label = "docs/" + name;

        // 2. ASCII: the manual's LaTeX feeds these files to pdflatex, which the project's own rule says
        //    must never see a non-ASCII byte (docs/shaders.md). Report WHERE, so the fix is one edit.
        {
            std::size_t line = 1;
            for (std::size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\n') {
                    ++line;
                    continue;
                }
                auto const byte = static_cast<uint8_t>(text[i]);
                if (byte >= 0x80u) {
                    deren::utility::vessel::string const where = label + ":" + deren::utility::vessel::to_string(line) + " has a non-ASCII byte (the LaTeX manual runs these documents through pdflatex)";
                    CHECK_MSG(false, where.c_str());
                    break;
                }
            }
            CHECK_MSG(true, (label + " is ASCII").c_str());
        }

        // 3. anchors resolve inside the document, headings and doxygen sections both answering
        {
            deren::utility::vessel::set<deren::utility::vessel::string> anchors;
            for (deren::utility::vessel::string const& line : read_lines(path.string())) {
                deren::utility::vessel::string const trimmed = trim(line);
                if (!trimmed.empty() && trimmed[0] == '#') {
                    std::size_t hashes = 0;
                    while (hashes < trimmed.size() && trimmed[hashes] == '#') {
                        ++hashes;
                    }
                    if (hashes <= 6u) {
                        deren::utility::vessel::string title = trim(trimmed.substr(hashes));
                        while (!title.empty() && title.back() == '#') {
                            title.pop_back();
                        }
                        anchors.insert(heading_anchor(title));
                    }
                }
                for (deren::utility::vessel::string const marker : {"@section ", "@subsection "}) {
                    std::size_t const at = trimmed.find(marker);
                    if (at == deren::utility::vessel::string::npos || at != 0u) {
                        continue;
                    }
                    deren::utility::vessel::istringstream value(trimmed.substr(deren::utility::vessel::string(marker).size()));
                    deren::utility::vessel::string name_token;
                    value >> name_token;
                    anchors.insert(name_token);
                }
            }
            std::size_t at = text.find("](#");
            while (at != deren::utility::vessel::string::npos) {
                std::size_t const end = text.find(')', at);
                if (end == deren::utility::vessel::string::npos) {
                    break;
                }
                // `](#anchor)`: the three bytes before the anchor are ']', '(' and '#', and the stored
                // anchors carry no '#'
                deren::utility::vessel::string const anchor = text.substr(at + 3u, end - at - 3u);
                ++checked_anchors;
                CHECK_MSG(anchors.contains(anchor), ("a markdown anchor does not resolve to a heading in the same document: " + label + " -> " + anchor).c_str());
                at = text.find("](#", end);
            }
        }

        // 4. fences and doxygen code blocks are balanced: an odd count means doxygen reads prose as code
        {
            std::size_t fences = 0;
            std::size_t code = 0;
            std::size_t endcode = 0;
            for (deren::utility::vessel::string const& line : read_lines(path.string())) {
                deren::utility::vessel::string const trimmed = trim(line);
                if (trimmed.rfind("```", 0) == 0) {
                    ++fences;
                }
                std::size_t at = trimmed.find("@code");
                while (at != deren::utility::vessel::string::npos) {
                    ++code;
                    at = trimmed.find("@code", at + 5u);
                }
                at = trimmed.find("@endcode");
                while (at != deren::utility::vessel::string::npos) {
                    ++endcode;
                    at = trimmed.find("@endcode", at + 8u);
                }
            }
            CHECK_MSG(fences % 2u == 0u, ("a code fence is unclosed, so doxygen reads prose as code: " + label).c_str());
            CHECK_MSG(code == endcode, ("@code and @endcode are unbalanced: " + label).c_str());
        }

        // 5. the slot names the documents quote exist: the host grid and the shaders' constants are the
        //    two files that declare them (test_render_resources owns the comparison between those two)
        {
            deren::utility::vessel::string const grid = read_text((root / "source" / "engine" / "render_layout" / "render_layout.cppm").string()) +
                                                        read_text((root / "source" / "shaders" / "common/heap_slot_constants.slang").string());
            deren::utility::vessel::set<deren::utility::vessel::string> const declared = slots_named_in(grid);
            for (deren::utility::vessel::string const& slot : slots_named_in(text)) {
                if (is_not_a_slot(slot)) {
                    continue;
                }
                CHECK_MSG(declared.contains(slot), ("a document names a heap slot that no file declares: " + slot + " (in " + label + ")").c_str());
            }
        }
    }
    CHECK_MSG(checked_anchors >= 1u, "the documents contain at least one markdown anchor link to check");

    // ---- 6. the CI workflow builds every target CMake registers (and nothing CMake does not) ----
    {
        deren::utility::vessel::set<deren::utility::vessel::string> const registered = vr_test_targets(read_text((root / "CMakeLists.txt").string()));
        deren::utility::vessel::set<deren::utility::vessel::string> const built = ci_built_targets(read_text((root / ".github" / "workflows" / "ci.yml").string()));
        CHECK_MSG(registered.size() >= 10u, "VR_TEST_TARGETS parses as the full list of targets");
        CHECK_MSG(!built.empty(), "the CI workflow's --target list parses");
        for (deren::utility::vessel::string const& target : registered) {
            CHECK_MSG(built.contains(target), ("a test target CMake registers is not built by the CI workflow, so ctest there reports it as Not Run: " + target).c_str());
        }
        for (deren::utility::vessel::string const& target : built) {
            CHECK_MSG(registered.contains(target), ("the CI workflow builds a test target CMake does not register: " + target).c_str());
        }
    }

    // ---- 7. every literal test executable CMakeLists creates is registered, headless or device ----
    //
    // The failure this exists for is not hypothetical: `add_executable(test_radiance_capture)` sat behind an
    // `if(EXISTS ...)` guard, the source was dropped, and the suite silently became one test smaller with no
    // failure anywhere. A test executable is now either on the headless list, on the device lane's list, or a
    // named exception - never invisible.
    {
        deren::utility::vessel::string const cmake_text = read_text((root / "CMakeLists.txt").string());
        deren::utility::vessel::set<deren::utility::vessel::string> const device = device_test_targets(cmake_text);
        CHECK_MSG(!device.empty(), "CMakeLists' VR_DEVICE_TEST_TARGETS parses as the device lane's list");
        deren::utility::vessel::set<deren::utility::vessel::string> const literal = literal_test_executables(cmake_text);
        CHECK_MSG(literal.size() >= 7u, "CMakeLists' literal `add_executable(test_*)` lines parse");
        deren::utility::vessel::set<deren::utility::vessel::string> registered = vr_test_targets(cmake_text);
        for (deren::utility::vessel::string const& target : device) {
            registered.insert(target);
        }
        // The boundary spike is report-shaped rather than pass/fail: CMakeLists says so where it is created,
        // and it stays outside both lists on purpose. Named here so that is a recorded decision.
        registered.insert("test_backend_boundary_spike");
        for (deren::utility::vessel::string const& target : literal) {
            CHECK_MSG(registered.contains(target),
                      ("CMakeLists creates a test executable that is in neither VR_TEST_TARGETS nor VR_DEVICE_TEST_TARGETS: " + target).c_str());
        }
    }

    // ---- 8. every document link points at a file the repository contains ----
    //
    // The measured failure: docs/render_resource_inventory.md linked
    // `reviews/2026-10-14-shared-assets.md`, a path under a deliberately gitignored directory, so the link
    // was dead in every clone while the document claimed a complete audit. Links inside fenced code blocks
    // are skipped (they are samples); anchors and external targets are skipped.
    {
        std::size_t checked_links = 0;
        for (auto const& file : std::filesystem::directory_iterator(docs)) {
            if (!file.is_regular_file() || file.path().extension() != ".md") {
                continue;
            }
            deren::utility::vessel::string const text = read_text(file.path().string());
            for (deren::utility::vessel::string const& target : markdown_link_targets(text)) {
                if (is_external_link(target)) {
                    continue;
                }
                deren::utility::vessel::string path = target;
                std::size_t const hash = path.find('#');
                if (hash != deren::utility::vessel::string::npos) {
                    path.resize(hash);
                }
                if (path.empty()) {
                    continue;
                }
                ++checked_links;
                std::filesystem::path const resolved = (file.path().parent_path() / std::filesystem::path(path.c_str())).lexically_normal();
                CHECK_MSG(std::filesystem::exists(resolved),
                          ("a document links a file that is not in the repository: " + target + " (in docs/" + file.path().filename().string().c_str() + ")").c_str());
            }
        }
        CHECK_MSG(checked_links >= 20u, "the documents' relative links were actually checked");
    }

    for (auto const* file : {"source/engine/runtime/runtime.cpp", "source/engine/runtime/runtime.constructor.cppm",
                             "source/engine/runtime/runtime.frames.cppm", "source/engine/runtime/runtime.declarations.cppm",
                             "source/engine/runtime/runtime.probes.cppm",
                             "source/engine/pass/software_visibility.cpp"}) {
        auto text = read_text((root / file).string());
        for (auto const* physical : {"heap_slot_offset", "heap_slot_stride", "rhi::descriptor_heap", "heap_buffer_write_info", "heap_image_write_info"})
            CHECK_MSG(text.find(physical) == text.npos, (deren::utility::vessel::string(file) + " retains physical descriptor access: " + physical).c_str());
    }
    return deren::vk_test::finish("test_docs");
}
