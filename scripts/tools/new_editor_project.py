"""Create a project that uses the built Deren editor as its authoring host."""

import argparse
import json
from pathlib import Path
from uuid import uuid4


def quote(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def create_project(root: Path, assembly: Path, assets: Path | None, name: str) -> None:
    if not assembly.is_file():
        raise ValueError(f"Build the editor first; missing {assembly}")
    if assets is not None and not assets.is_dir():
        raise ValueError(f"Asset library does not exist: {assets}")
    manifest = root / "deren.toml"
    settings = root / "render.toml"
    scene = root / "scenes" / "main.scene.toml"
    for path in (manifest, settings, scene):
        if path.exists():
            raise ValueError(f"Project file already exists: {path}")
    for directory in (scene.parent, root / "asset", root / "scripts"):
        directory.mkdir(parents=True, exist_ok=True)

    settings.write_text(
        '[render]\nwindow_title = "deren editor"\n'
        'window_width = 1600\nwindow_height = 900\n'
        'borderless_window = true\nfxaa = true\n',
        encoding="utf-8",
    )
    scene.write_text(
        f'[document]\nversion = 3\nname = {quote(name)}\n\n'
        f'[[node]]\nuid = "{uuid4()}"\nname = "root"\n',
        encoding="utf-8",
    )
    asset_path = quote(assets.as_posix()) if assets is not None else '"asset"'
    manifest.write_text(
        f'[project]\nname = {quote(name)}\nmain_scene = "render.toml"\n\n'
        f'[paths]\nasset_library = {asset_path}\n\n'
        f'[scripts]\nassembly = {quote(assembly.as_posix())}\n\n'
        '[editor]\nscene = "scenes/main.scene.toml"\n',
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--assembly", type=Path, default=Path("build/source/editor/Game.dll"))
    parser.add_argument("--asset-library", type=Path)
    parser.add_argument("--name")
    args = parser.parse_args()
    root = args.directory.resolve()
    try:
        create_project(
            root, args.assembly.resolve(),
            args.asset_library.resolve() if args.asset_library is not None else None,
            args.name or root.name,
        )
    except (ValueError, OSError) as error:
        parser.error(str(error))
    print(f"Project ready: {root}")


if __name__ == "__main__":
    main()
