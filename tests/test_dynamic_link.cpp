// Headless unit tests: deren.utility.dynamic_link (RHI plan v4 §7.4) ============
// The loader is the primitive the backend boundary is built on, so what is checked here is the
// contract rather than an implementation detail: a file that is not there is a returned error and
// not a crash, the platform suffix is completed when the caller leaves it off, a missing symbol is
// an error, a relative bare name is found, the library is unloaded when it goes out of scope, a
// moved-from library is empty instead of dangling, and each symbol lookup goes through the
// backend's own deleter exactly once.
//
// The library under test is tests/probe_backend.cpp, which CMake builds twice from one source
// file: the static half is linked into this test (so abi_export.hpp's static branch is exercised
// by the linker) and the DLL half is only ever opened at run time (the shared branch). The two
// halves must answer the same, which is the check that the export keywords do what they say.
#include "vk_test.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

// The probe backend's C ABI, declared exactly as tests/probe_backend.cpp defines it. The class is
// deliberately defined in both translation units: the object is created and destroyed inside the
// backend and the front end only calls it through the base (plan §7.4.4).
struct api_core {
    virtual ~api_core() = default;
    virtual int value() const noexcept = 0;
};

extern "C" unsigned int deren_abi_version();
extern "C" api_core* deren_make_api_core(unsigned int abi, int* out_error);
extern "C" void deren_destroy_api_core(api_core* core);

import deren.utility.dynamic_link;

namespace {

    namespace fs = std::filesystem;

    // From CMake ($<TARGET_FILE_NAME:probe_backend>): the DLL suffix is platform-dependent, so the
    // test does not hard-code it.
    constexpr std::string_view probe_file_name = VR_TEST_PROBE_BACKEND_FILE_NAME;
    constexpr std::string_view missing_file_name = "deren_probe_backend_that_does_not_exist.dll";
    constexpr std::string_view unload_probe_file_name = "deren_probe_backend_unload_probe.dll";

    using make_core_fn = api_core* (*)(unsigned int, int*);
    using destroy_core_fn = void (*)(api_core*);

    /** @brief a resolved C symbol: void const* to function pointer, constness dropped on purpose */
    template <typename function>
    function as_function(void const* address) {
        return reinterpret_cast<function>(const_cast<void*>(address));
    }

    /** @brief owns one api_core and counts how often it called the backend's deleter */
    class core_owner {
    public:
        core_owner(api_core* core, destroy_core_fn destroy, int* destroy_calls) noexcept
            : core_(core)
            , destroy_(destroy)
            , destroy_calls_(destroy_calls) {
        }
        core_owner(core_owner const&) = delete;
        core_owner& operator=(core_owner const&) = delete;
        ~core_owner() {
            if (this->core_ != nullptr) {
                ++*this->destroy_calls_;
                this->destroy_(this->core_);
            }
        }

        [[nodiscard]] api_core* get() const noexcept {
            return this->core_;
        }

    private:
        api_core* core_ = nullptr;
        destroy_core_fn destroy_ = nullptr;
        int* destroy_calls_ = nullptr;
    };

    void test_a_missing_file_is_a_returned_error(fs::path const& directory) {
        auto const missing =
            deren::utility::dynamic_link::load((directory / std::string{missing_file_name}).string());
        CHECK(!missing.has_value());
        if (missing.has_value()) {
            return;
        }
        // The system's error number AND its text survive: a caller can branch on the number
        // (2 = ERROR_FILE_NOT_FOUND, 3 = ERROR_PATH_NOT_FOUND, 126 = ERROR_MOD_NOT_FOUND) and log
        // the text as it came from the OS.
        CHECK(missing.error().code != 0);
        CHECK(!missing.error().message.empty());
    }

    void test_the_platform_suffix_is_completed(fs::path const& directory) {
        fs::path const complete = directory / std::string{probe_file_name};
        fs::path const without_suffix = complete.parent_path() / complete.stem();
        auto const loaded = deren::utility::dynamic_link::load(without_suffix.string());
        CHECK(loaded.has_value());
        if (loaded.has_value()) {
            CHECK(loaded->native_handle() != nullptr);
        }
    }

    void test_loading_the_probe_dll_and_using_its_symbols(fs::path const& directory) {
        auto loaded = deren::utility::dynamic_link::load((directory / std::string{probe_file_name}).string());
        CHECK(loaded.has_value());
        if (!loaded.has_value()) {
            return;
        }
        CHECK(loaded->native_handle() != nullptr);

        auto const version = loaded->symbol("deren_abi_version");
        CHECK(version.has_value());
        CHECK(!loaded->symbol("deren_no_such_symbol_in_the_probe_backend").has_value());
        CHECK(!loaded->symbol("").has_value());
        if (version.has_value()) {
            auto const abi_version = as_function<unsigned int (*)()>(version.value());
            CHECK(abi_version() == 1u);
            CHECK(abi_version() == deren_abi_version()); // the DLL and the static half agree
        }

        auto const make = loaded->symbol("deren_make_api_core");
        auto const destroy = loaded->symbol("deren_destroy_api_core");
        CHECK(make.has_value());
        CHECK(destroy.has_value());
        if (!make.has_value() || !destroy.has_value()) {
            return;
        }
        auto const make_core = as_function<make_core_fn>(make.value());
        auto const destroy_core = as_function<destroy_core_fn>(destroy.value());

        int destroy_calls = 0;
        {
            int make_error = -1;
            core_owner const core{make_core(1u, &make_error), destroy_core, &destroy_calls};
            CHECK(make_error == 0);
            CHECK(core.get() != nullptr);
            if (core.get() != nullptr) {
                CHECK(core.get()->value() == 42);
            }
        }
        CHECK(destroy_calls == 1); // the deleter ran exactly once, inside the loaded library

        // An ABI mismatch is refused before an object exists, and reports the plan's code 7
        // rather than a system error.
        int mismatch_error = -1;
        CHECK(make_core(2u, &mismatch_error) == nullptr);
        CHECK(mismatch_error == 7);

        // A moved-from library is empty (not a second owner of the same handle), and the symbols
        // taken from the destination keep working.
        deren::utility::dynamic_link::library moved{std::move(*loaded)};
        CHECK(moved.native_handle() != nullptr);
        CHECK(loaded->native_handle() == nullptr);
        auto const moved_version = moved.symbol("deren_abi_version");
        CHECK(moved_version.has_value());
        if (moved_version.has_value()) {
            CHECK(as_function<unsigned int (*)()>(moved_version.value())() == 1u);
        }
    }

    void test_the_library_is_unloaded_when_it_goes_out_of_scope(fs::path const& directory) {
        // A copy in the *current* directory, opened by its bare name: this is the relative branch
        // (classic search order) and it doubles as the unload proof - on Windows a loaded image
        // holds its file, so the file only becomes deletable after the destructor called
        // FreeLibrary.
        fs::path const copy = fs::current_path() / std::string{unload_probe_file_name};
        fs::copy_file(directory / std::string{probe_file_name}, copy, fs::copy_options::overwrite_existing);
        {
            auto const loaded = deren::utility::dynamic_link::load(copy.filename().string());
            CHECK(loaded.has_value());
            if (loaded.has_value()) {
                CHECK(loaded->symbol("deren_abi_version").has_value());
#if defined(_WIN32)
                std::error_code still_mapped{};
                CHECK(!fs::remove(copy, still_mapped)); // sharing violation: the image is mapped
#endif
            }
        }
        std::error_code remove_error{};
#if defined(_WIN32)
        CHECK(fs::remove(copy, remove_error)); // the destructor unloaded it
#else
        static_cast<void>(fs::remove(copy, remove_error)); // POSIX allows unlinking a loaded .so
#endif
    }

    void test_the_static_half_behaves_the_same() {
        // No library is involved here: abi_export.hpp's static branch produces ordinary C symbols
        // and the same three entry points are reached through the linker.
        CHECK(deren_abi_version() == 1u);
        int make_error = -1;
        int destroy_calls = 0;
        {
            core_owner const core{deren_make_api_core(deren_abi_version(), &make_error), &deren_destroy_api_core,
                                  &destroy_calls};
            CHECK(make_error == 0);
            CHECK(core.get() != nullptr);
            if (core.get() != nullptr) {
                CHECK(core.get()->value() == 42);
            }
        }
        CHECK(destroy_calls == 1);
        int mismatch_error = -1;
        CHECK(deren_make_api_core(0u, &mismatch_error) == nullptr);
        CHECK(mismatch_error == 7);
    }
} // namespace

int main(int argc, char** argv) {
    // The probe DLL sits next to this executable (both are outputs of the CMake binary directory)
    // while the working directory is <binary dir>/test-run, so argv[0] is where to look. Deriving
    // the path here keeps generated string literals with Windows path separators out of the build
    // ("\p" would be an escape sequence).
    fs::path const self = argc > 0 && argv != nullptr && argv[0] != nullptr ? fs::absolute(fs::path{argv[0]})
                                                                            : fs::current_path();
    fs::path const directory = self.parent_path();

    test_a_missing_file_is_a_returned_error(directory);
    test_the_platform_suffix_is_completed(directory);
    test_loading_the_probe_dll_and_using_its_symbols(directory);
    test_the_library_is_unloaded_when_it_goes_out_of_scope(directory);
    test_the_static_half_behaves_the_same();

    return vk_test::finish("test_dynamic_link");
}
