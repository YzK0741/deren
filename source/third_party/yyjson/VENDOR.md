# yyjson

- Upstream: https://github.com/ibireme/yyjson
- Version: 0.12.0, commit `8b4a38dc994a110abaec8a400615567bd996105f`.
- License: MIT, see LICENSE.
- Files: only upstream `src/yyjson.c`, `src/yyjson.h`, and `LICENSE`.
- Local changes: none. Each file's Git blob SHA-1 was checked against the
  pinned upstream repository response before generating manifest.sha256.
- Acquisition: connected GitHub file API; direct raw-file download was unavailable.
- Build: plain-C `yyjson_vendored` static target without upstream configuration,
  network downloads, tests, or tools. The converter supplies utility::memory
  allocation callbacks so JSON allocations use the process-wide mimalloc service.
- Purpose: preserve and edit the original glTF document, including unknown JSON
  fields. fastgltf continues to use the separately vendored simdjson for decoding.
