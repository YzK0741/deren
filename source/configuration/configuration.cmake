# Startup configuration target.
include_guard(GLOBAL)

# Startup configuration: parses config.toml (+ --config / argv merging) into app_settings.
# Uses toml++ (header-only, system package mingw-w64-clang-x86_64-tomlplusplus; under clang64 its
# headers live on the compiler's own include root, so nothing has to be added here). Pure CPU, no
# Vulkan.
#
# MSVC IS THE EXCEPTION, and it is the same shape as glm above: cl.exe has no such implicit root, so
# VR_TOMLPP_INCLUDE_DIR is where a build that is not using the clang64 toolchain finds <toml++/toml.hpp>.
# It is a directory that CONTAINS the toml++/ subtree (either a real include root or a shim).
set(VR_TOMLPP_INCLUDE_DIR "" CACHE PATH "Directory containing the toml++/ header subtree; required for MSVC builds (toml++ is header-only)")
add_library(app_config STATIC)

target_sources(app_config
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/configuration/configuration.cppm

    PRIVATE
        source/configuration/configuration.cpp
)

if(VR_TOMLPP_INCLUDE_DIR)
    target_include_directories(app_config SYSTEM PRIVATE "${VR_TOMLPP_INCLUDE_DIR}")
endif()

# toml++ picks its API from the exception mode it detects in the compiler flags (TOML_EXCEPTIONS,
# toml++/impl/preprocessor.hpp): exceptions OFF gives parse_result a type with operator bool and
# error(), exceptions ON makes parse_result an alias of toml::table and gives the parse functions a
# different return type. The product code is written against the non-exception API ("parsed" /
# "parsed.error().description()" in configuration.cpp), and clang64 gets that mode for
# free because the project compiles with -fno-exceptions.
#
# MSVC does NOT detect it: /EHsc is deliberate there (see the dialect table above - there is no
# supported "no exceptions" spelling beside the STL), so toml++ would select the throwing API and
# BOTH translation units of this target would fail to compile against it. State the mode instead of
# inferring it, here rather than in a source file, so the module interface and the implementation are
# guaranteed to agree: they must, because toml::parse_result is otherwise one type in
# configuration.cppm and another in configuration.cpp. Only this target sees
# the header (checked: no other source in the tree includes toml++), so PRIVATE is enough.
target_compile_definitions(app_config PRIVATE TOML_EXCEPTIONS=0)


target_link_libraries(app_config
        static_utility
        vstd_lib
)
