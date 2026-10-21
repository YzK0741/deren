# Subtilis offline converter

This standalone CPU tool builds the `DEREN_subtilis` shared-vertex cluster DAG.
The original glTF primitive remains the LOD0 fallback. It does not yet add a
renderer feature or require Vulkan/CUDA.

Build with CMake 4.3+, Ninja and a C++23 standard-library-module toolchain. For
the project's MSYS2 CLANG64 environment:

```powershell
$env:PATH = 'C:/msys64/clang64/bin;' + $env:PATH
cmake -S tools/subtilis -B build-subtilis -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=ON
cmake --build build-subtilis -j12
ctest --test-dir build-subtilis --output-on-failure
```

```powershell
./build-subtilis/deren-subtilis build input.glb -o cooked.glb --threads 1 --report build.json
./build-subtilis/deren-subtilis inspect cooked.glb
./build-subtilis/deren-subtilis validate cooked.glb --full
./build-subtilis/deren-subtilis export-cut cooked.glb --error 0.01 -o preview.glb
```

`export-cut` writes ordinary glTF indices for the selected model-space error
threshold and removes the consumed extension. Material, attributes and instances
keep their original identities. Exported files can be opened by standard GLB
viewers. Unreferenced old binary ranges are retained to preserve other IDs.

Current milestone: self-contained GLB input/output, one CPU thread, no overwrite.
External-resource packaging, repeat-conversion reuse and parallel scheduling are
the next milestones. Requests for other thread counts fail explicitly.

Ineligible primitives are preserved and counted as skipped; `--strict` rejects
any skip. Zero eligible primitives is an error. A valid asset without accepted
reductions succeeds with `lod0Only` in its report; this does not claim a geometry
reduction. Reports include per-frontier counts, rounds, cross-group repartitions,
known work estimates and wall time. Estimates are not process peak memory.

Exit codes: 0 success, 2 command usage, 3 invalid input/validation, 4 construction
or eligibility failure, 5 output failure. Existing asset/report files are refused.
Asset publication uses a temporary file on the destination volume and validates
it before publishing; the optional report is published afterwards.

The wire format and canonical identity are documented in
[`docs/extensions/DEREN_subtilis`](../../docs/extensions/DEREN_subtilis/README.md).
The implementation plan tracks the remaining packaging, profiling, real-scene
and CI validation work.
