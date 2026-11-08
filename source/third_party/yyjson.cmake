# Vendored yyjson target.
include_guard(GLOBAL)

add_library(yyjson_vendored STATIC ${DEREN_REPO_ROOT}/source/third_party/yyjson/src/yyjson.c)
target_include_directories(yyjson_vendored SYSTEM PUBLIC ${DEREN_REPO_ROOT}/source/third_party/yyjson/src)
