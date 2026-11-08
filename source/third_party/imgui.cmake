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
        ${DEREN_REPO_ROOT}/source/third_party/imgui/imgui.cpp
        ${DEREN_REPO_ROOT}/source/third_party/imgui/imgui_draw.cpp
        ${DEREN_REPO_ROOT}/source/third_party/imgui/imgui_tables.cpp
        ${DEREN_REPO_ROOT}/source/third_party/imgui/imgui_widgets.cpp
        ${DEREN_REPO_ROOT}/source/third_party/imgui/backends/imgui_impl_glfw.cpp
        ${DEREN_REPO_ROOT}/source/third_party/imgui/backends/imgui_impl_vulkan.cpp
)

target_include_directories(imgui
    PUBLIC
        ${DEREN_REPO_ROOT}/source/third_party/imgui
        ${DEREN_REPO_ROOT}/source/third_party/imgui/backends
)

target_link_libraries(imgui
    PUBLIC
        Vulkan::Vulkan
        glfw
)
