# Vendored stb_image interface target.
include_guard(GLOBAL)

# stb_image.h (vendored): gltf_loader.cpp decodes embedded/external textures with it
# (#define STB_IMAGE_IMPLEMENTATION + #include <stb/stb_image.h>).
add_library(stb_vendored INTERFACE)

target_include_directories(stb_vendored
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/source/third_party
)
