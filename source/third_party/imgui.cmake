# Vendored Dear ImGui target.
include_guard(GLOBAL)

# Dear ImGui (vendored under source/third_party/imgui, see the sources there): the Vulkan + GLFW
# backends are compiled as plain C++ (imgui is not a module and does not follow the project's
# -Werror policy — upstream code triggers warnings we do not own). No clang-format: the
# PROJECT_FORMAT_SOURCES glob only covers main/source/configuration/source/gltf_loader/source/utility/
# engine/backends/tests/promise, never vendored third-party code.
add_library(imgui STATIC)

target_sources(imgui
    PRIVATE
        source/third_party/imgui/imgui.cpp
        source/third_party/imgui/imgui_draw.cpp
        source/third_party/imgui/imgui_tables.cpp
        source/third_party/imgui/imgui_widgets.cpp
        source/third_party/imgui/backends/imgui_impl_glfw.cpp
        source/third_party/imgui/backends/imgui_impl_vulkan.cpp
)

target_include_directories(imgui
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/source/third_party/imgui
        ${CMAKE_CURRENT_SOURCE_DIR}/source/third_party/imgui/backends
)

target_link_libraries(imgui
    PUBLIC
        Vulkan::Vulkan
        glfw
)
