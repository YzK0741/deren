# RHI standard cleanup before buffer transfers

2026-10-25. ABI stays 54; no runtime behavior changes in this documentation pass.

- Removed 15 duplicate per-method summaries from the extension clauses, leaving one complete requirement,
  error and lifetime description. Sorting operands remain borrowed through GPU completion.
- Added section 5.1: coordinated target/compiler/standard-library/runtime/ABI-affecting build configuration;
  the C-linkage entry symbol is not an arbitrary-compiler C SDK guarantee.
- Added section 3.1: recoverable caller refusals versus returned device errors and existing fatal backend
  paths. Explicitly recorded VMA upload/mapping and native command setup limitations; did not promise
  recoverable OOM or expand panic to ordinary validation errors.
- Kept the ABI 54 removal and the actual layout/vtable change criteria separate.

Validation: test_docs 429 checks / 0 failures and git diff --check pass. These checks do not prove semantic
consistency; the source return/lifetime paths were inspected. No full rebuild or GPU benchmark is needed
for this documentation-only pass. Buffer transfer implementation is a separate step under its own design.
