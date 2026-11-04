"""Tests of offline retargeting math, independent of downloaded motion assets."""
import importlib.util
import math
from pathlib import Path
import struct
import unittest

PATH = Path(__file__).resolve().parents[2] / "scripts/assets/convert_mmd_motion.py"
spec = importlib.util.spec_from_file_location("convert_mmd_motion", PATH)
motion = importlib.util.module_from_spec(spec)
spec.loader.exec_module(motion)


class MotionTests(unittest.TestCase):
    def test_identity_keeps_nonidentity_rest_basis(self):
        rest = motion.normalize([0.2, -0.3, 0.4, 0.5])
        parent = motion.normalize([0.1, 0.3, -0.2, 0.8])
        world = motion.multiply(parent, rest)
        result = motion.local_rotation(parent, world, [0, 0, 0, 1])
        self.assertAlmostEqual(abs(sum(a*b for a, b in zip(result, rest))), 1)

    def test_reflection_converts_rotation_and_vector_consistently(self):
        q = motion.normalize([0.2, 0.1, 0.3, 0.9])
        v = [0.5, -0.2, 0.8]
        a = motion.rotate(motion.reflect(q), [v[0], v[1], -v[2]])
        b = motion.rotate(q, v)
        for x, y in zip(a, [b[0], b[1], -b[2]]):
            self.assertAlmostEqual(x, y)

    def test_bone_offset_is_preserved(self):
        nodes = [{"name": "Root", "children": [1]},
                 {"name": "Bip001", "translation": [0, 1, 0], "children": [2]},
                 {"name": "Bip001_Pelvis", "rotation": [0, 0, 0, 1], "children": [3]},
                 {"name": "Bip001_Spine", "translation": [0, 0.2, 0]}]
        rotations, translations = motion.retarget_frame(nodes, {}, 0, 0.08)
        self.assertEqual(translations, {1: [0, 1, 0]})
        self.assertEqual(nodes[3]["translation"], [0, 0.2, 0])
        self.assertAlmostEqual(rotations[3][3], 1)

    def test_vmd_truncation_is_rejected(self):
        with self.assertRaises(ValueError):
            motion.parse_vmd(b"Vocaloid Motion Data 0002".ljust(50, b"\0") + struct.pack("<I", 1))

    def test_shortest_arc(self):
        q = motion.slerp([0, 0, 0, 1], [0, 0, 0, -1], 0.5)
        self.assertAlmostEqual(abs(q[3]), 1)

    def test_rotation_curve_does_not_read_physics_flags(self):
        raw = bytearray(64)
        for c in range(4):
            for offset, value in zip((0, 4, 8, 12), (20, 20, 107, 107)):
                raw[c+offset] = value
        raw[2], raw[3], raw[17], raw[18] = 0, 0, 20, 20
        self.assertAlmostEqual(motion.bezier(raw, 3, 0.5), 0.5, places=6)

    def test_source_t_pose_alignment_keeps_lengths_and_foot_frame(self):
        q = [0, 0, -math.sin(math.pi/8), math.cos(math.pi/8)]
        nodes = [{"name": "Root", "children": [1]},
                 {"name": "Bip001_L_UpperArm", "rotation": q, "children": [2]},
                 {"name": "Bip001_L_Forearm", "translation": [0.2, 0, 0], "children": [3]},
                 {"name": "Bip001_L_Hand", "translation": [0.3, 0, 0]},
                 {"name": "Bip001_L_Foot", "rotation": q}]
        rig = motion.rig_data(nodes)
        reference = motion.reference_rest(nodes, rig, "mmd-t")
        direction = motion.rotate(reference[1], [0.2, 0, 0])
        for actual, expected in zip(direction, [0.2, 0, 0]):
            self.assertAlmostEqual(actual, expected)
        self.assertEqual(reference[4], rig[1][4])
        self.assertEqual(nodes[2]["translation"], [0.2, 0, 0])

    def test_poser_leg_reference_keeps_foot_and_accounts_for_ancestor_scale(self):
        nodes = [{"name": "Root", "scale": [1, 2, 1], "children": [1, 4]}]
        for side in ("L", "R"):
            thigh = len(nodes)
            nodes.extend([
                {"name": f"Bip001_{side}_Thigh", "children": [thigh + 1]},
                {"name": f"Bip001_{side}_Calf", "translation": [0, -0.45, 0],
                 "children": [thigh + 2]},
                {"name": f"Bip001_{side}_Foot", "translation": [0, -0.4, 0]},
            ])
        rig = motion.rig_data(nodes)
        reference = motion.reference_rest(nodes, rig, "poser-a")
        self.assertGreater(motion.rotate(reference[1], [0, -1, 0])[2], 0)
        self.assertLess(motion.rotate(reference[2], [0, -1, 0])[2], 0)
        self.assertEqual(reference[3], rig[1][3])
        self.assertAlmostEqual(motion.poser_unit_scale(nodes, rig),
                               1.7 / (math.hypot(4.5, 0.15) + math.hypot(4, 0.15)))


if __name__ == "__main__":
    unittest.main()
