# The GUI plugin DLL and its local loader test.
include_guard(GLOBAL)

# THE GUI PLUGIN IS BUILT BESIDE THE EXECUTABLE like the renderer backend, and for the same build-order
# reason - the runtime resolves `deren_gui_vulkan.dll` by NAME through deren.engine.gui_loader, so the file
# has to exist, but the executable must NOT link it (an import library would bind the GUI before main).
add_dependencies(deren deren_gui_vulkan)

# ============================================================================================
# deren_gui_vulkan: THE GUI AS ITS OWN PLUGIN (plan X2, the user's ruling: one DLL per API).
#
# WHAT LIVES HERE: the vendored ImGui sources (through the `imgui` static library, PRIVATE) and the whole
# `deren.vulkan.graphical_user_interface` module, plus the C entry that adapts the module's overlay to the
# `deren::gui::overlay` boundary (source/backends/vulkan/gui/gui_dll.cpp). THE DLL IS WHAT LINKS
# `vulkan-1`: the host resolves `deren_make_gui` by NAME through its loader and links nothing of this target,
# so `deren.exe` stops importing vulkan-1.dll - every one of those imports came from
# `imgui_impl_vulkan.cpp`, which is compiled in here.
#
# IT IS NOT IN ANY `add_dependencies` YET: the engine still uses the module directly (the "write it, wire it,
# test it, then delete the old path" order). The next step is the loader plus the runtime's rewiring.
# ============================================================================================
add_library(deren_gui_vulkan SHARED)
add_executable(test_gui_plugin source/tests/test_gui_plugin.cpp)
target_compile_definitions(test_gui_plugin PRIVATE VR_GUI_PLUGIN_DLL="$<TARGET_FILE:deren_gui_vulkan>")
target_link_libraries(test_gui_plugin static_utility vstd_lib promise)
add_dependencies(test_gui_plugin deren_gui_vulkan)
# Like the backend spike, this explicitly loads a graphics plugin and is run locally.
set_target_properties(deren_gui_vulkan PROPERTIES PREFIX "") # deren_gui_vulkan.dll, beside the executable
target_sources(deren_gui_vulkan
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/backends/vulkan/gui/gui.cppm
        PRIVATE
        source/backends/vulkan/gui/gui.cpp
        source/backends/vulkan/gui/gui_dll.cpp
)
# The project's compile options are not repeated here either - the sweep near the end of this file
# covers this DLL like every other first-party target; only the boundary macros are target-specific.
target_compile_definitions(deren_gui_vulkan PRIVATE DEREN_API_SHARED DEREN_API_BUILD)
target_link_libraries(deren_gui_vulkan
        PRIVATE
        imgui
        glfw
        Vulkan::Vulkan
        vulkan_main_context
        vulkan_constant_init
        static_utility
        vstd_lib
        promise
)
