# subtilis glTF DAG converter - fresh whole-change review (2026-10-21)

Reviewer: hzb-architect (single fresh read-only reviewer, no per-task agents; plan bullet 6).
Scope: the committed converter workstream on local master, HEAD 20e05cb9 (range 07e365bd..20e05cb9,
including cf7b7c3c/f6586564/d900c48f plan/design docs and the task commits ec773f19, c1afe60a,
4973efd9, 1ac1c0bb, de785b74, dbfc37c0, d3e371b7, 1e3df1e7, 071cfdd5, ed8aa587, 20e05cb9).
Authority docs read: plan docs/superpowers/plans/2026-10-21-subtilis-gltf.md (8 task bullets,
5 Review Focus, self-check), design docs/superpowers/specs/2026-10-21-subtilis-gltf-design.md,
checkpoint docs/reviews/2026-10-21-subtilis-converter-checkpoint.md, evidence
docs/reviews/2026-10-21-subtilis-converter.md + results JSONs, tools/subtilis/README.md,
docs/extensions/DEREN_subtilis/README.md and schema.

Read-only rules honored: no worktree file was modified except this report; no git mutation; no
implementation fixes; probe outputs were written only under the gitignored build-subtilis tree.
Verdict: see section 8. One Important finding; Minor findings listed; no Critical finding.
This document does not accept the work; that decision is the Lead's.

## 1. Method and environment

- Code read in full: source/asset/subtilis/{types.cppm,codec.cpp,digest.cpp,dag.cpp,cut.cpp,
  builder.cpp,topology.cpp,reduce.cpp,parallel.cpp,builder_internal.hpp,cut_closure.hpp},
  tools/subtilis/* (document/accessor/package/resources/document_edit/commands/main/CMake),
  scripts/measure/{make_subtilis_converter_fixtures.py,measure_subtilis_converter.py,
  verify_subtilis_converter.py,subtilis_converter_gltf.py,khronos_validator_manifest.json},
  .github/workflows/subtilis-converter.yml, cmake/deren_cpu_utility.cmake,
  cmake/deren_asset_dependencies.cmake, root CMakeLists.txt diff, all six new test sources.
- Commands run: build/ctest of the committed standalone tree (Release and ASan/UBSan), the six
  test binaries directly, the pinned Khronos validator on archived outputs, validate/inspect on
  archived outputs, a fresh bistro-merged conversion on committed source, an independent Python
  LOD0/cut checker on the archived artifacts, a full re-hash of the D:/ evidence archive, an
  independent recomputation of every measured median/table, the gate self-test, and crafted CLI
  probes for the packaging rules. Commands/shape are listed with each item in section 7.
- Host: Windows 11, MSYS2 clang64, 16 logical CPUs, Python 3.14. Evidence locations:
  build-subtilis (gitignored), D:/deren-assets/2026-10-21-subtilis-converter (archive),
  D:/deren-assets/bistro (real assets), D:/deren-assets/gltf-tools-khronos/2.0.0-dev.3.10.

## 2. Review Focus coverage (plan lines 21-27)

Each focus is confirmed with concrete evidence; negative controls or contrast arms are noted.
"Pass" means the committed tests/evidence exercise the stated hazard, not that the rule is
unconditionally enforced (see section 5 for the one gap).

| # | Review Focus | Evidence in the committed work | Negative control / contrast | Verdict |
|---|---|---|---|---|
| 1 | mesh shared by several nodes and partially skinned: no duplicate bake, no lost instance, no should-skip asset made static | test_subtilis_document.cpp:133-179 builds one mesh used by two nodes with node 1 skinned (mixed-skin.glb): eligible_primitives() is empty and the DOM keeps "skin":0 and both nodes; converter test :334 and :756-788 require node/scene/material lists unchanged and one payload per primitive for the two-node rich fixture; test_subtilis_parallel.cpp:574-610 requires same-mesh multi-instance GLBs byte-identical for threads 1/2/12 | The mixed fixture would report eligible=1 if a skinned instance were mistaken for static; the rich/parallel fixtures would fail if instances were baked or payload duplicated (file hash / DOM compare) | Pass |
| 2 | unknown extension with direct buffer refs / external resources: DOM preservation, GLB refuses dangerous packaging, required-extension errors | test_subtilis_converter.cpp:702-715 (GLB exit 5, glTF package exit 0, DOM keeps extensionsUsed, full validate passes); :756-797 rich fixture with external buffers, animation, image and unknown extension: IDs preserved, package relocatable; test_subtilis_document.cpp:265-277 (unknown required rejected, unsupported optional payload readable, required DEREN with bad payload rejected); document.cpp:159-184 filters only DEREN from the parser copy | non-self-contained glTF + listed unknown extension: exit 5 "unknown extension prevents safe GLB relocation"; same input with --format gltf: exit 0 and DOM preserved. **See Important I-1: the self-contained-GLB arm of the refusal rule is missing** | Partial (Important gap) |
| 3 | legal DAG cross-group output, equal-error thresholds, near-plane/scaling: no duplicate/missing geometry from tree recursion or mid-scan patching | dag fixtures: subtilis_dag_fixtures.hpp cross_dag is a real multi-parent DAG (G0/G1 outputs split into G2/G3); test_subtilis_dag.cpp:27-39 cuts at threshold 0/nextafter(.1)=.1/.2 boundaries, :63-73 mixed cut {5,7,8}, :75-120 screen projection, nonuniform scale, near-plane crossing, NaN view, unsupported handedness, 128 seeded camera distances, :121-132 owned-lifetime; cut_closure helper tested with inconsistent raw decisions and idempotence; test_subtilis_dag_builder.cpp:221-244 automatic cross-group witness, :246-272 LOD0-only and multi-root; consumer-side fix evidence in report section 3 and dag.cpp:55-183 | A tree-recursive or scan-time-mutating cut would produce a different set for the cross DAG; exact expected cluster sets fail if any region is duplicated or omitted on this fixture. The real-asset error-0 cut multiset was also independently checked by me (section 7.4) | Pass (with the coverage-property note M-9) |
| 4 | oversized/corrupt payload, quantized/sparse input: validate ranges and decode semantics before allocating/hashing | test_subtilis_codec.cpp:74-139 corrupt header/version/section/stride/range/padding/UTF-8/meta; codec.cpp:140-190 rejects before allocating decoded vectors (scalar sphere/group/meta checks then read_ids); digest.cpp:105-131 rejects nonfinite/out-of-range before hashing; test_subtilis_document.cpp:312-369 quantized interleaved strides with signed normalization, sparse accessor windows, out-of-range/bad stride rejection; accessor.cpp:51-148 preflight for fastgltf validate() dereference preconditions and accessor/sparse window+alignment | Byte-mutated decode cases each expect rejection; quantized/sparse fixtures are positive/negative pairs; preflight exists because fastgltf 0.9 validate() dereferences sparse views unchecked (verified in vendored fastgltf.cpp:997-1002,1084) | Pass |
| 5 | out-of-order parallel completion, workspace yield, repeated conversion/write failure: stable identity, source unchanged, final target complete | test_subtilis_parallel.cpp: forced completion order 2,0,3,1 commits 0,1,2,3 (:228), prepare-failure reports lowest index (:245), budget admission 999 rejects / 1000 -> 1 / 2500 -> 2 / generous -> 4 (:253-357), threads 1/2/12 and 1000-thread clamp byte-identical, adjacent rejection keeps IDs (:475), budget shrink monotonic and whole-primitive rejection (:486-519), no state leak (:563), same-mesh multi-instance deterministic; test_subtilis_converter.cpp:809-931 fault injection (reported short write, silently short resource caught by staged hash/validation, close failure, final rename failure, overwrite rollback restores prior target; no staging leftovers), :969-1021 cross-volume arm, :779-783 source files unchanged, :1199-1230 reuse without re-append; document_edit/replace_indices tested through section A | Each fault plan is a counterexample the code must survive with prior bytes intact; tests assert the *absence* of staging leftovers and byte equality of prior targets | Pass in the same-volume arms; cross-volume arm SKIPPED in this environment (section 6.4) |

## 3. Spec coverage map (plan self-check, line 175)

| Spec area | Plan task mapping | Verified evidence | Holds? |
|---|---|---|---|
| spec 1-2 (goals/boundaries, stack/deps, no runtime change) | Task 1 / Task 8 | tools/subtilis standalone CMake: no Vulkan/CUDA (CMakeCache has CMAKE_DISABLE_FIND_PACKAGE_Vulkan=TRUE; ninja target list has 0 vulkan/cuda matches; exe imports only KERNEL32/libc++/shared_utility); git diff 07e365bd..HEAD changes no source/engine, no shader, no root CMakeLists after ec773f19; yyjson pinned 0.12.0 with SHA-256 manifest verified against the three vendored files | Yes |
| DAG relations / error / cut | Task 2 / Task 5 | dag fixture + threshold/equal/near-plane/scale/NaN cut tests; independent full validator in dag.cpp; real asset cut counts; oriented LOD0 error-0 multiset (independently rechecked) | Yes |
| boundary / geometry profile | Task 3 / Task 4 | topology.cpp tests: shared edges, UV seam, hard normals, duplicate position different attributes, disconnected, non-manifold, no-progress carry; eligibility tests: non-indexed, missing NORMAL/UV, TANGENT/unknown attributes, MASK/BLEND, morph, skin, compression; real specified input (551/551 TANGENT primitives) exits 4 | Yes |
| wire / hash | Task 1 / Task 2 | seven-section codec round-trip/corruption tests; independent golden hash constants in test_subtilis_codec.cpp:140,156; digest scheme matches docs/extensions/DEREN_subtilis/README.md field by field (verified digest.cpp:105-154) | Yes |
| fidelity / CLI | Task 3 / Task 5 / Task 6 | document tests + converter sections A-K; independent LOD0/core check on archived bistro output (section 7.4); CLI exit codes match tools README (0/2/3/4/5) | Yes (with M-5/M-6 notes) |
| parallel / budget / profile | Task 7 / Task 8 | parallel test suite; stage profile rows, inclusive flags, accumulated sum checks; measurement matrix independently recomputed | Yes |

## 4. Findings

### Critical

None found.

### Important

#### I-1 The "refuse GLB output for unknown binary-reference semantics" rule is not applied to self-contained GLB inputs

Requirement: design spec line 168 ("when an extension carries unknown binary-reference semantics,
do not claim lossless repacking; refuse that GLB output and suggest keeping the glTF package
structure") and tools/subtilis/README.md:62-64 ("Unknown extensions or buffer metadata that cannot
relocate safely cause an explicit error for GLB and are preserved by the glTF package").

Code path:
- tools/subtilis/package.cpp:40 GLB output calls serialize_embedded_glb(document).
- tools/subtilis/document.cpp:53-66 serialize_embedded_glb: if the document is self-contained and no
  image carries a uri, it returns make_glb(...) at :65 with no extension check at all; otherwise it
  calls pack_glb at :57/:61.
- tools/subtilis/resources.cpp:86-95 pack_glb runs the extensionsUsed allowlist only under
  "!d.state->self_contained" (:88), so a self-contained GLB whose image forces pack_glb also skips it.

Reproduction (probes written under build-subtilis/review-probes, gitignored; same inputs are easy to
re-create from fixtures/small.glb by editing the JSON chunk with the generator-style helpers):
1. Self-contained GLB, no images, extensionsUsed=["VENDOR_binary"], root
   extensions.VENDOR_binary={"blob":"vendor.bin"}:
   build a3.glb -o a3-out.glb  -> observed exit 0, GLB written; expected (rule) exit 5 with the
   "unknown extension prevents safe GLB relocation" message and no output.
2. Self-contained GLB, extensionsUsed=["VENDOR_binary"],
   extensions.VENDOR_binary={"uri":"vendor-blob.bin"}, plus a data-URI PNG image (forces pack_glb):
   build a.glb -o a-out.glb -> observed exit 0; the output GLB still contains
   "uri":"vendor-blob.bin" in the unknown extension, which now resolves relative to the *new output
   directory* and is dangling. Expected: refusal.
3. Contrast (rule works for the non-self-contained arm): the same unknown extension listed in a glTF
   with an external buffer -> observed exit 5 and message
   "deren-subtilis: unknown extension prevents safe GLB relocation; use a glTF package"; -o with
   --format gltf -> exit 0 and the extension preserved (this is correct and covered by
   test_subtilis_converter.cpp:702-715).
4. Non-conformant variant (extension object present but not in extensionsUsed) on a non-self-contained
   glTF also exits 0 with the dangling unknown uri; see Minor M-1.

Why it matters: a valid-by-construction GLB input with an unknown extension can be re-serialized into
a differently located single file while claiming the conversion is safe; the unknown reference
semantics are exactly what the design says must not be guessed. The existing test arm covers only the
non-self-contained path, so the gap is untested rather than knowingly waived.

GREEN verification proposal (implementation is the Lead's, not mine):
1. Centralize a precondition, e.g. package_detail/glb_relocation_is_safe(document): walk
   extensionsUsed, fail on anything outside {DEREN_subtilis, KHR_mesh_quantization,
   KHR_texture_transform, KHR_materials_unlit, KHR_lights_punctual}, exactly as resources.cpp:88-95
   does today minus the self_contained guard.
2. Call it for every GLB output: in serialize_embedded_glb before both the make_glb branch and the
   pack_glb call, and remove the "!self_contained" guard in pack_glb (or make the guard redundant).
3. Add tests to test_subtilis_converter.cpp section F: (a) self-contained GLB + unknown ext listed,
   no images -> exit 5, no output; (b) self-contained GLB + unknown ext external uri + data-URI image
   -> exit 5; (c) same file with --format gltf -> exit 0 and preserved; (d) positive control: the
   known-extension cooked fixture (e.g. archived outputs/small.glb) still converts/reuses and stays
   byte-identical (I verified that arm currently exits 0 with reused=1 and identical bytes).
4. Regression: rerun Release+ASan ctest, the converter test, and one real-asset build; the real asset
   matrix has no unknown extensions, so its output must remain byte-identical
   (b235aa40...40b47), which is itself a strong no-regression check.

### Minor

- M-1 Unknown extension object present in the DOM but absent from extensionsUsed bypasses the GLB
  allowlist (allowlist reads extensionsUsed only, resources.cpp:87-95). Repro: non-self-contained
  glTF with extensions.VENDOR_binary={"uri":"vendor-blob.bin"} and no extensionsUsed entry ->
  build -> exit 0 and the output GLB keeps the dangling uri. This is a non-conformant input (a
  conformant file must list used extensions), so it is a hardening item: either scan DOM extension
  objects (cheap enough at tool level) or document that conformance is a precondition.
- M-2 read_local_resource (tools/subtilis/resources.cpp:28-44) accepts ".." path segments, so a
  buffer/image URI can read files outside the asset directory and embed their bytes into the output.
  The pinned Khronos validator reports no URI problem for such input (verified), so this is within
  its accepted input space; it is a trust-boundary/hardening decision, not a conformance bug.
  Suggest an explicit decision plus a test: either reject ".." with a typed error or document that
  local parent references are read (bounded by input byte limits) and therefore that untrusted glTF
  should be converted in a sandboxed directory.
- M-3 Dead publication code: publish_replace/publish_no_replace are declared
  (tools/subtilis/document_internal.hpp:58-59) and defined (tools/subtilis/package_native.cpp:22-44)
  but never called; the live path is package.cpp publish_staged. Remove or cover them; today the
  Windows branches of those two functions are unexercised.
- M-4 Dead local variable `canonical` in scripts/measure/verify_subtilis_converter.py:533 (assigned
  and never used). Harmless; remove when touching the harness.
- M-5 CLI --help text (tools/subtilis/main.cpp:6-11) omits --format glb|gltf and the validate --full
  spelling; README has them, so it is only discoverability.
- M-6 Plan Task 8 bullet "run the small asset, the synthetic large grid and
  D:/deren-assets/bistro/bistro-geometry.gltf, threads 1/12 x 3 rounds each" is marked [x], but the
  literal input could not be converted (551/551 primitives carry TANGENT; verified independently) and
  only one failing invocation is recorded (results/specified-input.json, exit 4). The substitution of
  bistro-merged.gltf is documented in report sections 1/8 and recovery.md, but the checkbox does not
  say so inline. Recommend an explicit one-line deviation note next to the plan bullet (no profile
  widening, no code change).
- M-7 The standalone executable imports shared_utility.dll at runtime (verified in the PE imports),
  and tools/subtilis/README.md does not tell a user to keep the DLL next to the exe; in-tree build
  and CI happen to put both in the same directory.
- M-8 The independent full validator compares region component *count* plus both directed boundary
  multisets (dag.cpp:154-183,287-289), while the builder's acceptance additionally checks
  one-position-class-per-component ownership (topology.cpp preserves_region). No counterexample was
  found: geometric "pinched components" is still rejected (dag.cpp:136-150) and the boundary
  multisets plus that pinch rule appear to constrain component re-pairing. This is a strength note,
  not a demonstrated hole; consider adding the per-component ownership comparison to make_asset in a
  later hardening pass (with a constructed counterexample fixture) or documenting that the builder
  check is the stronger one.
- M-9 Plan Task 2's "independent region coverage reference" is currently expressed as fixed expected
  cluster sets on the cross-DAG fixture plus 128 seeded threshold/camera cases, not as a general
  invariant test ("a cut expands to the source triangle multiset exactly once, selected clusters are
  an antichain"). My independent check verified the error-0 real-asset cut multiset (section 7.4),
  but the committed repo has no property test for non-zero thresholds. Suggest one test that
  expands a cut on the synthetic cross-DAG and compares the triangle multiset against the source.

## 5. What the existing architecture/test gaps make awkward (review notes)

- The GLB safety rule is split between three files (document.cpp choose-path, resources.cpp pack_glb
  allowlist, package.cpp entry) with two independent early exits, which is how the self-contained
  branch was missed. A single precondition call is the structural fix (I-1).
- The fastgltf parser is fed a mutable JSON copy with DEREN_subtilis filtered from
  extensionsRequired (document.cpp:159-184); that is necessary to parse our own extension but means
  "extension listed as required" is only enforced for the payload arms the tool checks itself. The
  tests cover this, and the design documents the rule.
- Failure-path determinism: run_ordered reports one of the failing tasks' typed errors
  (parallel.cpp:76-89), so with several simultaneous reduction failures the reported context is
  whichever failed first in scheduling order. The design only requires no partial asset on failure,
  which holds, so this is a known limitation rather than a finding (listed in section 6.7).

## 6. Unverified claims, missing evidence and residual risks

Stated as residual/unverified even though several are documented by the authors; none of these was
used to reach the section 8 recommendation, but the Lead should see them in one place.

6.1 Windows ASan has no LeakSanitizer. I re-ran the ASan/UBSan ctest (6/6 pass, 26.5 s); no leak
    coverage exists on this platform. The CI workflow comments and the evidence report state this
    honestly. Linux CI/LSan remains a follow-up.
6.2 Converter peak memory. The peak is a 20 ms-polled working-set sample and a lower bound
    (measurement environment string in measurements.json); it is not a proven process cap, and the
    known-work accounting covers only the tool's own buffers plus task estimates (meshoptimizer
    temporaries excluded). A Linux CI run with peak-RSS/LSan is still the place to bound this. The
    parallel arm's held prepared results are visible as +56 MiB sampled on Bistro (report section 2),
    consistent with the design note.
6.3 CI workflow was never executed on GitHub in this session (local-commit rule). I only simulated
    its assertions locally: ninja target list contains no vulkan/cuda target; ctest registers exactly
    6 tests; the standalone CMakeCache was configured with Vulkan find disabled. Its first real run
    status is not claimed by the evidence report either.
6.4 Cross-volume staging: the test arm exists (test_subtilis_converter.cpp:969-1021) but SKIPPED in
    this environment with "NOTE: cross-volume glTF package check SKIPPED (no second writable volume
    available to this process; last probe error: access denied)". archive RECOVERY.md records the same
    limitation for the Task 8 runs. Code reading shows staging is created in the destination parent
    directory for both GLB (package.cpp:312-323) and glTF package (package.cpp:426-431), and the
    rollback order is tested same-volume, but the actual cross-volume path was not executed in the
    evidence and not by me; treat as reasoned-by-inspection, not measured.
6.5 Plan-specified real asset bistro-geometry.gltf is TANGENT-ineligible (verified 551/551
    primitives carry TANGENT, 1,753,630 triangles; the file hash matches specified-input.json, and a
    converter run exits 4 with the documented message). The accepted real-asset matrix uses
    bistro-merged.gltf; the substitution is documented but the plan checkbox is literally not the
    asset it names (M-6).
6.6 Multiple simultaneous reduction failures return one typed error context (checkpoint acknowledges
    this; see section 5). Determinism of the *failure message* under a given budget is not asserted by
    any test; only success-path determinism is.
6.7 Measurement traceability: I independently recomputed every median and per-round table from the
    archived measurements.json and cross-checked the raw archived CLI report JSONs (section 7.6); this
    matched exactly. The "Lead independently rebuilt it and reproduced sha256 b235aa40..." claim in
    the evidence report is about the Lead's own action; I could not verify that claim as such, but I
    independently reproduced the same sha256 from committed sources (section 7.3), which is stronger
    for the reviewer's purpose.
6.8 The RED half of the seam-sharing validator fix is a manually witnessed RED (report section 3:
    4699 checks/1 failed with the restored pre-fix validator; GREEN 4705 checks/0 failed). I verified
    the GREEN from the committed tree (test_subtilis_dag_builder: 4705 checks/0 failed, line
    "seam-sharing full-validation regression: 26 groups, 11 rounds, 17 cross-group repartitions, 23
    rejects"), and I verified the root cause by reading the pre-fix dag.cpp (git show 071cfdd5: it
    used one attribute-class graph with a pinch rule). I did not restore the pre-fix validator (that
    would be a worktree mutation), so the RED numbers themselves are unverified; the substantive
    claim (old rule rejects legitimate seam-sharing reductions) is supported by the pre-fix code and
    by the design's two-graph rule.
6.9 Root engine regression (design acceptance item 8): I did not rebuild the root engine tree during
    this review. Structural evidence: the converter range changes no source/engine, no shader and no
    root CMakeLists after commit ec773f19; the CMake helper extraction in ec773f19 moved the
    vstd/mimalloc/shared_utility/fastgltf/meshoptimizer target definitions verbatim (names, files,
    flags, DLL prefix, Windows libraries preserved; only explicit BASE_DIRS and the new yyjson target
    were added) and the Task 1 checkpoint records root Release 58/58 after it. I did run the root
    test_docs binary against the current tree: 395 checks, 0 failed, PASS. A one-shot root
    build+ctest on committed HEAD is still the clean GREEN for this item.
6.10 Vendored yyjson provenance: manifest.sha256 matches the three vendored files byte-for-byte and
     VERSIONS.md records version 0.12.0 + commit 8b4a38dc...; the "Git blob SHA-1 checked against the
     pinned upstream response" claim needs network/upstream access to re-verify and was not
     re-verified here (the SHA-256 content pin is verified).

## 7. Independent verification actually performed (evidence ledger)

All commands run read-only against the worktree; build outputs went to gitignored trees.

7.1 Standalone tree is in sync with committed source and green.
    - cmake --build build-subtilis -j 12 -> "ninja: no work to do" (tree current with HEAD).
    - ctest --test-dir build-subtilis -> 6/6 passed (1.24 s).
    - cmake --build build-subtilis-asan -j 12; ctest --test-dir build-subtilis-asan -> 6/6 passed
      (26.47 s), ASan+UBSan, no leak detection (platform limitation).
    - Direct binary runs (independent of ctest): codec 71 checks, dag 297, document 153,
      dag_builder 4705 (seam regression line above), parallel 581, converter 518: all 0 failed.
      Parallel test printed the forced order 2,0,3,1 -> commit 0,1,2,3, the exact admission
      boundaries (999 rejects, 1000 -> 1, 2500 -> 2, generous -> 4), adjacent-rejection ID stability,
      budget-shrink monotonicity and the 1/2/12-thread byte-identical GLB line.
    - Root test_docs.exe -> "[test_docs] 395 checks, 0 failed -> PASS" (current tree).

7.2 Pinned Khronos validator acceptance.
    - Executable SHA-256 4388a152ff90b68c6430ae03862e05e257a9d50a500ed7d0eb1cd420dc75ff96 matches
      scripts/measure/khronos_validator_manifest.json; version 2.0.0-dev.3.10.
    - gltf_validator --stdout on archived outputs/bistro-merged.glb -> 0 errors, 0 warnings,
      486 infos, 1016 hints, one DEREN_subtilis UNSUPPORTED_EXTENSION info: matches the evidence
      report table and acceptance.json.

7.3 Real-asset reproducibility from committed sources.
    - Fresh build of D:/deren-assets/bistro/bistro-merged.gltf with default (12) threads:
      converted=231, skipped=23, lod0Only=57; output 247,474,732 B,
      sha256 b235aa4008a57a7db38aa1f87593114867efa685753f05616acc020a02840b47 = the report claim
      and the archived canonical output. This simultaneously corroborates cross-thread byte identity
      (the archived copies came from the threads=1 runs per measurements.json).
    - validate --full on that output: "validated 231 payload(s), full=true", exit 0; small/large
      archived outputs: 1 payload each, exit 0.
    - inspect on archived bistro output: 231 payload lines, exactly 57 with groups=0.
    - bistro-geometry.gltf run: exit 4 with the documented message; 551/551 primitives carry
      TANGENT (independent JSON scan); its SHA-256 matches specified-input.json.
    - Recorded converter binary sha256 fa9755...1233e4 equals the hash of the binary I rebuilt from
      the committed tree (no work to do), so the measurement binary and the reviewed source coincide.

7.4 Independent LOD0/cut checker (own Python GLB/glTF reader, not the repo harness).
    - Archived source vs normal bistro output: for all 254 primitives the LOD0 index lists are equal
      and POSITION/NORMAL/TEXCOORD_0 element bytes are equal; nodes/scenes/scene/materials/textures/
      images/samplers JSON are equal; 1016 source accessors are unchanged and 1016 source bufferViews
      keep their bytes (the merged buffer lands at offset 0, so even bufferView offsets are equal);
      231 payload bufferViews appended; extensionsUsed == ["DEREN_subtilis"].
    - Archived cut previews: triangle counts 4,209,006 / 3,686,904 / 3,332,400 / 3,214,166 for
      errors 0 / 0.01 / 0.1 / 1.0, exactly the acceptance.json claims, monotone non-increasing; at
      error 0 the oriented triangle multiset equals the source LOD0 multiset for every primitive
      (0 problems). Small/large normal outputs also pass the same LOD0 identity check.
    - The 12 archived preview files match acceptance.json bytes and SHA-256.

7.5 Evidence archive (D:/deren-assets/2026-10-21-subtilis-converter).
    - Full re-hash of SHA256SUMS.json: 62/62 files present, 0 mismatches, total 1,409,060,280 B =
      manifest total (the 1,343.8 MiB claim); RECOVERY.md's re-derivation commands reviewed.
    - Archive manifest records source_checkout "071cfdd5 (Task 7) + uncommitted Task 8 worktree",
      consistent with the commit timestamps of ed8aa587/20e05cb9.

7.6 Measurement matrix recomputation (own script over archived raw data).
    - For every asset x thread x round: medians recomputed from all three rounds for external wall,
      peak working set, output bytes, converted/skipped/lod0Only, rounds_max, cross-group
      repartitions, groups_final, report wall, accumulated and all eleven stage rows: matches the
      summary JSON. Per-run accumulated == sum of exclusive stage rows within 0.6 ms (the candidate
      row is the only inclusive row; every other row inclusive=false).
    - Per-round external wall lists match the report text (small 295.8/50.0/50.1 and
      49.3/50.5/49.4; large 1576.8/1559.1/1519.0 and 988.7/1013.4/1037.4; bistro
      35586.7/34054.0/36433.3 and 14598.5/14281.3/15082.7); medians 1559.1, 1013.4, 35586.7,
      14598.5 as tabulated.
    - Structural counters: bistro 10 rounds / 556 cross-group / 4,618 groups; large 15 / 246 / 345;
      small 3 / 1 / 3, all match. Raw archived CLI report JSONs agree with the transcribed report
      values (converted/skipped/lod0Only/threads, per-primitive wall sum, rejected aggregation).
    - Rejection totals over the six runs per asset: small {boundaryChanged:30};
      large {unsafeTopology:1878, boundaryChanged:3294};
      bistro {noProgress:32964, unsafeTopology:33270, numericError:66, boundaryChanged:15522}:
      exactly the report table.
    - Fixture regeneration from make_subtilis_converter_fixtures.py reproduced the recorded
      SHA-256 of both fixtures (4f57ae..., 0895f9...); bistro-merged source JSON/bin SHA-256 match
      real-asset.json; bistro-geometry SHA-256 matches specified-input.json.

7.7 Gate harness.
    - verify_subtilis_converter.py gate --self-test -> "self-test: 11 cases, 0 failed".
    - verify_subtilis_converter.py gate on the archived full measurements.json -> "GATE PASS:
      0 finding(s)".

7.8 Packaging-rule probes used for I-1/M-1/M-2 (files under build-subtilis/review-probes; all
    inputs derived from the recorded fixture small.glb by editing the JSON chunk only).
    - self-contained GLB + unknown ext listed, no images -> exit 0 (I-1 repro 1).
    - self-contained GLB + unknown ext external uri + data-URI image -> exit 0, output keeps
      "vendor-blob.bin" (I-1 repro 2).
    - non-self-contained glTF + unknown ext listed -> exit 5 with the rule message (contrast).
    - same with --format gltf -> exit 0, DOM preserved (contrast).
    - unknown extension object unlisted in extensionsUsed, non-self-contained glTF -> exit 0 with
      dangling unknown uri (M-1).
    - buffer uri "../outside.bin" -> exit 0, bytes read from outside the asset directory (M-2).
    - known-extension self-contained cooked GLB (archived outputs/small.glb) -> exit 0, reused=1,
      output byte-identical (451dcd9a...6ec36d): the positive control a fix must preserve.

## 8. Acceptance recommendation

- No Critical finding was found. The DAG/wire/cut/builder/parallel/packaging core, the real-asset
  evidence, both validators, the LOD0/cut identity claims, the measurement matrix and the archive
  hash chain all held up under independent reproduction (section 7). The committed standalone tree
  builds and all six tests pass in Release and under ASan/UBSan.
- Important finding I-1 is a real deviation from an explicit design requirement (spec line 168) and
  from the tools README claim, in a reachable input class, and it is untested. It does not invalidate
  the measured Bistro/DAG evidence (that asset has no unknown extensions and my rebuild matched
  byte-for-byte). Recommendation: fix I-1 with the GREEN steps in section 4 (allowlist precondition
  for every GLB path plus the four negative/positive tests), or have the Lead explicitly accept it
  as a documented limitation and correct tools/subtilis/README.md:62-64 and the plan Task 6 wording
  so the shipped claim matches the behavior.
- Minor findings M-1..M-9 are record-and-decide items; none blocks the committed evidence, but M-1,
  M-2, M-6 and M-8/M-9 are worth a decision line in the final handoff.
- Residual risks in section 6 (LSan, proven peak memory, GitHub CI first run, cross-volume execution,
  plan-specified asset substitution, RED witness, root engine re-run) should be carried into the
  Lead's acceptance record instead of being read as verified.
- I therefore recommend: do not mark the whole change set accepted until I-1 is fixed or explicitly
  waived, and until the section 6 residuals are recorded; everything else reviewed here supports
  acceptance.
