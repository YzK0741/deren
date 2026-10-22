# DEREN_subtilis — private draft v1

This optional glTF primitive extension stores a shared-vertex cluster DAG.
The standard primitive, including its original LOD0 indices, remains usable.
Register a vendor prefix before public release. The full design is in
[the design specification](../../superpowers/specs/2026-10-21-subtilis-gltf-design.md).

The extension object has `version: 1`, `profile: "shared-vertices"`, and an
integer `bufferView >= 0`. Add `DEREN_subtilis` to `extensionsUsed`, not
`extensionsRequired`. The referenced bufferView has no vertex/index target.
The JSON schema is in [schema/mesh.primitive.DEREN_subtilis.schema.json](schema/mesh.primitive.DEREN_subtilis.schema.json).

## Wire format

Integers are unsigned little-endian; floats are IEEE754 binary32. Offsets
are payload-relative. Host struct layout and pointer alignment are irrelevant.

| Header offset | Bytes | Value |
| --- | --- | --- |
| 0 | 8 | `DRNSDAG\0` |
| 8, 10 | 2 each | major=1, minor=0 |
| 12 | 4 | headerBytes=128 |
| 16 | 8 | totalBytes equals bufferView byteLength |
| 24, 28 | 4 each | sectionCount=7, flags=0 |
| 32, 64 | 32 each | sourceHash, buildHash (raw SHA-256) |
| 96 | 32 | zero |

Seven 32-byte descriptors follow: type:u32, stride:u32, count:u64,
offset:u64, length:u64. Every type below occurs exactly once. Unknown types,
duplicates, overflow, overlapping ranges, invalid stride/length, and nonzero
padding are rejected. Section offsets are multiples of eight and at least
352. The canonical encoder orders by type and pads every section to eight
bytes, including the final section. Empty sections have zero length.
Count and byte limits are checked before allocating decoded records.

| Type | Section | Stride | Record |
| --- | --- | --- | --- |
| 1 | INDICES | 4 | original primitive vertex index |
| 2 | CLUSTERS | 32 | firstIndex, indexCount, producer, consumer (u32 each), geometry sphere (x,y,z,r f32) |
| 3 | GROUPS | 48 | fineFirst, fineCount, coarseFirst, coarseCount (u32 each), LOD sphere (4 f32), error:f32, 12 zero bytes |
| 4 | REFERENCES | 4 | cluster ID |
| 5 | LEAVES | 4 | LOD0 cluster ID |
| 6 | ROOTS | 4 | unconsumed cluster ID |
| 7 | BUILD_META | 1 | strict UTF-8 JSON object |

NONE is `0xffffffff`. Spheres and errors must be finite; radii/errors must be
nonnegative. Wire decoding is **not** DAG or geometry validation; the immutable
validated-asset factory performs those checks separately.

## Canonical identity encoding

Every string below is a u32 byte length followed by UTF-8 bytes, without
a terminating NUL. Counts/IDs are u32 LE. Floats are f32 LE, both zero
signs encode as positive zero; nonfinite attributes/options are rejected.
The stream is fed directly into SHA-256, without C++ object padding.

Source fields, in order:

1. Strings `DEREN_subtilis.source.v1`, `shared-vertices`; mode:u32=4, vertexCount:u32.
2. For each attribute in order POSITION, NORMAL, TEXCOORD_0: its string name,
   vertexCount:u32, then row-major f32 components (3, 3, 2 per vertex).
3. sourceIndexCount:u32 and original ordered u32 triangle indices.
4. Strings `OPAQUE`, `POSITION,NORMAL,TEXCOORD_0`; UV set:u32=0.

Missing UV is normalized to zero only when document eligibility allows it.
Attribute counts must match; source indices must be nonempty triangles and
in range. Node transforms, material pixels, paths, and JSON formatting are excluded.

Build fields, in order:

1. Strings `DEREN_subtilis.build.v1`, `shared-vertices`, `subtilis-dag-v1`.
2. triangleLimit, vertexLimit, targetGroupSize, maxRounds as u32.
3. targetRatio, relativeError, normalWeight, uvWeight as f32.
4. Strings `meshoptimizer-1.3`, `9e1f07b159d3cb777f1c67ed31fc11fd117986f4`,
   `fastgltf-0.9.0`, `simdjson-4.6.9`, `yyjson-0.12.0`.

Defaults are 85/256/8/16 and 0.5/0.01/0.1/0.01. Thread count, scheduling,
timings, devices, and budgets are excluded. Successful builds must not change
geometry to accommodate a smaller execution budget. Fingerprints bind identity
and caching; they do not replace full geometry validation.

## CPU reference selection

`make_asset` validates source binding, triangle coverage/winding, directed
boundary/component preservation, metric monotonicity and sphere containment.
Payload decoding alone never establishes those properties.

`select_cut` uses `<=` at an error threshold, closes all group decisions along
dependencies, then scans cluster IDs in stable order. Model mode uses the stored
group error. Screen mode accepts column-major affine model/view matrices and
standard right-handed zero-to-one depth perspective/orthographic projections.
With `S = sqrt(||view*model||1 * ||view*model||infinity)` on the linear 3x3 and
`P = max(abs(proj[0])*width, abs(proj[5])*height)/2`, the orthographic metric is
`error*S*P`; perspective divides by the closest depth of the transformed LOD
sphere. Near-plane crossings prohibit coarse selection. Unsupported/nonfinite
views return complete LOD0; insufficient capacity returns an error, never a prefix.

## Runtime consumer

The engine consumes v1 `shared-vertices` payloads without changing this wire format.
The app's single `[render] subtilis=true` switch requests loader extraction and
engine adoption; default OFF loads leave payload bytes unparsed. Enabling only
the runtime switch after an OFF startup cannot import an absent hierarchy.

The loader owns raw bytes/metadata; the engine checks profile/material eligibility,
source fingerprints, finite data, index windows and DAG structure. Full geometric
validation stays in the trusted offline pipeline (`validate --full`), rather than
being repeated at load time. Static OPAQUE POSITION/NORMAL/optional TEXCOORD_0
profiles are supported; extra attributes such as TANGENT are not adopted.

Source/payload identities share immutable CPU data and GPU generations across
instances. Payload leaves form the complete LOD0 item run; only the main-view cut
uses appended coarse indices. Shadow/BLAS and unsupported strategies retain LOD0.

Malformed, unsupported, source-mismatched or LOD0-only payloads retain ordinary
glTF geometry with typed fallback counts. Admission preserves 4096 visible items,
4096 selected records per item and the 65536-record shared table. A legal cut may
need more records than its leaf reservation; it falls back as a whole, never a
prefix. The existing visibility admission can refuse an entire frame.

The default 1px target is configurable with `subtilis_error_px`; optional
`subtilis_hysteresis=false` keeps reference selection deterministic by default.
See [Subtilis](../../subtilis.md) and the [runtime acceptance report](../../reviews/2026-10-21-object-lod.md)
for lifecycle, configuration, source-asset limitations and measurement evidence.
