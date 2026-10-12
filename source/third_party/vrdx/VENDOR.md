# VRDX vendored source

- Upstream: https://github.com/jaesung-cs/vulkan_radix_sort
- Version: 0.6.0
- Revision: `b0db22b0c65e17a9b803bc4daf20354898b820eb`
- Retrieved: 2026-10-11
- License: MIT; original copyright and license are retained in `LICENSE`.
- Local patches: none. All 36 upstream files were copied byte-for-byte, including the generated integration header, original Slang sources, regeneration tools, benchmark sources and documentation.
- Archive: https://codeload.github.com/jaesung-cs/vulkan_radix_sort/zip/b0db22b0c65e17a9b803bc4daf20354898b820eb
- Archive SHA-256: `60f1655e5f705803abfe4044b520fc60ea115ff550d049f0467c59c4c3799f50`
- Integration header SHA-256: `7aa3c9b987b4aacdf4ea805b05d598b3a8cd4183d789854c209dcb4651fb3ff4`

This copy is not yet connected to deren's build or renderer. Integration will use `include/vk_radix_sort.h` privately in the Vulkan backend, defining `VRDX_IMPLEMENTATION` in exactly one translation unit. The header includes the generated SPIR-V; consumers do not need to regenerate it.

Do not add the upstream CMake subtree to the normal build merely to consume this header: its default configuration downloads Slang 2026.11 and regenerates the header. The preserved upstream CMake/tools remain available for deliberate dependency maintenance, and its standalone benchmark also fetches its own dependencies.

Requirements of this revision include Vulkan 1.4 push descriptors, synchronization2, subgroup sizes 32 or 64 and at least 20 KB workgroup shared memory; verify actual enabled features and sorter stability during backend integration. Vendoring alone does not establish GPU correctness or performance.
