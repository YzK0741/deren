"""Cook PBR colour, material and normal mip chains with Intel BC encoders."""

import argparse
import csv
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

import ispc_texcomp


def dds_header(width: int, height: int, levels: int, format_id: int = 99) -> bytes:
    header = [124, 0xA1007, height, width, ((width + 3) // 4) * ((height + 3) // 4) * 16, 0, levels]
    header += [0] * 11
    header += [32, 4, 0x30315844, 0, 0, 0, 0, 0]
    header += [0x401008 if levels > 1 else 0x1000, 0, 0, 0, 0]
    return b"DDS " + struct.pack("<31I", *header) + struct.pack("<5I", format_id, 3, 0, 1, 0)


def encode_chain(source: bytes, width: int, height: int, levels: int, format_id: int = 99) -> bytearray:
    if format_id not in (83, 98, 99):
        raise ValueError("unsupported BC texture format")
    profile = ispc_texcomp.BC7EncSettings.from_profile("basic")
    output = bytearray(dds_header(width, height, levels, format_id))
    offset = 0
    for _ in range(levels):
        size = width * height * 4
        pixels = source[offset:offset + size]
        if len(pixels) != size:
            raise ValueError("truncated source mip chain")
        # Replicate edge texels to fill partial 4x4 blocks, including 1x1 and 2x2 mips.
        padded_width, padded_height = (width + 3) // 4 * 4, (height + 3) // 4 * 4
        padded = bytearray(padded_width * padded_height * 4)
        for y in range(padded_height):
            row = pixels[min(y, height - 1) * width * 4:(min(y, height - 1) + 1) * width * 4]
            padded[y * padded_width * 4:(y + 1) * padded_width * 4] = row + row[-4:] * (padded_width - width)
        if format_id == 83:
            # The native BC5 kernel reads packed RG8, despite the wrapper's RGBASurface name.
            packed = bytearray(padded_width * padded_height * 2)
            packed[0::2], packed[1::2] = padded[0::4], padded[1::4]
            surface = ispc_texcomp.RGBASurface(bytes(packed), padded_width, padded_height, padded_width * 2)
            compressed = ispc_texcomp.compress_blocks_bc5(surface)
        else:
            surface = ispc_texcomp.RGBASurface(bytes(padded), padded_width, padded_height, padded_width * 4)
            compressed = ispc_texcomp.compress_blocks_bc7(surface, profile)
        if len(compressed) != padded_width * padded_height:
            raise ValueError("unexpected BC encoder output size")
        output.extend(compressed)
        offset += size
        width, height = max(1, width // 2), max(1, height // 2)
    if offset != len(source):
        raise ValueError("unexpected trailing source data")
    return output


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--exporter", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    entries = []
    with tempfile.TemporaryDirectory(prefix="deren-bc7-") as temporary:
        directory = Path(temporary)
        subprocess.run([str(args.exporter.resolve()), str(args.model.resolve()), str(directory)], check=True)
        with (directory / "textures.tsv").open() as file:
            textures = list(csv.DictReader(file, delimiter="\t"))
        for index, texture in enumerate(textures):
            key = texture["key"]
            width, height, levels = (int(texture[name]) for name in ("width", "height", "levels"))
            format_id = int(texture.get("format", 99))
            destination = args.output / f"{key}.dds"
            expected_size = 148 + sum(((max(1, width >> mip) + 3) // 4) *
                                      ((max(1, height >> mip) + 3) // 4) * 16 for mip in range(levels))
            cached = destination.read_bytes() if destination.is_file() else b""
            if len(cached) == expected_size and cached[:148] == dds_header(width, height, levels, format_id):
                encoded_size = len(cached) - 148
            else:
                source = (directory / f"{key}.rgba").read_bytes()
                compressed = encode_chain(source, width, height, levels, format_id)
                with tempfile.NamedTemporaryFile(dir=args.output, suffix=".tmp", delete=False) as file:
                    temporary_path = Path(file.name)
                    file.write(compressed)
                try:
                    os.replace(temporary_path, destination)
                finally:
                    temporary_path.unlink(missing_ok=True)
                encoded_size = len(compressed) - 148
            entries.append({**texture, "encoded_bytes": encoded_size})
            print(f"[{index + 1}/{len(textures)}] {key}", flush=True)
    (args.output / "manifest.json").write_text(json.dumps({"encoder": "ispc_texcomp 1.0.1 basic", "textures": entries}, indent=2))
    source_bytes = sum(int(entry["bytes"]) for entry in entries)
    encoded_bytes = sum(entry["encoded_bytes"] for entry in entries)
    print(f"{len(entries)} textures: {source_bytes} RGBA mip bytes -> {encoded_bytes} BC mip bytes")


if __name__ == "__main__":
    main()
