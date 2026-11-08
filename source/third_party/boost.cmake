# Vendored Boost header-only targets.
include_guard(GLOBAL)

# Private header subset; link only from graph implementation/probes, never the RHI ABI.
# Standalone, pinned Boost.Int128 is a private implementation dependency of BVH only.
add_library(boost_int128_headers INTERFACE)
target_include_directories(boost_int128_headers SYSTEM INTERFACE
        "${DEREN_REPO_ROOT}/source/third_party/boost")

add_library(boost_graph_headers INTERFACE)
target_include_directories(boost_graph_headers SYSTEM INTERFACE
    "${DEREN_REPO_ROOT}/source/third_party/boost")
target_compile_definitions(boost_graph_headers INTERFACE BOOST_NO_EXCEPTIONS BOOST_ALL_NO_LIB)
