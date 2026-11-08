# Vendored GLFW interface target.
include_guard(GLOBAL)

# GLFW (window + input): linked STATICALLY so the released exe carries no glfw3.dll.
# The MSYS2 clang64 package's own CMake config exports a SHARED import (libglfw3.dll.a),
# so we do not use find_package(glfw3): a plain INTERFACE target linking the bare name
# would now resolve to that shared import (the exe no longer links with -static - see the
# link-options block below), which is why the interface names libglfw3.a outright, and
# crucially does NOT define GLFW_DLL (the dynamic-import macro static builds must not see).
# Headers live on the compiler's implicit include path; an explicit dir is added when found.
#
# THE EXCEPTION IS MSVC, where `glfw3` is not a library name at all (that bare-name convention is the
# MinGW one). The official GLFW binary release ships include/ plus lib-vc2022/glfw3.lib, so
# VR_GLFW_ROOT points at the unpacked release directory and both the headers and the .lib come from
# there. lib-vc2022/glfw3.lib is an /MD (dynamic CRT) build, which is why the MSVC branch of the
# compile options leaves the CRT at its own default.
set(VR_GLFW_ROOT "" CACHE PATH "Unpacked GLFW binary release (the directory holding include/ and lib-vc2022/); required for MSVC builds")
add_library(glfw INTERFACE)
if(WIN32)
    if(VR_GLFW_ROOT)
        target_include_directories(glfw SYSTEM INTERFACE "${VR_GLFW_ROOT}/include")
        find_library(VR_GLFW_LIBRARY NAMES glfw3 glfw3_mt
                PATHS "${VR_GLFW_ROOT}/lib-vc2022" "${VR_GLFW_ROOT}/lib-mingw-w64"
                NO_DEFAULT_PATH REQUIRED)
        target_link_libraries(glfw INTERFACE ${VR_GLFW_LIBRARY} gdi32)
    else()
        # MINGW ONLY: a bare `glfw3` is "the linker's choice", and that choice is the SHARED import
        # (libglfw3.dll.a) whenever the executable is not linked with -static. The C++ runtime is
        # dynamic now (see the link-options block below), so the static archive is named explicitly
        # here instead - the released exe must still carry no glfw3.dll (see the note above).
        find_library(VR_GLFW_STATIC_LIBRARY NAMES libglfw3.a REQUIRED)
        target_link_libraries(glfw INTERFACE ${VR_GLFW_STATIC_LIBRARY} gdi32) # glfw3.pc: Libs.private = -lgdi32
    endif()
else()
    find_package(Threads REQUIRED)
    find_library(GLFW_LIBRARY NAMES glfw3 REQUIRED)
    target_link_libraries(glfw INTERFACE ${GLFW_LIBRARY} Threads::Threads)
endif()
