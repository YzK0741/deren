# The executable, backend loader, app support and final link.
include_guard(GLOBAL)

# THE APPLICATION (the runtime switch and the S1-S3 gate that used to wrap this are retired: see the
# comment above `deren_engine`).
add_executable(deren source/app/main.cpp source/app/startup_splash.cpp)
if(WIN32)
    enable_language(RC)
    target_sources(deren PRIVATE source/app/resources/deren.rc)
    set_source_files_properties(source/app/resources/deren.rc PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/source/app/resources/deren.ico;${CMAKE_CURRENT_SOURCE_DIR}/source/app/resources/deren-splash.bmp")
    target_include_directories(deren PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/source/app/resources")
    target_link_libraries(deren user32 gdi32)
endif()
# Every build links as a Windows GUI-subsystem executable: no console window appears when running.
# On the MSYS2 clang64 / MinGW-w64 toolchain WIN32_EXECUTABLE maps to -mwindows; the CRT still calls
# main(), only the subsystem changes. Logging writes deren-debug.log in every build (no console log
# route), so no configuration needs the terminal.
set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE ON)


# shared_utility: the process-wide half. vessel containers route allocations through the
# vendored mimalloc (source/third_party/mimalloc), so the dependency moved here with the allocator hook -
# the executable no longer needs MSYS2's libmimalloc.dll. vstd_lib is all it needs besides: the
# module imports NEITHER the contract nor static_utility.
# ============================================================================================
# backend_loader - WHERE THE BACKEND IS LOADED AND THE DEVICE ROOT IS ACQUIRED (batch ⑥).
#
# WHY A TARGET OF ITS OWN: the runtime used to do this (it loaded deren_vulkan.dll, resolved the one
# export and called it before its own construction), so the APPLICATION could not answer which
# backend, from where, with which search rules, or what a failure means - and the renderer owned a
# policy that is not its job. The module now sits between them:
#
#     auto core = deren::vulkan::load_api_core(core_options);   // deren.app.application (the app decides)
#     deren::vulkan::runtime runtime{core, core_options};       // the renderer (given a root)
#
# NOT IN static_utility, deliberately: that half is the pure-CPU toolkit with no loading semantics,
# and deren.utility.dynamic_link (which it owns) is the MECHANISM - open a named image, resolve a
# named symbol - not a policy about backends. This target is the policy: the file name, the search
# rules, the creation-side handshake, the named diagnosis. It links BOTH the toolkit and glfw, because
# it is also the module that crosses the image boundary and therefore translates the caller's window
# handle into the native one the DLL can understand (see the module's banner).
# ============================================================================================
add_library(backend_loader STATIC)
target_sources(backend_loader
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/engine/runtime/backend_loader.cppm
)
target_link_libraries(backend_loader
        promise          # the contract: create_info / api_core / abi_version
        static_utility   # deren.utility (executable_directory / the diagnosis channel) + dynamic_link
        glfw             # glfwGetWin32Window: the ONE field translated for the wire
)

# The file dialog (source/utility/platform_dialog.cpp, for the config's `model = "ask"`) is GetOpenFileNameW,
# which lives in the common dialog library. Windows-only, like the backend itself.


# Application-level support modules. The renderer's pass wiring lives here, not in deren_engine:
# the engine records whatever chain it is handed, while this target owns the chain this app builds.
# This is the former `chores` target split into focused modules: startup config, pipeline setup,
# the renderer, the debug GUI, the animation backend and the instancing stress helper.
add_library(app_support STATIC)
target_sources(app_support
    PUBLIC
        FILE_SET CXX_MODULES
        FILES
        source/app/app_config.cppm
        source/app/app_pipelines.cppm
        source/app/app_renderer.cppm
        source/app/app_ui.cppm
        source/app/app_animation.cppm
        source/app/app_instancing.cppm
        source/app/app_toon.cppm
        source/app/app_application.cppm
    PRIVATE
        source/app/app_config.cpp
        source/app/app_pipelines.cpp
        source/app/app_renderer.cpp
        source/app/app_ui.cpp
        source/app/app_animation.cpp
        source/app/app_instancing.cpp
        source/app/app_toon.cpp
        source/app/app_application.cpp
)
target_link_libraries(app_support
        app_config
        asset_library
        deren_assets
        deren_engine
        static_utility
        vstd_lib
        backend_loader   # deren.app.application calls load_api_core(): the app owns the acquisition (batch ⑥)
)

target_link_libraries(${PROJECT_NAME}
        static_utility
        deren_engine
        app_support
        # AUTO-IMPORT, and it is the opposite rule from the two graphics plugins, which are resolved
        # BY NAME at run time and are not in this link line at all (see the add_dependencies below).
        # Linking deren_assets' import library is what makes the image load before main. Its banner
        # states why an asset loader, unlike a backend, has nothing to gain from the manual regime.
        deren_assets
        app_config
        vstd_lib
        glm::glm
        # (imgui is NOT linked here: the GUI is a plugin loaded by name - see deren_gui_vulkan.)
)
# BUILD ORDER, NOT LINKAGE: the runtime resolves deren_make_api_core out of deren_vulkan.dll at run
# time (source/engine/runtime/runtime.constructor.cppm), so the DLL has to exist beside the executable - but the
# executable must NOT link it (an import library would load the backend before main, which is the
# load-time binding the flip removes). A missing DLL is then a NAMED loader panic instead of a system
# 'entry point not found' dialog. shared_utility.dll comes in through static_utility's link interface;
# listing deren_vulkan here also drags it in, and the explicit pair below documents the package: the
# backend ships as deren_vulkan.dll + shared_utility.dll, both beside the executable.
add_dependencies(${PROJECT_NAME} deren_vulkan shared_utility)
