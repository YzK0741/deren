"""Check the compiled deferred push ABI, including the host-appended indices.

The host transmits an 88-byte payload followed by frame_slot and image_index.
Checking SPIR-V offsets catches missing padding even when the shader compiles.
The matching host offsets are pinned in engine/pass/deferred.cppm.
"""
from pathlib import Path
import struct
import sys


def member_offsets(path):
    data = Path(path).read_bytes()
    if len(data) % 4 or len(data) < 20:
        raise ValueError("invalid SPIR-V size")
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] != 0x07230203:
        raise ValueError("invalid SPIR-V magic")
    names, offsets = {}, {}
    cursor = 5
    while cursor < len(words):
        count, opcode = words[cursor] >> 16, words[cursor] & 0xffff
        if count == 0 or cursor + count > len(words):
            raise ValueError("invalid SPIR-V instruction")
        operands = words[cursor + 1:cursor + count]
        if opcode == 6:  # OpMemberName: structure, member, literal string
            raw = struct.pack(f"<{len(operands) - 2}I", *operands[2:])
            names[operands[:2]] = raw.split(b"\0", 1)[0].decode("utf-8")
        elif opcode == 72 and operands[2] == 35:  # OpMemberDecorate Offset
            offsets[operands[:2]] = operands[3]
        cursor += count
    blocks = {}
    for (block, member), name in names.items():
        if (block, member) in offsets:
            blocks.setdefault(block, {})[name] = offsets[block, member]
    candidates = [members for members in blocks.values()
                  if {"inv_view_proj", "ssao", "unlit", "frame_slot", "image_index"} <= members.keys()]
    if len(candidates) != 1:
        raise ValueError(f"expected one deferred push block, got {len(candidates)}")
    return candidates[0]


def main():
    actual = member_offsets(sys.argv[1])
    expected = {"inv_view_proj": 0, "ssao": 64, "unlit": 80,
                "punctual": 84, "frame_slot": 88, "image_index": 92}
    errors = [f"{name}: expected offset {offset}, got {actual.get(name, 'missing')}"
              for name, offset in expected.items() if actual.get(name) != offset]
    if errors:
        raise SystemExit("deferred push ABI mismatch:\n" + "\n".join(errors))
    print("deferred push ABI: host payload 88 bytes, frame_slot 88, image_index 92")


if __name__ == "__main__":
    main()
