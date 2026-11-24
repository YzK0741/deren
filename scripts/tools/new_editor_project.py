"""Create a Deren scene project with game and editor launch modes."""

import argparse
import json
import math
import struct
import subprocess
import wave
from pathlib import Path
from uuid import uuid4
from xml.sax.saxutils import escape


def quote(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def write_demo(root: Path, managed_api: Path) -> None:
    source = root / "script-source"
    source.mkdir()
    (source / "helmet_behaviour.cs").write_text(
        Path(__file__).with_name("editor_demo_behaviour.cs").read_text(encoding="utf-8"),
        encoding="utf-8",
    )
    project = source / "HelmetDemo.csproj"
    project.write_text(
        '<Project Sdk="Microsoft.NET.Sdk">\n'
        '  <PropertyGroup><TargetFramework>net10.0</TargetFramework><Nullable>enable</Nullable></PropertyGroup>\n'
        '  <ItemGroup><Reference Include="Deren.Managed">\n'
        f'    <HintPath>{escape(managed_api.as_posix())}</HintPath><Private>false</Private>\n'
        '  </Reference></ItemGroup>\n'
        '</Project>\n',
        encoding="utf-8",
    )
    subprocess.run(
        ["dotnet", "build", str(project), "--configuration", "Release", "--output", str(root / "scripts")],
        check=True,
    )
    audio_directory = root / "audio"
    audio_directory.mkdir()
    rate = 24000
    samples = rate // 5
    with wave.open(str(audio_directory / "tone.wav"), "wb") as tone:
        tone.setparams((1, 2, rate, samples, "NONE", "not compressed"))
        tone.writeframes(b"".join(
            struct.pack("<h", round(7500 * (1 - i / samples) * math.sin(math.tau * 523.25 * i / rate)))
            for i in range(samples)
        ))


def write_scene(path: Path, name: str, asset: str | None = None, second_scene: bool = False) -> None:
    version = 11 if asset else 3
    content = f'[document]\nversion = {version}\nname = {quote(name)}\n\n[[node]]\nuid = "{uuid4()}"\n'
    if asset is None:
        content += 'name = "root"\n'
    else:
        sound_uid = uuid4()
        door_uid = uuid4()
        content += (
            f'name = "Helmet"\nasset = {quote(asset)}\n'
            'script = "HelmetDemo.dll:editor_demo.helmet_behaviour"\n'
            f'script_parameter."second_scene" = "{second_scene}"\n'
            f'script_parameter."sound" = "{sound_uid}"\n'
            f'script_parameter."door" = "{door_uid}"\n'
            'trigger_target = true\nposition = [0, 0, 0.55]\n'
            f'\n[[node]]\nuid = "{uuid4()}"\nname = "Door Trigger"\n'
            # Tall enough to meet a capsule character: the demo capsule is 0.9 + 0.3 around its origin, so
            # the character centre stands 1.2 above whatever it stands on. The box spans y -3.65..2.35.
            'position = [0.53, -0.65, 0]\nscale = [0.24, 6.0, 0.34]\n'
            'script = "HelmetDemo.dll:editor_demo.door_trigger_behaviour"\nbox_trigger = true\n'
            f'script_parameter."second_scene" = "{second_scene}"\n'
            f'script_parameter."sound" = "{sound_uid}"\n'
            f'\n[[node]]\nuid = "{sound_uid}"\nname = "Door Audio"\n'
            'audio_file = "audio/tone.wav"\naudio_volume = 0.7\naudio_loop = false\n'
        )
        # The demo floor, the start platform and the walls double as visible geometry: their block
        # asset has the scale of their collision box, so a player can see the passage, the obstacle
        # and the floor the character stands on instead of walking into invisible walls.
        for wall, position, scale, extra in (
            ("Workshop Floor", [0, -1.85, 0], [6, 0.2, 6], ""),
            ("Start Platform", [-0.05, -0.22, 0.05], [1.0, 0.2, 1.2], ""),
            ("Door Wall North", [0.32, -0.55, 1.015], [0.06, 2.4, 1.13], ""),
            ("Door Wall South", [0.32, -0.55, -1.015], [0.06, 2.4, 1.13], ""),
            ("Workshop Obstacle", [-0.42, -0.55, 0.30], [0.14, 2.4, 0.32], ""),
        ):
            content += (
                f'\n[[node]]\nuid = "{uuid4()}"\nname = {quote(wall)}\n'
                f'asset = "block"\nposition = {json.dumps(position)}\nscale = {json.dumps(scale)}\n'
                f'collision_box = true\n{extra}'
            )
        # The door fills the gap between the two door walls. It is a moving solid, so the collision
        # follows the node when the demo behaviour lifts it, and the same opening changes from blocked
        # to passable without touching the walls. The gap is 0.9 across so the 0.3-radius character
        # capsule fits through with room to spare, while the closed door still fills it exactly.
        content += (
            f'\n[[node]]\nuid = "{door_uid}"\nname = "Workshop Door"\n'
            'asset = "block"\nposition = [0.32, -0.55, 0]\nscale = [0.06, 2.4, 0.90]\n'
            'collision_box = true\nmoving_solid = true\n'
        )
    path.write_text(content, encoding="utf-8")


def create_project(root: Path, assembly: Path, assets: Path | None, name: str, demo: bool = False) -> None:
    if not assembly.is_file():
        raise ValueError(f"Build the editor first; missing {assembly}")
    if assets is not None and not assets.is_dir():
        raise ValueError(f"Asset library does not exist: {assets}")
    managed_api = assembly.with_name("Deren.Managed.dll")
    if demo and not managed_api.is_file():
        raise ValueError(f"Build the managed API first; missing {managed_api}")
    manifest = root / "deren.toml"
    settings = root / "render.toml"
    scene = root / "scenes" / "main.scene.toml"
    demo_files = (scene.with_name("hangar.scene.toml"), root / "script-source", root / "audio") if demo else ()
    for path in (manifest, settings, scene, *demo_files):
        if path.exists():
            raise ValueError(f"Project file already exists: {path}")
    for directory in (scene.parent, root / "asset", root / "scripts"):
        directory.mkdir(parents=True, exist_ok=True)

    settings.write_text(
        f'[render]\nwindow_title = {quote(name)}\n'
        'window_width = 1600\nwindow_height = 900\n'
        'borderless_window = true\nfxaa = true\n\n[gui]\nshow = false\n',
        encoding="utf-8",
    )
    if demo:
        write_demo(root, managed_api)
        write_scene(scene, "Workshop", "damaged-helmet")
        write_scene(scene.with_name("hangar.scene.toml"), "Hangar", "flight-helmet", second_scene=True)
    else:
        write_scene(scene, name)
    actions = (
        '\n[input.actions]\nplay_tone = ["space"]\nnext_scene = ["enter"]\ninteract = ["e"]\n'
        'move_right = ["d"]\nmove_left = ["a"]\nmove_forward = ["w"]\nmove_back = ["s"]\n'
        if demo else ''
    )
    asset_path = quote(assets.as_posix()) if assets is not None else '"asset"'
    manifest.write_text(
        f'[project]\nname = {quote(name)}\nmain_scene = "render.toml"\n\n'
        f'[paths]\nasset_library = {asset_path}\n\n'
        f'[scripts]\nassembly = {quote(assembly.as_posix())}\n\n'
        '[editor]\nscene = "scenes/main.scene.toml"\n\n'
        '[game]\nscene = "scenes/main.scene.toml"\n'
        + actions,
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--assembly", type=Path, default=Path("build/source/editor/Game.dll"))
    parser.add_argument("--asset-library", type=Path)
    parser.add_argument("--name")
    parser.add_argument("--demo", action="store_true", help="create a two-scene helmet game with controls and audio")
    args = parser.parse_args()
    root = args.directory.resolve()
    assets = args.asset_library
    if args.demo and assets is None:
        assets = Path(__file__).resolve().parents[2] / "asset"
    try:
        create_project(
            root, args.assembly.resolve(),
            assets.resolve() if assets is not None else None,
            args.name or ("Deren Helmet Demo" if args.demo else root.name),
            demo=args.demo,
        )
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.error(str(error))
    print(f"Project ready: {root}")


if __name__ == "__main__":
    main()
