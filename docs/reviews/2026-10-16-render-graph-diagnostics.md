# Render graph diagnostics and RHI mapping audit

Later design clarification supersedes the recommendation below to make frontend
layouts match GENERAL: frontend states are abstract and may differ from native
layouts. See the current [semantic mapping documentation](../render_graph_rhi_mapping.md).

Adds explicit dump_json/dump_text helpers for compiled_graph and graph_error.
The existing CPU algorithms and production rendering behavior are unchanged.
No automatic logging, file writes, crash hooks, RHI calls or GPU submission are
introduced. Existing unrelated working-tree changes were preserved.

## Output contract

- Owned vessel::string output, schema_version 1, deterministic record ordering.
- Resources include identity, shape, initial state, declared logical content and
  an inclusive use interval measured in live execution positions.
- Passes include extracted accesses, snapshot presence, live/side-effect flags
  and coarse unreachable-value culling reason. Arbitrary user values are opaque.
- Dependencies, all declared reads, outputs, execution order, barriers and planned
  final state frontiers retain their ranges and state information.
- uint64 values use decimal strings, invalid indices use null. Stage/access masks
  retain symbolic names and numeric bits. Valid UTF-8 is preserved; invalid bytes
  become replacement characters. Control characters and embedded NUL are escaped.
- Text output labels one record per line and reuses the JSON field serializers.
  Neither format is a binary serialization or a validator for manually modified
  results. Interval derivation bounds-checks referenced indices.

Synthetic fixture outputs, generated from the tested executable:
[JSON](2026-10-16-render-graph-diagnostics/sample.json),
[text](2026-10-16-render-graph-diagnostics/sample.txt),
[error](2026-10-16-render-graph-diagnostics/error.json).
These are CPU test fixtures, not captured production frames.

## Mapping audit result

The [manual audit](../render_graph_rhi_mapping.md) checks the current RHI contract,
Vulkan role-pair table, stage-hint replacement, buffer masks, aspect inference,
barrier-group capacity, layout recipes and real graphics Bloom recording path.

The backend uses GENERAL for ordinary image operations, whereas the baseline
graph assigns distinct layouts. One shared image stage_hint and finite role pairs
also cannot represent every independent endpoint scope or aspect subset. The audit
therefore does not certify an executable mapping. It recommends an explicit graph
GENERAL policy and an additive, versioned RHI synchronization contract before GPU
integration, while retaining all memory dependencies and legacy callers.

No backend/ABI/layout behavior was changed by this increment.

## Verification

Windows Clang64, C++23, exceptions/RTTI disabled:

- RED: test_render_graph_dump failed compilation on absent dump APIs.
- GREEN: 18 C++ checks pass, including determinism, precision, escaping, empty
  graphs/errors, unused intervals and safely handling modified record indices.
- Python independently parses JSON and every text record, compares field parity,
  and validates schema semantics, all byte values, valid 2/3/4-byte UTF-8, overlong,
  surrogate, out-of-range and truncated input sequences.
- Boost allocation probe: 285 checks, ordinary-new delta zero in measured path,
  now including JSON and text generation from the actual graph module.
- Full Release build -j12 succeeded; complete CTest 37/37 passed.
- ASan/UBSan build -j12 succeeded; focused CTest 8/8 passed, including both dump
  tests and existing vendor/Boost/A1/range/parameter/synchronization tests.
- Backend artifact gate explicitly includes librender_graph_core.a: zero symbols,
  reference sites and owning STL crossings. Full native gate passed.
- Formatting dry-run, documentation and CI-list check, and git diff --check passed.
- Independent read-only review found no actionable issues in implementation,
  schema, integration or source grounding of the mapping audit. Its optional
  suggestion to parse text bodies independently was implemented and passed in
  the full Release and final sanitizer runs.

Evidence in ignored build trees: build-release-dyn-clang64/render-graph-dump-
{red,build,full-build,full-ctest,boundary}.log and boundary.json;
build-vessel-asan-clang64/render-graph-dump-{build,test}.log. No Linux/MSVC or GPU
validation was run. No commit/push or old-path removal.

Next: explicit GENERAL policy and independent RHI source/destination scopes,
followed by validated execution preparation and Bloom boundary integration.
