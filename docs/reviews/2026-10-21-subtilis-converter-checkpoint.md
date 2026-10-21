# Subtilis converter checkpoint — 2026-10-21

Plan: [CPU glTF DAG converter](../superpowers/plans/2026-10-21-subtilis-gltf.md).
Base: d900c48f. Current-checkout inline execution, local commits, no push.

## Task 1: standalone codec and identity

- Extracted existing vstd/shared utility/mimalloc and CPU asset dependency target
  definitions into shared CMake helpers; existing target/module names retained.
- Standalone Release configuration explicitly disables finding Vulkan. Generated
  dependency graph contains no engine, promise/RHI, Vulkan, or CUDA targets.
- Wire v1 codec handles seven sections, explicit little-endian fields, checked
  counts/ranges, zero padding, JSON metadata and finite scalar records. The
  returned payload remains untrusted; it is not a validated DAG.
- Streaming SHA-256 adapted from the existing test implementation. Source/build
  identities have independently generated Python struct/hashlib golden vectors;
  NIST empty/abc/multiblock/million-a vectors pass.
- yyjson 0.12.0 fixed commit vendored with license and SHA256 manifest; all three
  source Git blob IDs match upstream. Mutable JSON parsing uses utility memory.
- MSYS2 simdjson 5.0.4 considered: retain vendored 4.6.9 for fastgltf and yyjson
  for document mutation. Switching to an immutable parser does not remove the
  need to preserve and modify arbitrary original JSON.

Validation: witnessed codec RED (encode missing behavior), then Release GREEN;
all standalone targets build with `-j12`, codec test passes (0.15s). Root Release
configuration/build succeeds; 58/58 CTest pass in 37.21s. First sandboxed root run
had two `os.link` WinError 5 errors in test_backend_boundary; rerun outside that
restriction passes unchanged tests. No product behavior changed to bypass it.

Ruling: source_digest/build_digest return `result<digest>` to reject nonfinite
geometry/invalid options as typed errors before hashing. Callers must unwrap;
canonical valid-input encoding remains fixed and documented.

## Task 2: immutable validated DAG and CPU cuts

- Validates table limits/ranges, reciprocal producer/consumer relationships,
  fine-to-coarse group order, strict triangle progress, exact leaf/root sets,
  finite metrics, nested LOD spheres, and all reference cells (including unused ones).
- Full factory binds the source digest, checks leaf triangle multiplicity/winding,
  cluster/group sphere containment in double precision, and per-reduction directed
  boundary/component preservation with normal/UV seam identities. Owned input
  survives destruction of caller copies. No mutable payload API is exposed.
- CPU cut supports model-space thresholds and standard right-handed ZO
  perspective/orthographic projections using column-major matrices. Nonuniform
  scale/shear uses a conservative operator-norm bound. Invalid views/near-plane
  crossing fall back conservatively; capacity never truncates a cut.
- Metric decisions are closed along group dependencies before scanning clusters.
  The private closure helper is independently tested with inconsistent raw
  decisions, including idempotence; no public test hooks were added.
- Actual cross-group fixture reduces 24→16→12 triangles and exercises a mixed
  cut `{5,7,8}` as well as full LOD0/intermediate/root cuts. Tests include 128
  seeded thresholds and 128 seeded camera distances, plus inward/outward f32
  sphere rounding, malformed references, and geometry changes.

Validation: witnessed initial DAG RED; an additional invalid unused-reference
fixture also failed before its check was added. Release 2/2 CTest pass (0.13s);
ASan/UBSan Debug 2/2 pass (0.76s). Root shared build configuration did not change
in this task; its Task 1 58/58 run remains the relevant regression check.

## Task 3: document preservation and fixture file loop

- Added an owning mutable document, stable mesh/primitive keys and instance/skin
  classification. Original DOM, array IDs and embedded BIN prefix are preserved.
- Embedded GLB and data-URI glTF decode via fastgltf after independent bounded
  window/sparse-index checks. No node transforms are baked into geometry.
- Unknown nested JSON and u64 precision, nodes/scenes/materials/accessors,
  original primitive fields and animation tracks survive extension attachment.
- The hand-authored DAG now completes attach→GLB→read→decode→full factory→cut.
  Supported required DEREN metadata is filtered only in the parser copy; original
  declarations survive. Unknown/unsupported required extensions and invalid
  required/used relationships fail. Unsupported optional payload leaves LOD0 readable.
- Tests cover signed-normalized quantization/strides, sparse/no-base accessors,
  overflow/bad windows, nonindexed triangles, missing UV, texture requirements,
  unmodeled attributes, morphs/alpha modes, static+skin instances and rigid animation.
- Found fastgltf 0.9 `validate()` dereferences sparse bufferView indices without a
  bounds check. A negative fixture reproduced an ASan heap-buffer-overflow at
  upstream fastgltf.cpp:999. Converter preflight now checks those references and
  analogous animation references before calling upstream; vendored code unchanged.
  Additional RED→GREEN regressions cover direct attachment to an ineligible
  primitive and required-extension version/subset handling.

Validation: initial document RED witnessed. Latest Release 3/3 pass (0.27s),
ASan/UBSan 3/3 pass (1.04s). Root dependencies unchanged since Task 1.
External-resource loading/packaging and transactional output remain Task 6;
embedded-mode helper rejects resource relocation it cannot yet preserve.

## Task 4: spatial clustering and constrained reduction candidates

- LOD0 and reduced meshlets use meshoptimizer with the profile's 256-vertex /
  85-triangle limits and cone weight zero. Original vertex IDs and source
  triangle multiplicity/winding remain intact; bounds round outward.
- Partitions use meshopt_partitionClusters and stable group/member order.
- Reduction uses meshopt_simplifyWithAttributes with immutable source data,
  primitive-diagonal normalization, fixed normal/UV weights, explicit seam/border
  locks and isolated floating-point status. Only strictly smaller valid candidates
  are accepted; no-progress/unsafe regions retain their original representation.
- Position and attribute boundary signatures plus source-component ownership are
  checked again after simplification. This builder topology remains independently
  checked by Task 2's full asset validator when assembled in Task 5.
- Topology reuse adds an optional private cached-topology argument to reduce_group,
  avoiding a whole-source analysis for each candidate; four-argument callers retain
  the original convenience form. This is not a public consumer API.

Validation: witnessed initial leaf-clustering RED. Curved-grid reduction, exact
LOD0 coverage, partitions, shared boundaries, UV/normal seams, disconnected
components, duplicate/nonmanifold faces, invalid options/nonfinite indices and
scale invariance pass. Release 4/4 CTest (0.20s), ASan/UBSan 4/4 (1.23s).

## Task 5: serial DAG construction and working CLI

- Serial rounds partition the active frontier, prepare immutable candidates and
  assign accepted global IDs in stable order. Declined regions carry forward;
  local error accumulates with dependency error and nested spheres round outward.
- A generated 32x32 grid yields eight groups and two producers whose coarse
  clusters feed distinct consumers. The test scans actual references; a hand DAG
  or an all-LOD0 result cannot satisfy this witness. Repeated payload bytes match.
- `build`, `inspect`, `validate --full` and `export-cut` now work for self-contained
  GLBs. Exported indices match CPU cut selection at three thresholds, and decoded
  source attributes remain exact. Tests cover skipped/strict/zero-target cases
  and explicit LOD0-only reporting.
- Destination-volume staging validates the generated GLB before publishing without
  replacing an existing target. Full packaging, overwrite/reuse policy, external
  resources, parallelism and real-scene/CI acceptance remain Tasks 6–8.

Validation: Task 5 initial builder/CLI RED recorded in the execution workspace.
Release 5/5 (0.37s), ASan/UBSan 5/5 (2.10s). Actual executable smoke:
build/report, inspect, full validate and cut
export all exit 0; preview instance/attribute identities checked independently.
Usage and current limitations are in `tools/subtilis/README.md`.

Ruling: minimal validated, no-replace staging was pulled forward from Task 6 to
make Task 5's CLI output safe to try. It costs a later packaging refactor; full
transaction/resource guarantees are still Task 6 acceptance requirements.

## Task 6: partial implementation, not complete

Task 5 commit: `de785b74`. Task 6 continues from that base.

- Added bounded local-resource snapshots, percent-escaped URIs and validated
  base64 data URIs. Original buffer/accessor IDs survive document editing;
  non-embedded input gets a dedicated appended buffer.
- GLB packaging merges known buffer references, keeps original geometry exact,
  embeds PNG/JPEG bytes and validates MIME signatures. Unknown extensions or
  buffer metadata prevent unsafe relocation. No network resources are fetched.
- Full-valid source/build-compatible payloads are reused without growth.
  Incompatible/corrupt payloads fail even when their primitive is now ineligible.
- Extracted package validation/publication from CLI commands. Added the typed
  `convert_file` API and `write_package`; CLI and API share construction logic.
- `--overwrite` uses atomic file replacement, forbids source/report aliases and
  leaves prior bytes intact on publication failure. Windows read-only-target,
  directory-target failure and temporary cleanup tests pass.

RED witnessed for reuse, external buffers, image embedding, overwrite, hidden
corrupt payload classification and converter API. Latest Release 5/5 (0.51s),
ASan/UBSan 5/5 (2.75s), built with `-j12`. Root build files unchanged since Task 1.

Remaining Task 6 acceptance: transactional **glTF directory-package output**
(the public format enum currently rejects gltf explicitly), controlled short-write
and close-failure injection, counted >4GiB packaging overflow fixtures, exhaustive
resource hashes/core-DOM checks, then the whole Task 6 completion gate. No Task 6
checkbox is marked complete. Task 7 parallel scheduling and Task 8 real assets,
core validator, independent CI and final fresh whole-change review remain pending.

Resume from this checkpoint and `.superpowers/sdd/2026-10-21-subtilis-gltf/progress.md`;
do not redo Tasks 1–5 or claim final converter acceptance.
