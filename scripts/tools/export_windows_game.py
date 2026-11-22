"""Export a scene project with a private Windows and .NET runtime."""

import argparse
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import tempfile
import tomllib
from urllib.parse import unquote, urlsplit

NATIVE_FILES = (
    "deren.exe", "deren_assets.dll", "deren_audio.dll", "deren_vulkan.dll",
    "deren_gui_vulkan.dll", "shared_utility.dll", "libc++.dll",
)
BRIDGE_FILES = ("Deren.Managed.dll", "Deren.Managed.deps.json", "Deren.Managed.runtimeconfig.json", "nethost.dll")


def read_toml(path: Path) -> dict:
    with path.open("rb") as source:
        return tomllib.load(source)


def copy_path(source: Path, destination: Path) -> None:
    if source.is_symlink() or source.is_junction():
        raise ValueError(f"Export requires regular files and directories: {source}")
    if source.is_dir():
        destination.mkdir(parents=True, exist_ok=True)
        for child in source.iterdir():
            copy_path(child, destination / child.name)
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)


def project_path(root: Path, value: str, label: str) -> Path:
    path = Path(value)
    if not value or path.anchor or ".." in path.parts or not (root / path).resolve().is_relative_to(root):
        raise ValueError(f"{label} must refer to a relative path inside the project: {value!r}")
    return path


def pe_machine(path: Path) -> int:
    with path.open("rb") as binary:
        if binary.read(2) != b"MZ":
            raise ValueError(f"Not a Windows executable: {path}")
        binary.seek(0x3C)
        data = binary.read(4)
        if len(data) == 4:
            binary.seek(struct.unpack("<I", data)[0])
            header = binary.read(6)
            if header.startswith(b"PE\0\0") and len(header) == 6:
                return struct.unpack("<H", header[4:])[0]
    raise ValueError(f"Invalid Windows executable header: {path}")


def bundle_runtime(build: Path, output: Path, dotnet_root: Path) -> None:
    for filename in NATIVE_FILES:
        copy_path(build / filename, output / filename)
    for filename in ("libunwind.dll", "libwinpthread-1.dll"):
        if (build / filename).is_file():
            copy_path(build / filename, output / filename)
    for directory in ("shaders", "environments"):
        if directory == "shaders" or (build / directory).exists():
            copy_path(build / directory, output / directory)
    for filename in BRIDGE_FILES:
        copy_path(build / "managed" / filename, output / "managed" / filename)

    config = json.loads((build / "managed" / "Deren.Managed.runtimeconfig.json").read_text(encoding="utf-8"))
    framework = config["runtimeOptions"]["framework"]
    minimum = tuple(map(int, framework["version"].split(".")))
    shared = dotnet_root / "shared" / framework["name"]
    machine = pe_machine(build / "deren.exe")
    versions = sorted(
        (tuple(map(int, entry.name.split("."))), entry.name)
        for entry in shared.iterdir() if entry.is_dir() and re.fullmatch(r"\d+\.\d+\.\d+", entry.name)
    )
    for version, name in reversed(versions):
        fxr = dotnet_root / "host" / "fxr" / name
        if (
            version[:2] == minimum[:2] and version >= minimum and (fxr / "hostfxr.dll").is_file()
            and pe_machine(shared / name / "coreclr.dll") == machine
            and pe_machine(fxr / "hostfxr.dll") == machine
        ):
            bundle = output / "managed" / "dotnet"
            copy_path(shared / name, bundle / "shared" / framework["name"] / name)
            copy_path(fxr, bundle / "host" / "fxr" / name)
            for license_file in ("LICENSE.txt", "ThirdPartyNotices.txt"):
                copy_path(dotnet_root / license_file, bundle / license_file)
            return
    raise ValueError(f"No compatible Windows {framework['name']} {framework['version']} runtime in {dotnet_root}")


def collect_scene_assets(scenes: Path) -> set[str]:
    assets = set()
    for path in scenes.rglob("*.scene.toml"):
        for node in read_toml(path).get("node", []):
            if node.get("asset"):
                assets.add(node["asset"])
    return assets


def asset_file(library: Path, base: Path, name: str) -> Path:
    path = Path(name)
    resolved = (base / path).resolve()
    if path.anchor or not resolved.is_relative_to(library) or not resolved.is_file():
        raise ValueError(f"Asset dependency must be a local file in {library}: {name!r} from {base}")
    return resolved


def gltf_document(model: Path) -> dict:
    if model.suffix.lower() == ".gltf":
        return json.loads(model.read_text(encoding="utf-8-sig"))
    if model.suffix.lower() == ".glb":
        with model.open("rb") as binary:
            header = binary.read(20)
            if len(header) != 20:
                raise ValueError(f"Invalid GLB header: {model}")
            magic, version, size, length, chunk_type = struct.unpack("<4sIII4s", header)
            if magic != b"glTF" or version != 2 or chunk_type != b"JSON" or not 0 < length <= size - 20:
                raise ValueError(f"Invalid GLB JSON chunk: {model}")
            return json.loads(binary.read(length))
    return {}


def copy_asset_files(library: Path, destination: Path, entry: Path) -> None:
    manifest = read_toml(entry / "asset.toml")
    for field in ("model", "motion"):
        if value := manifest.get(field):
            model = asset_file(library, entry, value)
            document = gltf_document(model)
            dependencies = [model]
            for item in document.get("buffers", []) + document.get("images", []):
                if not (uri := item.get("uri")):
                    continue
                parsed = urlsplit(uri)
                if parsed.scheme == "data":
                    continue
                if parsed.scheme or parsed.netloc or parsed.query or parsed.fragment:
                    raise ValueError(f"Asset URI must reference a local file: {uri!r} in {model}")
                dependencies.append(asset_file(library, model.parent, unquote(parsed.path)))
            for source in dependencies:
                target = destination / source.relative_to(library)
                if not target.is_file():
                    copy_path(source, target)


def copy_assets(library: Path, destination: Path, declared: set[str], requested: list[str], shared: bool) -> None:
    selected = declared | set(requested)
    if not library.is_dir():
        if selected:
            raise ValueError(f"Asset library not found: {library}")
        destination.mkdir()
        return
    if not selected:
        if shared:
            destination.mkdir()
        else:
            copy_path(library, destination)
        return
    config_path = library / "library.toml"
    config = read_toml(config_path) if config_path.is_file() else {}
    depth = max(0, min(4, config.get("scan_depth", 1)))
    ignored = [PurePosixPath(path) for path in config.get("ignore", []) if path]
    candidates = [library]
    entries: dict[str, Path] = {}
    for _ in range(depth):
        children = sorted(child for parent in candidates for child in parent.iterdir()
                          if not child.name.startswith(".") and child.is_dir()
                          and not child.is_symlink() and not child.is_junction())
        candidates = []
        for entry in children:
            relative = PurePosixPath(entry.relative_to(library).as_posix())
            if any(relative.is_relative_to(path) for path in ignored):
                continue
            manifest = entry / "asset.toml"
            if not manifest.is_file():
                candidates.append(entry)
                continue
            asset_id = read_toml(manifest).get("id", entry.name)
            if asset_id in selected:
                if asset_id in entries:
                    raise ValueError(f"Multiple manifests found for asset {asset_id!r}")
                entries[asset_id] = entry
    if missing := selected - entries.keys():
        raise ValueError(f"Assets not found in {library}: {', '.join(sorted(missing))}")
    if not shared:
        copy_path(library, destination)
        return
    for entry in entries.values():
        copy_path(entry, destination / entry.relative_to(library))
    for entry in entries.values():
        copy_asset_files(library, destination, entry)
    configured_default = config.get("default")
    default = configured_default if configured_default in selected else min(selected)
    (destination / "library.toml").write_text(
        f'version = 1\nscan_depth = {depth}\ndefault = {json.dumps(default)}\n', encoding="utf-8",
    )


def copy_project(root: Path, output: Path, extra_data: list[str], extra_assets: list[str]) -> None:
    manifest = read_toml(root / "deren.toml")
    name = manifest["project"]["name"]
    game = project_path(root, manifest["game"]["scene"], "[game] scene")
    render = project_path(root, manifest["project"]["main_scene"], "[project] main_scene")
    if not game.is_relative_to("scenes") or not game.name.endswith(".scene.toml") or not (root / game).is_file():
        raise ValueError("[game] scene must name a saved .scene.toml file in scenes/")
    config = read_toml(root / render)
    assembly = (root / manifest["scripts"]["assembly"]).resolve(strict=True)
    data_root = output / "project"
    for relative in ("scenes", "scripts", "audio", "ui"):
        if (root / relative).is_dir():
            copy_path(root / relative, data_root / relative)
    for value in extra_data:
        relative = project_path(root, value, "--data")
        if relative == Path(".") or relative.parts[0] in {".deren", "app", "asset", "deren.toml"}:
            raise ValueError(f"--data cannot copy the project root or a reserved path: {value!r}")
        copy_path(root / relative, data_root / relative)

    copy_path(root / render, data_root / render)
    # Keep the renderer document byte-for-byte. Bring along the project-relative files it names.
    for table, keys, base in (
        (config.get("paths", {}), ("shaders_dir", "model_dir"), Path(".")),
        (config.get("gui", {}), ("rml_document", "rml_font", "rml_fallback_font"), Path(".")),
        (config.get("reflection_probe", {}), ("asset",), Path(".")),
        (config.get("lighting", {}), ("profile",), render.parent),
        (config.get("toon", {}), ("profile",), render.parent),
        (config, ("model",), Path(".")),
    ):
        for key in keys:
            if value := table.get(key):
                relative = base / project_path(root, value, key)
                copy_path(root / relative, data_root / relative)
    if value := config.get("lighting", {}).get("environment_hdr"):
        relative = project_path(root, value, "lighting.environment_hdr")
        copy_path(root / relative, output / relative)
    if value := config.get("paths", {}).get("screenshot_dir"):
        project_path(root, value, "paths.screenshot_dir")

    library_setting = manifest.get("paths", {}).get("asset_library") or config.get("paths", {}).get("asset_library") or "asset"
    library = (root / library_setting).resolve()
    copy_assets(library, data_root / "asset", collect_scene_assets(root / "scenes"),
                extra_assets, shared=not library.is_relative_to(root))
    relative_assembly = Path("app") / assembly.name
    copy_path(assembly, data_root / relative_assembly)
    deps = assembly.with_suffix(".deps.json")
    if deps.is_file():
        copy_path(deps, (data_root / relative_assembly).with_suffix(".deps.json"))

    actions = manifest.get("input", {}).get("actions", {})
    lines = [
        "[project]", f"name = {json.dumps(name, ensure_ascii=False)}",
        f"main_scene = {json.dumps(render.as_posix(), ensure_ascii=False)}",
        "", "[paths]", 'asset_library = "asset"',
        "", "[scripts]", f"assembly = {json.dumps(relative_assembly.as_posix(), ensure_ascii=False)}",
        "", "[game]", f"scene = {json.dumps(game.as_posix(), ensure_ascii=False)}",
    ]
    if actions:
        lines.extend(("", "[input.actions]"))
        for action, keys in actions.items():
            lines.append(f"{json.dumps(action, ensure_ascii=False)} = {json.dumps(keys, ensure_ascii=False)}")
    (data_root / "deren.toml").write_text("\n".join(lines) + "\n", encoding="utf-8")


def export(build: Path, root: Path, destination: Path, dotnet_root: Path, data: list[str], assets: list[str]) -> None:
    if destination.exists() or destination.is_symlink():
        raise ValueError(f"Export destination already exists: {destination}")
    destination = destination.resolve()
    if destination.is_relative_to(root):
        raise ValueError("Choose an export destination outside the source project.")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".deren-export-", dir=destination.parent) as staging:
        output = Path(staging) / "package"
        output.mkdir()
        bundle_runtime(build, output, dotnet_root)
        copy_project(root, output, data, assets)
        license_file = Path(__file__).resolve().parents[2] / "LICENSE"
        copy_path(license_file, output / "LICENSE.txt")
        output.rename(destination)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--build", type=Path, default=Path("build"))
    parser.add_argument("--dotnet-root", type=Path)
    parser.add_argument("--data", action="append", default=[], help="include an additional project-relative file or directory")
    parser.add_argument("--asset", action="append", default=[], help="include an additional asset ID used dynamically by scripts")
    args = parser.parse_args()
    try:
        dotnet_root = args.dotnet_root
        if dotnet_root is None:
            dotnet = shutil.which("dotnet.exe")
            if dotnet is None:
                raise ValueError("Specify --dotnet-root pointing to a compatible .NET installation.")
            dotnet_root = Path(dotnet).resolve().parent
        export(args.build.resolve(), args.project.resolve(), args.destination.absolute(), dotnet_root.resolve(),
               args.data, args.asset)
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.error(str(error))
    print(f"Export ready: {args.destination.resolve() / 'deren.exe'}")


if __name__ == "__main__":
    main()
