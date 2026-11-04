"""Bake a VMD onto a Bip001 rig as a verified, skeleton-only glTF motion.

Python standard library only. The native deren executable supplies the skeleton
fingerprint, keeping the hash algorithm and canonicalization in one place.
"""
import argparse
import bisect
import copy
import json
import math
from pathlib import Path
import struct
import subprocess
import zipfile

IDENTITY = [0.0, 0.0, 0.0, 1.0]
CENTER = "\u30bb\u30f3\u30bf\u30fc"
LOWER = "\u4e0b\u534a\u8eab"
UPPER = "\u4e0a\u534a\u8eab"
UPPER2 = UPPER + "2"


def normalize(q):
    length = math.sqrt(sum(x*x for x in q))
    if length < 1e-10 or not math.isfinite(length):
        raise ValueError("invalid quaternion")
    return [x/length for x in q]


def multiply(a, b):
    x, y, z, w = a
    X, Y, Z, W = b
    return [w*X+x*W+y*Z-z*Y, w*Y-x*Z+y*W+z*X,
            w*Z+x*Y-y*X+z*W, w*W-x*X-y*Y-z*Z]


def inverse(q):
    x, y, z, w = normalize(q)
    return [-x, -y, -z, w]


def rotate(q, v):
    return multiply(multiply(q, [*v, 0]), inverse(q))[:3]


def reflect(q):
    return [-q[0], -q[1], q[2], q[3]]


def local_rotation(parent_world, rest_world, source_delta):
    return normalize(multiply(inverse(parent_world), multiply(source_delta, rest_world)))


def slerp(a, b, t):
    a, b = normalize(a), normalize(b)
    dot = sum(x*y for x, y in zip(a, b))
    if dot < 0:
        b, dot = [-x for x in b], -dot
    if dot > 0.9995:
        return normalize([x+(y-x)*t for x, y in zip(a, b)])
    angle = math.acos(min(1, dot))
    return [(x*math.sin((1-t)*angle)+y*math.sin(t*angle))/math.sin(angle)
            for x, y in zip(a, b)]


def parse_vmd(data):
    if len(data) < 54 or data[:30].rstrip(b"\0") != b"Vocaloid Motion Data 0002":
        raise ValueError("expected a VMD 0002 motion")
    count = struct.unpack_from("<I", data, 50)[0]
    if count > (len(data)-54)//111:
        raise ValueError("truncated VMD bone records")
    tracks = {}
    for i in range(count):
        offset = 54+i*111
        name = data[offset:offset+15].split(b"\0")[0].decode("cp932")
        frame = struct.unpack_from("<I", data, offset+15)[0]
        values = struct.unpack_from("<7f", data, offset+19)
        if not all(math.isfinite(x) for x in values):
            raise ValueError("non-finite VMD key")
        tracks.setdefault(name, []).append((frame, list(values[:3]), normalize(values[3:]),
                                            data[offset+47:offset+111]))
    for track in tracks.values():
        track.sort(key=lambda k: k[0])
        if any(a[0] == b[0] for a, b in zip(track, track[1:])):
            raise ValueError("duplicate VMD bone key")
    if not tracks:
        raise ValueError("motion has no bone keys")
    return tracks


def bezier(raw, channel, t):
    # Slots 2/3 hold physics flags, not the Z/rotation first control point.
    x1 = raw[(0, 1, 17, 18)[channel]]/127
    y1, x2, y2 = [raw[channel+i]/127 for i in (4, 8, 12)]
    def cubic(a, b, s):
        return 3*(1-s)**2*s*a+3*(1-s)*s*s*b+s**3
    lo, hi = 0.0, 1.0
    for _ in range(24):
        s = (lo+hi)*0.5
        if cubic(x1, x2, s) < t:
            lo = s
        else:
            hi = s
    return cubic(y1, y2, (lo+hi)*0.5)


def sample(track, frame):
    if not track:
        return [0, 0, 0], IDENTITY
    index = bisect.bisect_right(track, frame, key=lambda key: key[0])
    if index == 0 or index == len(track):
        key = track[0] if index == 0 else track[-1]
        return key[1], key[2]
    a, b = track[index-1], track[index]
    if frame == a[0]:
        return a[1], a[2]
    t = (frame-a[0])/(b[0]-a[0])
    position = [x+(y-x)*bezier(b[3], c, t) for c, (x, y) in enumerate(zip(a[1], b[1]))]
    return position, slerp(a[2], b[2], bezier(b[3], 3, t))


def profile():
    # Source chains express MMD world-space rotation deltas. Upper body and
    # lower body are siblings in MMD but a parent/child chain in this target rig.
    result = {
        "Bip001": [CENTER], "Bip001_Pelvis": [CENTER, LOWER],
        "Bip001_Spine": [CENTER, UPPER],
        "Bip001_Spine1": [CENTER, UPPER],
        "Bip001_Spine2": [CENTER, UPPER, UPPER2],
        "Bip001_Neck": [CENTER, UPPER, UPPER2, "\u9996"],
        "Bip001_Head": [CENTER, UPPER, UPPER2, "\u9996", "\u982d"],
    }
    for side, prefix in [("L", "\u5de6"), ("R", "\u53f3")]:
        base = [CENTER, UPPER, UPPER2]
        arm = base+[prefix+"\u80a9"]
        result[f"Bip001_{side}_Clavicle"] = arm
        arm = arm+[prefix+"\u8155"]
        result[f"Bip001_{side}_UpperArm"] = arm
        arm = arm+[prefix+"\u3072\u3058"]
        result[f"Bip001_{side}_Forearm"] = arm
        arm = arm+[prefix+"\u624b\u9996"]
        result[f"Bip001_{side}_Hand"] = arm
        leg = [CENTER, LOWER]
        for target, source in [("Thigh", "\u8db3"), ("Calf", "\u3072\u3056"), ("Foot", "\u8db3\u9996")]:
            leg = leg+[prefix+source]
            result[f"Bip001_{side}_{target}"] = leg
        for finger, source in enumerate(["\u89aa\u6307", "\u4eba\u6307", "\u4e2d\u6307", "\u85ac\u6307", "\u5c0f\u6307"]):
            chain = arm[:]
            for joint in range(3):
                number = joint if finger == 0 else joint+1
                # VMD conventions use full-width digits for these tracks.
                chain = chain+[prefix+source+chr(0xff10+number)]
                suffix = str(finger) + (str(joint) if joint else "")
                result[f"Bip001_{side}_Finger{suffix}"] = chain
    return result


PROFILE = profile()


def rig_data(nodes):
    parents = {c: i for i, n in enumerate(nodes) for c in n.get("children", [])}
    rest = {}
    order = []
    def visit(i):
        if i in rest:
            return
        if i in parents:
            visit(parents[i])
        n = nodes[i]
        if "matrix" in n:
            raise ValueError("Bip001 conversion requires TRS binding nodes")
        rest[i] = normalize(multiply(rest.get(parents.get(i), IDENTITY), n.get("rotation", IDENTITY)))
        order.append(i)
    for i in range(len(nodes)):
        visit(i)
    return parents, rest, order


def from_to(a, b):
    a_length = math.sqrt(sum(v*v for v in a))
    b_length = math.sqrt(sum(v*v for v in b))
    if min(a_length, b_length) < 1e-8:
        raise ValueError("degenerate reference bone direction")
    a, b = [v/a_length for v in a], [v/b_length for v in b]
    dot = sum(x*y for x, y in zip(a, b))
    cross = [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]
    if dot < -0.999999:
        axis = [1, 0, 0] if abs(a[0]) < 0.8 else [0, 1, 0]
        return normalize([a[1]*axis[2]-a[2]*axis[1], a[2]*axis[0]-a[0]*axis[2],
                          a[0]*axis[1]-a[1]*axis[0], 0])
    return normalize([*cross, 1+dot])


# Segment offsets of Poser's standard MMD leg, expressed in glTF axes.
POSER_LEG = ((0, -4.5, 0.15), (0, -4.0, -0.15))


def poser_unit_scale(nodes, rig):
    parents, _, order = rig
    by_name = {node.get("name"): i for i, node in enumerate(nodes)}
    # Carry linear transforms through ancestors, including nonuniform scale.
    axes = {}
    for i in order:
        parent_axes = axes.get(parents.get(i), ((1, 0, 0), (0, 1, 0), (0, 0, 1)))
        rotation = normalize(nodes[i].get("rotation", IDENTITY))
        scale = nodes[i].get("scale", [1, 1, 1])
        axes[i] = []
        for axis, factor in zip(((1, 0, 0), (0, 1, 0), (0, 0, 1)), scale):
            vector = rotate(rotation, [value*factor for value in axis])
            axes[i].append([sum(vector[k]*parent_axes[k][j] for k in range(3)) for j in range(3)])
    lengths = []
    for side in ("L", "R"):
        for joint in ("Calf", "Foot"):
            i = by_name.get(f"Bip001_{side}_{joint}")
            if i is None or i not in parents:
                raise ValueError("automatic motion scale requires both complete legs")
            offset = nodes[i].get("translation", [0, 0, 0])
            basis = axes[parents[i]]
            lengths.append(math.hypot(*(sum(offset[k]*basis[k][j] for k in range(3)) for j in range(3))))
    if any(not math.isfinite(length) or length <= 0 for length in lengths):
        raise ValueError("automatic motion scale requires finite, nonzero leg lengths")
    return sum(lengths) / (2*sum(math.hypot(*segment) for segment in POSER_LEG))


def reference_rest(nodes, rig, source_pose):
    """Align anatomical directions, without changing stored target bind data.

    A VMD has no reference skeleton. These explicit canonical A/T choices are
    approximations, not inference from a filename or the first motion frame.
    Feet retain their own frame: aiming an ankle at a toe would tilt heeled shoes.
    """
    _, rest, _ = rig
    reference = dict(rest)
    if source_pose == "native":
        return reference
    if source_pose not in ("mmd-a", "mmd-t", "poser-a"):
        raise ValueError("unknown source reference pose")
    by_name = {n.get("name"): i for i, n in enumerate(nodes)}
    def align(parent, child, direction):
        if parent not in by_name or child not in by_name:
            return
        i, j = by_name[parent], by_name[child]
        if j not in nodes[i].get("children", []):
            raise ValueError("reference profile expects a direct anatomical child")
        offset = nodes[j].get("translation", [0, 0, 0])
        scale = nodes[i].get("scale", [1, 1, 1])
        original = rotate(rest[i], [a*b for a, b in zip(offset, scale)])
        reference[i] = normalize(multiply(from_to(original, direction), rest[i]))
    for parent, child in [("Pelvis", "Spine"), ("Spine", "Spine1"),
                          ("Spine1", "Spine2"), ("Spine2", "Neck"), ("Neck", "Head")]:
        align("Bip001_"+parent, "Bip001_"+child, [0, 1, 0])
    for side, sign in [("L", 1), ("R", -1)]:
        prefix = f"Bip001_{side}_"
        arm = [sign, 0 if source_pose == "mmd-t" else -1, 0]
        align(prefix+"Clavicle", prefix+"UpperArm", [sign, 0, 0])
        align(prefix+"UpperArm", prefix+"Forearm", arm)
        align(prefix+"Forearm", prefix+"Hand", arm)
        align(prefix+"Hand", prefix+"Finger2", arm)
        thigh, calf = POSER_LEG if source_pose == "poser-a" else ((0, -1, 0), (0, -1, 0))
        align(prefix+"Thigh", prefix+"Calf", thigh)
        align(prefix+"Calf", prefix+"Foot", calf)
    return reference


def retarget_frame(nodes, tracks, frame, unit_scale, rig=None, reference=None):
    parents, rest, order = rig or rig_data(nodes)
    reference = reference or rest
    world, rotations, translations = {}, {}, {}
    for i in order:
        n = nodes[i]
        parent = world.get(parents.get(i), IDENTITY)
        chain = PROFILE.get(n.get("name"))
        if chain is not None:
            if "_Finger" in n.get("name", ""):
                # Independent finger keys live in the calibrated native parent's
                # bind space; carry the aligned hand instead of cancelling it.
                basis = inverse(rest.get(parents.get(i), IDENTITY))
                key = reflect(sample(tracks.get(chain[-1]), frame)[1])
                delta = multiply(multiply(basis, key), inverse(basis))
                rotations[i] = normalize(multiply(delta, n.get("rotation", IDENTITY)))
            else:
                delta = IDENTITY
                for name in chain:
                    delta = multiply(delta, reflect(sample(tracks.get(name), frame)[1]))
                rotations[i] = local_rotation(parent, reference[i], delta)
            world[i] = normalize(multiply(parent, rotations[i]))
        else:
            world[i] = normalize(multiply(parent, n.get("rotation", IDENTITY)))
        if n.get("name") == "Bip001":
            p = sample(tracks.get(CENTER), frame)[0]
            offset = [p[0]*unit_scale, p[1]*unit_scale, -p[2]*unit_scale]
            offset = rotate(inverse(rest.get(parents.get(i), IDENTITY)), offset)
            translations[i] = [a+b for a, b in zip(n.get("translation", [0, 0, 0]), offset)]
    return rotations, translations


def read_model(path):
    data = path.read_bytes()
    if data[:4] == b"glTF":
        size, kind = struct.unpack_from("<II", data, 12)
        if kind != 0x4e4f534a:
            raise ValueError("missing GLB JSON chunk")
        return json.loads(data[20:20+size])
    return json.loads(data)


def binding_nodes(model):
    nodes = model["nodes"]
    parents = {c: i for i, n in enumerate(nodes) for c in n.get("children", [])}
    joints = {j for skin in model["skins"] for j in skin["joints"]}
    selected = set(joints)
    for joint in joints:
        while joint in parents:
            joint = parents[joint]
            selected.add(joint)
    indices = sorted(selected)
    mapping = {old: new for new, old in enumerate(indices)}
    result = []
    for old in indices:
        n = {k: copy.deepcopy(v) for k, v in nodes[old].items()
             if k in ("name", "translation", "rotation", "scale", "matrix")}
        children = [mapping[c] for c in nodes[old].get("children", []) if c in mapping]
        if children:
            n["children"] = children
        result.append(n)
    roots = [mapping[i] for i in indices if i not in parents or parents[i] not in selected]
    return result, roots, [mapping[i] for i in sorted(joints)]


def convert(model_path, vmd_path, output, deren, unit_scale, source_pose="mmd-a"):
    if unit_scale is not None and (not math.isfinite(unit_scale) or unit_scale <= 0):
        raise ValueError("MMD unit scale must be positive and finite")
    result = subprocess.run([str(deren.resolve()), "--skeleton-hash", str(model_path.resolve())],
                            capture_output=True, text=True, encoding="utf-8", errors="replace", check=True)
    fingerprint = next(line.split("=", 1)[1].strip() for line in result.stdout.splitlines()
                       if line.startswith("skeleton_hash="))
    if vmd_path.suffix.lower() == ".zip":
        with zipfile.ZipFile(vmd_path) as archive:
            entries = [n for n in archive.namelist() if n.lower().endswith(".vmd")]
            if len(entries) != 1:
                raise ValueError("ZIP must contain exactly one VMD")
            data = archive.read(entries[0])
    else:
        data = vmd_path.read_bytes()
    tracks = parse_vmd(data)
    nodes, roots, joints = binding_nodes(read_model(model_path))
    if not {"Bip001", "Bip001_Pelvis", "Bip001_Spine2", "Bip001_Head"}.issubset({n.get("name") for n in nodes}):
        raise ValueError("target is not the supported Bip001 humanoid rig")
    rig = rig_data(nodes)
    if unit_scale is None:
        unit_scale = poser_unit_scale(nodes, rig) if source_pose == "poser-a" else 0.08
    print(f"Root motion scale: {unit_scale:.6f}")
    reference = reference_rest(nodes, rig, source_pose)
    last = max(track[-1][0] for track in tracks.values())
    if last > 30*60*10:
        raise ValueError("motion exceeds the ten-minute conversion limit")
    rotations, translations = {}, {}
    for frame in range(last+1):
        r, t = retarget_frame(nodes, tracks, frame, unit_scale, rig, reference)
        for i, q in r.items():
            values = rotations.setdefault(i, [])
            if values and sum(a*b for a, b in zip(values[-4:], q)) < 0:
                q = [-x for x in q]
            values.extend(q)
        for i, p in t.items():
            translations.setdefault(i, []).extend(p)
    binary = bytearray()
    views, accessors = [], []
    def accessor(values, width, bounds=False):
        start = len(binary)
        binary.extend(struct.pack(f"<{len(values)}f", *values))
        views.append({"buffer": 0, "byteOffset": start, "byteLength": len(binary)-start})
        item = {"bufferView": len(views)-1, "componentType": 5126, "count": len(values)//width,
                "type": {1: "SCALAR", 3: "VEC3", 4: "VEC4"}[width]}
        if bounds:
            item.update(min=[min(values)], max=[max(values)])
        accessors.append(item)
        return len(accessors)-1
    time = accessor([i/30 for i in range(last+1)], 1, True)
    samplers, channels = [], []
    for path, table, width in [("rotation", rotations, 4), ("translation", translations, 3)]:
        for i, values in table.items():
            samplers.append({"input": time, "output": accessor(values, width), "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers)-1, "target": {"node": i, "path": path}})
    output.mkdir(parents=True, exist_ok=True)
    document = {"asset": {"version": "2.0", "generator": "deren Bip001 motion converter"},
                "scene": 0, "scenes": [{"nodes": roots}], "nodes": nodes,
                "buffers": [{"uri": "motion.bin", "byteLength": len(binary)}],
                "bufferViews": views, "accessors": accessors,
                "animations": [{"name": "Bibideba", "samplers": samplers, "channels": channels}],
                "extras": {"deren_motion": {"binding": "bip001-rest-v1", "skeleton_hash": fingerprint,
                                             "joints": joints, "mmd_unit_scale": unit_scale,
                                             "source_reference_pose": source_pose,
                                             "source_fps": 30, "last_frame": last}}}
    (output/"motion.bin").write_bytes(binary)
    destination = output/"motion.gltf"
    destination.write_text(json.dumps(document, indent=2, ensure_ascii=True)+"\n", encoding="utf-8")
    verify = subprocess.run([str(deren.resolve()), "--verify-motion", str(model_path.resolve()), str(destination.resolve())],
                            capture_output=True, text=True, encoding="utf-8", errors="replace")
    if verify.returncode:
        raise ValueError("native motion verification failed: "+verify.stdout+verify.stderr)
    print(verify.stdout.strip())
    used = {name for n in nodes for name in PROFILE.get(n.get("name"), [])}
    unsupported = sorted(set(tracks)-used)
    print(f"Saved {destination}: {last/30:.3f}s, {len(channels)} channels, {len(binary)} bytes")
    print("Unmapped source tracks: "+", ".join(n.encode("unicode_escape").decode("ascii") for n in unsupported))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--vmd", required=True, type=Path, help="VMD file or ZIP containing one VMD")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--deren", required=True, type=Path)
    parser.add_argument("--mmd-unit", type=float,
                        help="override the root motion scale (poser-a uses leg lengths, others use 0.08)")
    parser.add_argument("--source-pose", choices=["mmd-a", "mmd-t", "native", "poser-a"], default="mmd-a",
                        help="explicit source rest-pose convention; VMD does not contain a reference rig")
    args = parser.parse_args()
    convert(args.model, args.vmd, args.output, args.deren, args.mmd_unit, args.source_pose)


if __name__ == "__main__":
    main()
