# Vendored glm header-only target.
include_guard(GLOBAL)

# glm (header-only). find_package() is enough for clang64 because that package ships a config, but the
# config points glm::glm's INTERFACE_INCLUDE_DIRECTORIES at the WHOLE clang64 include root. For clang
# that root is the compiler's own root and nothing is shadowed; for MSVC it is a foreign libc's headers
# and it is fatal - measured with cl.exe, which then found mingw's corecrt.h / stdlib.h and emitted a
# wall of C2146/C2081/C2061 with corecrt.h(219) at the top of it.
# Point VR_GLM_INCLUDE_DIR at a directory that CONTAINS the glm/ subtree (the clang64 include root is
# fine for headers clang already owns, but on MSVC use a shim that holds only glm/) and the target is
# built here from that directory instead of from the config package.
set(VR_GLM_INCLUDE_DIR "" CACHE PATH "Directory containing the glm/ header subtree; when set, glm::glm is built from it instead of find_package(glm) (MSVC builds need this)")
if(VR_GLM_INCLUDE_DIR)
    add_library(glm_headers INTERFACE)
    target_include_directories(glm_headers SYSTEM INTERFACE "${VR_GLM_INCLUDE_DIR}")
    add_library(glm::glm ALIAS glm_headers)
    # ...AND at directory scope, because not every target that includes <glm/...> links glm::glm:
    # `deren.utility` (BVH) and `deren.gltf_loader` both include it while linking only each other, and under clang
    # that worked purely because glm lived on the compiler's implicit include path. Measured on MSVC
    # without this line: three C1083s - source/utility/BVH/bvh.cppm(3), source/gltf_loader/gltf_loader.cppm(18),
    # source/gltf_loader/gltf_loader.cpp(6). This call is inside the VR_GLM_INCLUDE_DIR branch on purpose: when
    # the variable is empty (every clang build) no include directory is added anywhere and the command
    # lines stay exactly what they were.
    include_directories(SYSTEM "${VR_GLM_INCLUDE_DIR}")
else()
    find_package(glm REQUIRED)
endif()
