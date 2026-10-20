# meshoptimizer

- Upstream: https://github.com/zeux/meshoptimizer
- Version: v1.3, commit `9e1f07b159d3cb777f1c67ed31fc11fd117986f4`
- License: MIT, see LICENSE.md. Uses meshoptimizer. Copyright (c) 2016-2026, Arseny Kapoulkine.
- Source archive: https://codeload.github.com/zeux/meshoptimizer/tar.gz/9e1f07b159d3cb777f1c67ed31fc11fd117986f4
- Archive SHA256: `e753b45fbc410031bd69cb278180ae31fa0d5430d0c3e662e3a3b100467ce0b3`
- Local changes: none. Checked-in src/ and license are byte-identical to upstream; manifest.sha256 records each file.
- Build: private static library; only allocator, clusterizer, partition, simplifier and indexgenerator are compiled for Subtilis.
- Demos, gltfpack, JavaScript and codecs are not built. No configure-time downloads.
- Project calls pass through the private builder adapter, which initializes allocation callbacks once using utility::memory.
