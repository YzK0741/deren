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

## Runtime import fixtures

`make_object_lod_fixtures.py` generates `cross_dag`, `seam` and `lod0_only` assets
using the accepted standalone converter. Their cooked `.glb`, extracted `.payload`
and source `.source.bin` files are checked in so root tests need neither a running
converter nor an external scene. `fixtures.json` records each byte length/SHA-256,
the converter source commit and binary hash, and the generation command. The DAG
test verifies these hashes and full geometry validity before comparing cuts.

The source sidecar is test-only: two little-endian u32 counts (vertices, indices),
then tightly packed POSITION float3, NORMAL float3, TEXCOORD_0 float2 arrays and u32
indices. It is not a runtime asset format.

```powershell
python scripts/measure/make_object_lod_fixtures.py
python scripts/measure/make_object_lod_fixtures.py --verify
```
