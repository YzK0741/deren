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

Current milestone: local glTF/GLB and base64 data URI input, GLB or glTF
directory-package output, one CPU thread. Local buffer URIs support percent
escaping. Known buffer references can merge into GLB; PNG/JPEG resources can
embed without changing their bytes. Unknown extensions or buffer metadata that
cannot relocate safely cause an explicit error for GLB and are preserved by the
glTF package. Requests for other thread counts fail explicitly.

`--format glb` (the default) keeps the embedded single-file output: `-o
cooked.glb` writes one GLB. `--format gltf` writes a relocatable package
directory: `-o cooked.gltf` creates `cooked/` containing `cooked.gltf` plus its
resources (`cooked.bin` for the embedded/appended buffer, `bufferN.bin` for each
original external buffer, `imageN.png`/`imageN.jpeg` for external PNG/JPEG
images, all referenced by percent-encoded relative URIs). The package stages on
the destination volume and is published atomically: an existing directory or
file is refused unless `--overwrite`, and a failed publication leaves the source
and any previous package untouched.

An existing payload is fully validated and reused when its source/build identity
matches. `reused` counts these primitives; the payload is not appended again.
Mismatching or damaged payloads require rebuilding from the original source.

Ineligible primitives are preserved and counted as skipped; `--strict` rejects
any skip. Zero eligible primitives is an error. A valid asset without accepted
reductions succeeds with `lod0Only` in its report; this does not claim a geometry
reduction. Reports include per-frontier counts, rounds, cross-group repartitions,
known work estimates and wall time. Estimates are not process peak memory.

Exit codes: 0 success, 2 command usage, 3 invalid input/validation, 4 construction
or eligibility failure, 5 output failure. Existing asset/report files are refused.
Pass `--overwrite` to replace an existing output/report atomically. Source-file
overwrites are always refused, including equivalent paths. Asset publication uses
a temporary file on the destination volume and validates it before publishing;
the optional report is published afterwards.

The host API exposes `convert_file` with typed options/report and
`write_package`; for `output_format::gltf` the `write_package` target is the
package directory (the CLI maps `-o out.gltf` to `out/`). Package resources are
written through a small publication seam so the test target can inject short
writes, close failures and final rename failures; production entry points never
inject.

The wire format and canonical identity are documented in
[`docs/extensions/DEREN_subtilis`](../../docs/extensions/DEREN_subtilis/README.md).
The implementation plan tracks the remaining packaging, profiling, real-scene
and CI validation work.
