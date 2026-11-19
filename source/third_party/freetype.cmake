# Vendored FreeType 2.13.3, the font engine behind the RmlUi overlay.
include_guard(GLOBAL)

# The library only: no harness. Optional codecs stay off so the vendored tree needs no
# system zlib/libpng/bzip2/brotli/harfbuzz and no pkg-config probes.
set(FT_DISABLE_ZLIB ON CACHE BOOL "Vendored FreeType: build without zlib." FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "Vendored FreeType: build without bzip2." FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "Vendored FreeType: build without libpng." FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "Vendored FreeType: build without HarfBuzz." FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "Vendored FreeType: build without Brotli." FORCE)
# Debug builds keep the plain `freetype` name instead of upstream's `freetyped`.
set(DISABLE_FORCE_DEBUG_POSTFIX ON)
# FreeType's cmake_minimum_required predates the 3.5 compatibility floor of CMake 4.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

add_subdirectory(
    "${DEREN_REPO_ROOT}/source/third_party/freetype"
    "${CMAKE_BINARY_DIR}/third_party/freetype"
    EXCLUDE_FROM_ALL
    SYSTEM
)

# Upstream only names the target Freetype::Freetype on install (EXPORT_NAME), while RmlUi's
# find_package(Freetype) path expects that exact name to exist when it links.
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype-interface)
endif()
