# Vendored RmlUi 6.3 (core + debugger): the retained-mode document library behind the
# [gui] overlay. Only the library is built - no samples, tests, backends or plugins.
include_guard(GLOBAL)

if(TARGET RmlUi::Core)
    return()
endif()

# Scoped so the vendored build's standard and BUILD_SHARED_LIBS choice never leak into
# deren's own targets.
function(deren_vendor_rmlui)
    set(RMLUI_SAMPLES OFF CACHE BOOL "Vendored RmlUi: library only." FORCE)
    set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "Vendored RmlUi: no Lua bindings." FORCE)
    set(RMLUI_LOTTIE_PLUGIN OFF CACHE BOOL "Vendored RmlUi: no Lottie plugin." FORCE)
    set(RMLUI_SVG_PLUGIN OFF CACHE BOOL "Vendored RmlUi: no SVG plugin." FORCE)
    set(RMLUI_HARFBUZZ_SAMPLE OFF CACHE BOOL "Vendored RmlUi: no HarfBuzz sample." FORCE)
    set(RMLUI_TRACY_PROFILING OFF CACHE BOOL "Vendored RmlUi: no Tracy profiling." FORCE)
    # RmlUi's own warning/standards flag set is meant for standalone builds of the library.
    set(RMLUI_COMPILER_OPTIONS OFF CACHE BOOL "Vendored RmlUi: inherit deren's compiler flags." FORCE)
    set(RMLUI_WARNINGS_AS_ERRORS OFF CACHE BOOL "Vendored RmlUi: warnings are not errors." FORCE)
    set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "Vendored RmlUi: no precompiled headers." FORCE)
    set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "Vendored RmlUi: FreeType is the font engine." FORCE)

    # Static libraries. RMLUI_FONT_ENGINE needs Freetype::Freetype to exist before RmlUi's
    # CMake/Dependencies.cmake reports the dependency, so freetype.cmake runs first.
    set(BUILD_SHARED_LIBS OFF)

    # RmlUi targets C++14; compile the vendored tree at C++17 instead of deren's C++26.
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    add_subdirectory(
        "${DEREN_REPO_ROOT}/source/third_party/rmlui"
        "${CMAKE_BINARY_DIR}/third_party/rmlui"
        EXCLUDE_FROM_ALL
        SYSTEM
    )
endfunction()

deren_vendor_rmlui()
