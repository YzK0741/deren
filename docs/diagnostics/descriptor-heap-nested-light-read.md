# Descriptor-heap nested light read anomaly

Measured 2026-11-01.

## Current conclusion

This is **not unique to Slang**. Both Slang and an independent GLSL/glslang
build reproduce it: reading a nested light struct from a descriptor-heap
storage buffer drops the outer 352-byte member offset and returns the first
64 bytes of the buffer instead.

The evidence points at the NVIDIA descriptor-heap / nested-structure access
path. It has not yet been cross-checked on another driver version or another
GPU that supports the extension, so no upstream component is finally blamed.
The system driver was not changed and no external issue has been filed yet.

## Environment

- NVIDIA GeForce RTX 4060 Laptop GPU, driver 617.42, Vulkan 1.4.351.
- Slang `2026.13.1-1-g84792eb15`.
- glslang 16.4.0.
- SPIRV-Tools 2026.3.
- `SPV_EXT_descriptor_heap`, resource descriptor stride 64 bytes.
- The same application, model, light buffer and GPU; only a private shader
  directory was changed.
- The installed GameToon shader hash stayed constant after all tests:
  `902a81423855fcaa11549de6d5f303996ca7c3d93596aeddf231783488e39203`.

## Controls that rule out Slang alone

| Test | GPU result |
| --- | --- |
| Original ConstantBuffer nested light array | wrong |
| Slang `-O0` | wrong |
| StructuredBuffer, nested structure kept | wrong |
| Read the four fields separately | wrong |
| One nested light struct, no light array | wrong |
| spirv-opt merges the access chain | wrong |
| Manually expand the SPIR-V chain to the full index | wrong |
| Flatten to direct fields | correct |
| Flatten to a float4 array | correct |
| Byte-addressed read | correct |
| Minimal Slang: nested struct | wrong |
| Minimal GLSL/glslang: nested struct | wrong |
| Minimal Slang: flat fields | correct |
| Minimal GLSL/glslang: flat fields | correct |
| All Vulkan layers disabled: GLSL nested | wrong |
| All Vulkan layers disabled: GLSL flat | correct |

All SPIR-V passes `spirv-val --target-env vulkan1.3`. The original program
and the minimal program both decorate `Offset 352`, `ArrayStride 64`, and the
inner members at 0/16/32/48, matching the CPU layout. Validation alone does
not prove a driver defect; the independent compilers and the address
comparison below are the stronger evidence. The layer-disabled run uses
`VK_LOADER_LAYERS_DISABLE=*`; the log confirms validation layers were disabled
and no device layer was inserted, so OBS and validation layers are not
necessary for the repro.

## Which offset is lost

The minimal GLSL structure is:

```glsl
struct Light { vec4 p; vec4 c; vec4 d; vec4 q; };
layout(descriptor_heap, descriptor_stride=64, std430) readonly buffer Data {
    vec4 prefix[22];
    Light lights[128];
} buffers[];
```

SPIR-V decorates `lights` with `Offset 352`, the light stride as 64, and the
inner fields as 0/16/32/48. The first light should be bytes 352..415.

Another probe in the same fragment reads the same descriptor through a flat
`float4` view. The four fields observed through `lights[0]` equal
`rows[0..3]`, not `rows[22..25]`: the GPU read the buffer prefix. The address
comparison passes with 346245 matching pixels, so this is not a colour
misinterpretation.

Moving padding into the inner light (outer light at offset 0, inner
p/q/c/d at 352/368/384/400) reads correctly, consistent with "the outer
member offset is dropped".

## Reproducer files and evidence

Minimal sources for both compilers and offset probes:

```text
scripts/gates/fixtures/descriptor_heap_nested/
```

They use the current Laevatain captured light as the known data. They are a
shader-level minimization and still need the application's Vulkan host; they
are not a standalone Vulkan executable.

The 18 test sources, SPIR-V, disassembly, validation logs, configuration and
screenshots are in:

```text
tmp/slang-light-investigation/
```

with the summary in `results.json`. All tests used a private shader directory
and did not overwrite the installed shaders.

A standalone reproducer repository for the bug report is maintained
separately; it contains the English report, minimal Slang/GLSL sources,
SPIR-V/disassembly and the result log.

## Engineering handling

Keep the byte-addressed `game_toon_load_light` workaround and the CPU layout
assertions. Do not adjust material parameters or disable punctual lights.
The GPU regression tool `scripts/gates/game_toon_light_reads.py` compares the
actual light fields so a future change cannot silently return to the broken
typed access.

The check was rerun after the diagnosis; frames 4 and 5 pass and the installed
normal GameToon shader hash remains unchanged.
