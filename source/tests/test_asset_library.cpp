// Headless unit tests: asset_library (pure CPU) ================================
// The test executable runs in the build's per-test scratch directory (see the CMakeLists.txt
// VR_BUILD_TESTS block), so every fixture here is created under the working directory and removed
// again. The production catalogue under asset/ is deliberately not required by this test.
#include "vk_test.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

import deren.asset_library;

namespace {
    namespace fs = std::filesystem;

    void write_file(fs::path const& path, std::string_view const text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path) << text;
    }

    bool any_warning_contains(deren::asset_library::asset_library const& library, std::string_view const needle) {
        for (std::string const& warning : library.warnings()) {
            if (warning.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    /// AN ASSET IS A FOLDER: the scanner discovers `<root>/<folder>/asset.toml`, validates the
    /// model inside that folder, and exposes a stable id. `library.toml` only carries catalogue
    /// policy (default/scan_depth/ignore); it never names an asset file.
    void test_scan_discovers_manifests_and_default() {
        fs::path const root = "asset-library-scan";
        fs::remove_all(root);
        write_file(root / "library.toml", "version = 1\ndefault = \"alpha\"\nscan_depth = 2\n");
        write_file(root / "alpha" / "asset.toml",
                   "id = \"alpha\"\nname = \"Alpha\"\ntype = \"pbr\"\nmodel = \"alpha.glb\"\ntags = [\"pbr\"]\n");
        write_file(root / "alpha" / "alpha.glb", "glb");
        write_file(root / "nested" / "beta" / "asset.toml",
                   "id = \"beta\"\nname = \"Beta\"\ntype = \"toon\"\nmodel = \"beta.glb\"\n[toon]\ngame_toon = true\nprofile = \"game_toon\"\n");
        write_file(root / "nested" / "beta" / "beta.glb", "glb");

        deren::asset_library::asset_library const library = deren::asset_library::asset_library::scan(root);
        CHECK_MSG(library.valid(), library.error().c_str());
        CHECK(library.entries().size() == 2);
        CHECK(library.find("alpha") != nullptr);
        CHECK(library.find("beta") != nullptr);
        CHECK(library.default_asset() != nullptr);
        CHECK(library.default_asset()->id == "alpha");

        deren::asset_library::asset_entry const* beta = library.find("beta");
        CHECK(beta != nullptr);
        if (beta != nullptr) {
            CHECK(beta->type == deren::asset_library::asset_type::toon);
            CHECK(beta->game_toon);
            CHECK(beta->toon_profile == "game_toon");
            CHECK(beta->model.filename() == "beta.glb");
        }
        fs::remove_all(root);
    }

    /// A MANIFEST THAT REFERENCES SOMETHING OUTSIDE ITS FOLDER IS INVALID: path traversal in the
    /// model field must not turn a data file into a read of the whole disk.
    void test_path_traversal_is_rejected() {
        fs::path const root = "asset-library-traversal";
        fs::remove_all(root);
        write_file(root / "library.toml", "version = 1\n");
        write_file(root / "inside" / "asset.toml", "id = \"inside\"\nmodel = \"../outside.glb\"\n");
        write_file(root / "outside.glb", "glb");

        deren::asset_library::asset_library const library = deren::asset_library::asset_library::scan(root);
        CHECK(!library.valid());
        CHECK(any_warning_contains(library, "must not contain '..'"));
        fs::remove_all(root);
    }

    /// A MISSING MODEL SKIPS THE ENTRY WITH A WARNING; it is a catalogue authoring error, not a
    /// crash at load time.
    void test_missing_model_is_reported() {
        fs::path const root = "asset-library-missing";
        fs::remove_all(root);
        write_file(root / "library.toml", "version = 1\n");
        write_file(root / "broken" / "asset.toml", "id = \"broken\"\nmodel = \"missing.glb\"\n");

        deren::asset_library::asset_library const library = deren::asset_library::asset_library::scan(root);
        CHECK(!library.valid());
        CHECK(any_warning_contains(library, "does not exist"));
        fs::remove_all(root);
    }

    /// DUPLICATE IDS ARE REFUSED, first one in sorted folder order wins, and the warning names the
    /// id. Silent last-wins would make `--asset <id>` depend on directory iteration order.
    void test_duplicate_ids_are_skipped() {
        fs::path const root = "asset-library-duplicate";
        fs::remove_all(root);
        write_file(root / "library.toml", "version = 1\n");
        write_file(root / "a" / "asset.toml", "id = \"same\"\nmodel = \"a.glb\"\n");
        write_file(root / "a" / "a.glb", "glb");
        write_file(root / "b" / "asset.toml", "id = \"same\"\nmodel = \"b.glb\"\n");
        write_file(root / "b" / "b.glb", "glb");

        deren::asset_library::asset_library const library = deren::asset_library::asset_library::scan(root);
        CHECK_MSG(library.valid(), library.error().c_str());
        CHECK(library.entries().size() == 1);
        CHECK(library.entries().front().model.filename() == "a.glb");
        CHECK(any_warning_contains(library, "duplicate asset id"));
        fs::remove_all(root);
    }

    /// AN UNKNOWN DEFAULT FALLS BACK TO THE FIRST SORTED ENTRY WITH A WARNING; a library with no
    /// entries is an error, so startup can name the bad root instead of loading nothing.
    void test_default_fallback_and_empty_library() {
        fs::path const root = "asset-library-default";
        fs::remove_all(root);
        write_file(root / "library.toml", "version = 1\ndefault = \"nope\"\n");
        write_file(root / "only" / "asset.toml", "id = \"only\"\nmodel = \"only.glb\"\n");
        write_file(root / "only" / "only.glb", "glb");

        deren::asset_library::asset_library const library = deren::asset_library::asset_library::scan(root);
        CHECK_MSG(library.valid(), library.error().c_str());
        CHECK(library.default_asset() != nullptr);
        CHECK(library.default_asset()->id == "only");
        CHECK(any_warning_contains(library, "default asset 'nope' not found"));
        fs::remove_all(root);

        deren::asset_library::asset_library const empty = deren::asset_library::asset_library::scan("asset-library-empty");
        CHECK(!empty.valid());
        CHECK(empty.error().find("not a directory") != std::string::npos);
    }
} // namespace

int main() {
    test_scan_discovers_manifests_and_default();
    test_path_traversal_is_rejected();
    test_missing_model_is_reported();
    test_duplicate_ids_are_skipped();
    test_default_fallback_and_empty_library();
    return deren::vk_test::finish("test_asset_library");
}
