# Shader source organization

Moved all 41 active Slang files from the flat source/shaders directory into
common, geometry, scene, lighting, toon, gaussian, post and debug folders.
Compile scripts remain at the root. Includes are qualified relative to that
root; CMake and both manual compilation tables use the new source paths.
SPIR-V output names and runtime loading names are unchanged.

Updated active source/documentation references and tests that read shader files.
Shader source checks now recursively scan subfolders, using root-relative source
names for mesh-stage coverage. Path computation is lexical: filesystem::relative
was rejected by Windows sandbox canonicalization, while no filesystem lookup is
needed for these iterator paths. Archived references and third-party code were
not relocated.

Validation:

- Full Release build passed, recompiling 45 SPIR-V stages (41 application stages
  plus four traversal-test stages).
- All 46 existing build-tree SPIR-V hashes match their pre-move values, including
  the extra preexisting output; all 45 recompiled stages are unchanged.
- PowerShell and POSIX manual scripts each compiled 41 application stages.
  The final manual outputs match CMake byte-for-byte.
- Full CTest: 24/24 passed. Existing hard-link boundary tests ran outside the
  sandbox. Shader path checks themselves also passed inside the sandbox.
- Changed shader-reading test sources pass clang-format; git diff --check passes.

This is a source organization change with no shader behavior change. No commit
or push performed; existing Gaussian work and user-owned assets are preserved.
