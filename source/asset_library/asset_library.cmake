# Folder-backed asset catalogue target.
include_guard(GLOBAL)

# The folder-backed asset catalogue: a pure-CPU manifest scanner (no Vulkan, no engine dependency),
# so it can be unit-tested and used by the startup path before any device exists. It reads the same
# toml++ headers as app_config and must select the same non-exception API for the same reason.
add_library(asset_library STATIC)

target_sources(asset_library
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/asset_library/asset_library.cppm

    PRIVATE
        source/asset_library/asset_library.cpp
)

if(VR_TOMLPP_INCLUDE_DIR)
    target_include_directories(asset_library SYSTEM PRIVATE "${VR_TOMLPP_INCLUDE_DIR}")
endif()
target_compile_definitions(asset_library PRIVATE TOML_EXCEPTIONS=0)

target_link_libraries(asset_library
        static_utility
        vstd_lib
)
