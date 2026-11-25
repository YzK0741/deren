# Vendored HarfBuzz 11.2.1, the shaper behind the editor UI's text pipeline.
include_guard(GLOBAL)

# Only the library: the amalgam translation unit is upstream's supported single-file build of the
# core, OpenType, AAT and fallback shapers. The optional font backends stay off, so nothing here
# needs glib, ICU, Graphite, Cairo or FreeType: the face is created from the font file's bytes and
# FreeType is only asked to rasterise the glyphs HarfBuzz chose.
set(DEREN_HARFBUZZ_DIR "${DEREN_REPO_ROOT}/source/third_party/harfbuzz")

add_library(deren_harfbuzz STATIC "${DEREN_HARFBUZZ_DIR}/src/harfbuzz.cc")
target_include_directories(deren_harfbuzz SYSTEM PUBLIC "${DEREN_HARFBUZZ_DIR}/src")
set_target_properties(
    deren_harfbuzz
    PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
)
# Third-party code: its warnings are not this project's to fix, and the amalgam trips several.
if(NOT MSVC)
    target_compile_options(deren_harfbuzz PRIVATE -w)
endif()

add_library(HarfBuzz::HarfBuzz ALIAS deren_harfbuzz)
