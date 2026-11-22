import json
from pathlib import Path
import tempfile
import unittest

from export_windows_game import copy_project, export, read_toml


class windows_export_test(unittest.TestCase):
    def setUp(self):
        workspace = tempfile.TemporaryDirectory()
        self.addCleanup(workspace.cleanup)
        self.root = Path(workspace.name).resolve()
        self.project = self.root / "Source Project"
        self.target = self.root / "Output"
        library = self.root / "asset library"
        for asset in ("helmet", "extra", "unused"):
            folder = library / asset
            folder.mkdir(parents=True)
            (folder / "asset.toml").write_text(f'id = "{asset}"\n', encoding="utf-8")
            (folder / "model.glb").write_bytes(b"fixture")
        app = self.root / "Host.dll"
        app.write_bytes(b"fixture")
        (self.project / "scenes").mkdir(parents=True)
        (self.project / "scenes/main.scene.toml").write_text(
            '[document]\nversion = 3\n[[node]]\nasset = "helmet"\n', encoding="utf-8",
        )
        (self.project / "render.toml").write_text(
            '[render]\nwindow_width = 800\n', encoding="utf-8",
        )
        (self.project / "deren.toml").write_text(
            '[project]\nname = "Example"\nmain_scene = "render.toml"\n\n'
            f'[paths]\nasset_library = {json.dumps(library.as_posix())}\n\n'
            f'[scripts]\nassembly = {json.dumps(app.as_posix())}\n\n'
            '[game]\nscene = "scenes/main.scene.toml"\n\n[input.actions]\nmove = ["a"]\n',
            encoding="utf-8",
        )

    def test_copies_referenced_assets_and_explicit_dynamic_assets(self):
        copy_project(self.project, self.target, [], ["extra"])
        exported = self.target / "project"
        manifest = read_toml(exported / "deren.toml")
        self.assertEqual(manifest["scripts"]["assembly"], "app/Host.dll")
        self.assertEqual(manifest["paths"]["asset_library"], "asset")
        self.assertEqual(manifest["input"]["actions"]["move"], ["a"])
        self.assertTrue((exported / manifest["game"]["scene"]).is_file())
        self.assertEqual({path.name for path in (exported / "asset").iterdir() if path.is_dir()}, {"helmet", "extra"})

    def test_shared_motion_copies_its_buffer(self):
        library = self.root / "asset library"
        motions = library / "motions"
        motions.mkdir()
        (motions / "walk.gltf").write_text('{"buffers":[{"uri":"walk.bin"}]}', encoding="utf-8")
        (motions / "walk.bin").write_bytes(b"motion")
        (motions / "unused.bin").write_bytes(b"unused")
        (library / "helmet" / "asset.toml").write_text(
            'id = "helmet"\nmotion = "../motions/walk.gltf"\n', encoding="utf-8",
        )
        copy_project(self.project, self.target, [], [])
        packaged = self.target / "project" / "asset" / "motions"
        self.assertTrue((packaged / "walk.gltf").is_file())
        self.assertEqual((packaged / "walk.bin").read_bytes(), b"motion")
        self.assertFalse((packaged / "unused.bin").exists())

    def test_missing_dynamic_asset_fails(self):
        with self.assertRaisesRegex(ValueError, "Assets not found.*missing"):
            copy_project(self.project, self.target, [], ["missing"])

    def test_existing_export_is_preserved(self):
        self.target.mkdir()
        marker = self.target / "keep.txt"
        marker.write_text("original", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "already exists"):
            export(self.root, self.project, self.target, self.root, [], [])
        self.assertEqual(marker.read_text(encoding="utf-8"), "original")


if __name__ == "__main__":
    unittest.main()
