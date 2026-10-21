# Subtilis converter fixtures

Small GLB fixtures are generated in a unique temporary directory by
`test_subtilis_document.cpp`; no opaque binary copies are checked in.

The shared cross-group DAG in `subtilis_dag_fixtures.hpp` supplies 24 source
triangles, 16 intermediate triangles and 12 root triangles. Its two producers
feed two different consumers, and node instances share the same mesh.

Document variants cover unknown nested JSON/u64 precision, original array IDs
and BIN prefix, optional/required extensions, static/skin instance mixtures,
unsupported attributes/morph/alpha modes, missing UV and textured UV requirements,
nonindexed triangles, quantized signed-normalized attributes with strides, sparse
accessors, broken windows/strides, and unsupported required extension versions.
