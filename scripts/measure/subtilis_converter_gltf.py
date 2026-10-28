"""Minimal glTF/GLB reader shared by the subtilis converter acceptance scripts.

Only the feature subset the offline converter fixtures and the Bistro geometry asset use:
triangle primitives, non-sparse accessors with float/unsigned integer components, GLB chunks
and external or data-URI buffers. Anything outside that subset raises instead of guessing.

Requires Python 3 and NumPy (the same requirement as scripts/measure/prepare_bistro_geometry.py).
"""

import base64
import hashlib
import json
import math
import struct
import urllib.parse
from pathlib import Path

import numpy as np

_GLB_MAGIC = 0x46546C67
_GLB_JSON = 0x4E4F534A
_GLB_BIN = 0x004E4942
_COMPONENT = {
    5120: ("b", 1),
    5121: ("B", 1),
    5122: ("h", 2),
    5123: ("H", 2),
    5125: ("I", 4),
    5126: ("f", 4),
}
_WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4, "MAT3": 9, "MAT4": 16}


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def load_gltf(path):
    """Return {'json': dict, 'buffers': [bytes], 'encoding': 'glb'|'gltf', 'path': Path}."""
    path = Path(path)
    raw = path.read_bytes()
    if len(raw) >= 12 and struct.unpack_from("<I", raw, 0)[0] == _GLB_MAGIC:
        version, length = struct.unpack_from("<II", raw, 4)
        if version != 2 or length != len(raw):
            raise ValueError(f"{path}: unsupported GLB header version={version} length={length} real={len(raw)}")
        offset = 12
        document_json = None
        binary = None
        while offset + 8 <= len(raw):
            chunk_length, chunk_type = struct.unpack_from("<II", raw, offset)
            offset += 8
            if offset + chunk_length > len(raw):
                raise ValueError(f"{path}: GLB chunk runs past the file end")
            payload = raw[offset : offset + chunk_length]
            offset += chunk_length
            if chunk_type == _GLB_JSON and document_json is None:
                document_json = json.loads(payload.decode("utf-8"))
            elif chunk_type == _GLB_BIN and binary is None:
                binary = payload
        if document_json is None:
            raise ValueError(f"{path}: GLB has no JSON chunk")
        buffers = []
        for entry in document_json.get("buffers", []):
            if "uri" in entry:
                buffers.append(_read_uri(path.parent, entry["uri"]))
            elif binary is not None and len(buffers) == 0:
                buffers.append(binary)
            else:
                raise ValueError(f"{path}: GLB buffer without a BIN chunk")
        return {"json": document_json, "buffers": buffers, "encoding": "glb", "path": path}
    document_json = json.loads(raw.decode("utf-8"))
    buffers = [_read_uri(path.parent, entry["uri"]) for entry in document_json.get("buffers", [])]
    return {"json": document_json, "buffers": buffers, "encoding": "gltf", "path": path}


def _read_uri(parent, uri):
    if uri.startswith("data:"):
        header, _, payload = uri.partition(",")
        if not header.endswith(";base64"):
            raise ValueError("only base64 data URIs are supported")
        return base64.b64decode(payload)
    return (parent / urllib.parse.unquote(uri)).read_bytes()


def accessor(document, index):
    """Decode accessor `index` into a NumPy array of shape (count, width) without guessing."""
    entry = document["json"]["accessors"][index]
    if "sparse" in entry:
        raise ValueError(f"accessor {index}: sparse accessors are not supported by this reader")
    if entry.get("normalized"):
        raise ValueError(f"accessor {index}: normalized accessors are not supported by this reader")
    code, item_size = _COMPONENT[entry["componentType"]]
    width = _WIDTH[entry["type"]]
    count = entry["count"]
    dtype = np.dtype("<" + code)
    if "bufferView" not in entry:
        raise ValueError(f"accessor {index}: accessors without a bufferView are not supported")
    view = document["json"]["bufferViews"][entry["bufferView"]]
    buffer_bytes = document["buffers"][view["buffer"]]
    base = view.get("byteOffset", 0) + entry.get("byteOffset", 0)
    stride = view.get("byteStride", item_size * width)
    if stride < item_size * width:
        raise ValueError(f"accessor {index}: byteStride smaller than the element size")
    needed = base + stride * (count - 1) + item_size * width
    if needed > len(buffer_bytes):
        raise ValueError(f"accessor {index}: range exceeds its buffer view")
    if stride == item_size * width:
        return np.frombuffer(buffer_bytes, dtype=dtype, count=count * width, offset=base).reshape(count, width).copy()
    flat = np.empty(count * width, dtype=dtype)
    for row in range(count):
        at = base + row * stride
        flat[row * width : (row + 1) * width] = np.frombuffer(buffer_bytes, dtype=dtype, count=width, offset=at)
    return flat.reshape(count, width)


def attribute(document, primitive, name):
    return accessor(document, primitive["attributes"][name])


def indices(document, primitive):
    """Flattened triangle indices as int64; a non-indexed primitive is supported too."""
    if "mode" in primitive and primitive["mode"] != 4:
        raise ValueError(f"primitive mode {primitive['mode']} is not TRIANGLES")
    if "indices" in primitive:
        values = accessor(document, primitive["indices"]).reshape(-1)
    else:
        count = document["json"]["accessors"][primitive["attributes"]["POSITION"]]["count"]
        if count % 3:
            raise ValueError("non-indexed triangle count is not a multiple of three")
        values = np.arange(count, dtype=np.uint32)
    return values.astype(np.int64)


def has_deren_extension(primitive):
    return "DEREN_subtilis" in primitive.get("extensions", {})


def primitives(document):
    for mesh_index, mesh in enumerate(document["json"].get("meshes", [])):
        for primitive_index, primitive in enumerate(mesh.get("primitives", [])):
            yield mesh_index, primitive_index, primitive


def canonical_triangles(flat_indices):
    """Triangles as rows rotated so the smallest vertex id comes first, preserving winding, sorted.

    Two geometries with the same oriented triangle multiset compare equal even if the cluster
    builder reordered the indices, which is exactly the LOD0 identity contract.
    """
    triangles = np.asarray(flat_indices, dtype=np.int64).reshape(-1, 3)
    shift = np.argmin(triangles, axis=1)
    rotated = np.take_along_axis(triangles, (np.arange(3) + shift[:, None]) % 3, axis=1)
    order = np.lexsort((rotated[:, 2], rotated[:, 1], rotated[:, 0]))
    return rotated[order]


def compare_lod0_and_core(source, output):
    """Check that every converted primitive keeps the exact source LOD0 geometry, attributes and
    core resource semantics, and that untouched primitives are unchanged too.

    Returns a list of human-readable problems; empty means accepted.
    """
    problems = []
    source_json, output_json = source["json"], output["json"]
    for field in ("asset", "nodes", "scenes", "materials"):
        if source_json.get(field) != output_json.get(field):
            problems.append(f"{field} differs from the source document")
    for field in ("scene",):
        if source_json.get(field) != output_json.get(field):
            problems.append(f"{field} differs from the source document")
    source_primitives = list(primitives(source))
    output_primitives = list(primitives(output))
    if len(source_primitives) != len(output_primitives):
        problems.append(f"primitive count {len(output_primitives)} != source {len(source_primitives)}")
        return problems
    for (mesh, primitive_index, source_primitive), (_, _, output_primitive) in zip(source_primitives, output_primitives):
        label = f"mesh {mesh} primitive {primitive_index}"
        if source_primitive.get("material") != output_primitive.get("material"):
            problems.append(f"{label}: material index changed")
        if source_primitive.get("mode", 4) != output_primitive.get("mode", 4):
            problems.append(f"{label}: primitive mode changed")
        if sorted(source_primitive.get("attributes", {})) != sorted(output_primitive.get("attributes", {})):
            problems.append(f"{label}: attribute set changed")
            continue
        for name in source_primitive.get("attributes", {}):
            if not np.array_equal(attribute(source, source_primitive, name), attribute(output, output_primitive, name)):
                problems.append(f"{label}: attribute {name} bytes/semantics differ")
        if not np.array_equal(indices(source, source_primitive), indices(output, output_primitive)):
            problems.append(f"{label}: LOD0 index geometry differs")
    return problems


def compare_cut(source, output, threshold):
    """Check one export-cut preview: core semantics + attributes unchanged, only indices replaced."""
    problems = []
    source_json, output_json = source["json"], output["json"]
    for field in ("asset", "nodes", "scenes", "materials", "scene"):
        if source_json.get(field) != output_json.get(field):
            problems.append(f"threshold {threshold}: {field} differs from the source document")
    for mesh, primitive_index, primitive in primitives(output):
        if has_deren_extension(primitive):
            problems.append(f"threshold {threshold}: mesh {mesh} primitive {primitive_index} still declares DEREN_subtilis")
    return problems
