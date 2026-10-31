"""Independent check of the GPU-built HZB max pyramid and its header (M3 phase A2).

WHAT IT IS FOR. `hzb_build.comp.spv` builds the max pyramid of the frame's G-buffer depth on the GPU
(historical design record §4.1/§4.2/§7.1), and the whole design rests on that
pyramid being EXACTLY the layout the host/CPU reference describes: level 0 = the depth image verbatim, level L =
the 2x2 max of L-1 with the odd edge REPLICATED, levels packed contiguously in one buffer lane. A shader is
validated by pictures, which cannot say which of a dozen formulas is the wrong one, so this script rebuilds the
pyramid INDEPENDENTLY with numpy from the raw depth dump and compares the two word for word. That is the phase-A
correctness evidence (§10.2/§15): the GPU may differ from the reference by ZERO words, not by an epsilon.

THE INPUTS (three files, all written on the same screenshot frame by the renderer):

  <prefix>.depth.bin    - DEREN_DEPTH_PROBE_CAPTURE's file: u32 width, u32 height, then one u32 per texel (the
                          device stores D32_SFLOAT, so the u32 bits ARE a float32; 1.0 = the far plane).
                          (The parser is the one scripts/measure/cluster_occlusion_probe.py:59-63 already uses.)
  <prefix>.pyramid.bin  - DEREN_HZB_PROBE_CAPTURE's file: u32 {file magic 0x485A4250, width, height,
                          level_count}, then `pyramid_words(width, height)` float32 words, level 0 first.
  <prefix>.header.bin   - the same probe's 256-byte header lane, VERBATIM (see source/shaders/common/hzb.slang):
                          u32[0] = HZB magic (0x485A4201), u32[1..3] = width, height, level count,
                          u32[4..19] = the camera view matrix's bits, u32[20..35] = proj's,
                          u32[36..38] = camera position, u32[39] = pad, u32[40..63] = reserved zeroes.
  <prefix>.json         - the probe's metadata: the extent/level count/frame slot, and the HOST's own camera
                          snapshot for the SAME frame. "header view/proj == that camera" is the C21 invariant
                          (§4.1: the header's matrices are the ones that BUILT the pyramid, and the next
                          frame overwrites the camera UBO lane so they cannot be re-read later).

THE PASS CRITERIA (any failure exits non-zero):
  1. the depth extent, the pyramid file's extent and the metadata's extent agree, and the level count is the
     one the recurrence gives for that extent;
  2. the numpy pyramid equals the dumped one word for word (bit patterns, not `close enough`);
  3. the header's magic/extent/level count are the expected values, its reserved tail is zero, and its
     view/proj/camera_pos words are BYTE-IDENTICAL to the metadata's same-frame camera.

Usage:
  python scripts/measure/hzb_verify.py --depth D --pyramid P --header H --meta M
  python scripts/measure/hzb_verify.py --prefix build-release-dyn-clang64/bistro-probe/hzb-far \
      --depth build-release-dyn-clang64/bistro-probe/probe-depth-far.bin
  (--depth may also be derived from --prefix's sibling names; explicit paths always win.)

Exit status: 0 = PASS, 1 = a mismatch (printed), 2 = a missing/malformed input or a usage error.
"""
import argparse
import json
import sys

import numpy as np

# THE HEADER'S MIRRORED CONSTANTS (source/shaders/common/hzb.slang's hzb_magic, and the probe's own file magic
# from runtime.probes.cppm's hzb_pyramid_file_magic). They are duplicated HERE on purpose: a script that read the
# magic from the file it is checking would accept a header written by a different build of the shader.
HZB_HEADER_MAGIC = 0x485A4201
HZB_PYRAMID_FILE_MAGIC = 0x485A4250
HZB_HEADER_BYTES = 256
HZB_HEADER_WORDS = HZB_HEADER_BYTES // 4
HEADER_WORD_MAGIC = 0
HEADER_WORD_WIDTH = 1
HEADER_WORD_HEIGHT = 2
HEADER_WORD_LEVELS = 3
HEADER_WORD_VIEW = 4
HEADER_WORD_PROJ = 20
HEADER_WORD_CAMERA_POS = 36
HEADER_WORD_RESERVED_BEGIN = 40


class VerificationError(Exception):
    """A mismatch or a malformed input; main() prints it and exits non-zero."""


def level_width(width, level):
    """the ceil-halving recurrence of deren.engine.visibility.hzb::level_width, in Python"""
    result = width
    for _ in range(level):
        result = (result + 1) // 2
    return result


def level_height(height, level):
    result = height
    for _ in range(level):
        result = (result + 1) // 2
    return result


def level_count(width, height):
    if width == 0 or height == 0:
        return 0
    remaining_width, remaining_height, levels = width, height, 1
    while remaining_width > 1 or remaining_height > 1:
        remaining_width = (remaining_width + 1) // 2
        remaining_height = (remaining_height + 1) // 2
        levels += 1
    return levels


def level_offset(width, height, level):
    levels = level_count(width, height)
    return sum(level_width(width, each) * level_height(height, each) for each in range(min(level, levels)))


def pyramid_words(width, height):
    return level_offset(width, height, level_count(width, height))


def fail(message):
    raise VerificationError(message)


def read_depth_dump(path):
    """DEREN_DEPTH_PROBE_CAPTURE: u32 width, u32 height, then one u32 per texel (a float32's bits)."""
    raw = open(path, 'rb').read()
    if len(raw) < 8:
        fail(f'{path}: depth dump is shorter than its 8-byte header')
    width, height = (int(value) for value in np.frombuffer(raw[:8], dtype=np.uint32))
    expected = 8 + width * height * 4
    if width == 0 or height == 0 or len(raw) < expected:
        fail(f'{path}: depth dump claims {width}x{height} but holds {len(raw)} bytes (need {expected})')
    depth = np.frombuffer(raw[8:expected], dtype=np.uint32).view(np.float32).reshape(height, width).copy()
    return width, height, depth


def read_pyramid_dump(path):
    """DEREN_HZB_PROBE_CAPTURE's .pyramid.bin: u32 {magic, width, height, level_count}, then the lane's words."""
    raw = open(path, 'rb').read()
    if len(raw) < 16:
        fail(f'{path}: pyramid dump is shorter than its 16-byte header')
    magic, width, height, levels = (int(value) for value in np.frombuffer(raw[:16], dtype=np.uint32))
    if magic != HZB_PYRAMID_FILE_MAGIC:
        fail(f'{path}: pyramid file magic is 0x{magic:08x}, expected 0x{HZB_PYRAMID_FILE_MAGIC:08x}')
    words = np.frombuffer(raw[16:], dtype=np.uint32).copy()
    return width, height, levels, words


def build_max_pyramid(depth):
    """the reference: level 0 = depth, level L = 2x2 max of L-1, the odd edge REPLICATED.

    The edge replication is the shader's clamp (`min(2*x + 1, previous_width - 1)`), and for a MAX it is
    equivalent to padding with -inf - but padding with the EDGE is written here because that is the formula the
    two implementations promise each other, and "equivalent for max" is the kind of reasoning that stops being
    true the day the reduction changes.
    """
    levels = [depth]
    while levels[-1].shape[0] > 1 or levels[-1].shape[1] > 1:
        current = levels[-1]
        height, width = current.shape
        padded = np.pad(current, ((0, height & 1), (0, width & 1)), mode='edge')
        levels.append(padded.reshape(padded.shape[0] // 2, 2, padded.shape[1] // 2, 2).max(axis=(1, 3)))
    return levels


def describe_word_position(width, height, index):
    """the (level, x, y) of a flat lane word, for a diff report a human can act on"""
    for level in range(level_count(width, height)):
        begin = level_offset(width, height, level)
        size = level_width(width, level) * level_height(height, level)
        if begin <= index < begin + size:
            level_w = level_width(width, level)
            return f'level {level}, x={index % level_w}, y={(index % size) // level_w}'
    return f'word {index} (outside the pyramid)'


def verify(depth_path, pyramid_path, header_path, metadata_path):
    metadata = json.load(open(metadata_path, 'r'))
    extent = metadata['extent']
    meta_width, meta_height = int(extent['width']), int(extent['height'])
    meta_levels = int(metadata['level_count'])
    meta_words = int(metadata['words_per_lane'])
    meta_lane_bytes = int(metadata['levels_bytes_per_lane'])
    camera = metadata['camera']

    depth_width, depth_height, depth = read_depth_dump(depth_path)
    if (depth_width, depth_height) != (meta_width, meta_height):
        fail(f'extent mismatch: depth dump {depth_width}x{depth_height}, metadata {meta_width}x{meta_height}')
    expected_levels = level_count(depth_width, depth_height)
    expected_words = pyramid_words(depth_width, depth_height)
    if expected_levels > 16:
        fail(f'{depth_width}x{depth_height} needs {expected_levels} levels, over the build stage\'s 16-level cap')
    if meta_levels != expected_levels or meta_words != expected_words:
        fail(f'metadata says {meta_levels} level(s)/{meta_words} word(s); the extent {depth_width}x{depth_height} needs {expected_levels}/{expected_words}')
    if meta_lane_bytes != expected_words * 4:
        fail(f'metadata says {meta_lane_bytes} B per lane; {expected_words} words are {expected_words * 4} B')

    pyramid_width, pyramid_height, pyramid_levels, actual = read_pyramid_dump(pyramid_path)
    if (pyramid_width, pyramid_height, pyramid_levels) != (depth_width, depth_height, expected_levels):
        fail(f'pyramid dump is {pyramid_width}x{pyramid_height} with {pyramid_levels} level(s); the depth dump is {depth_width}x{depth_height} with {expected_levels}')
    if actual.size != expected_words:
        fail(f'pyramid dump holds {actual.size} word(s); {depth_width}x{depth_height} needs {expected_words}')

    # ---- the pyramid, word for word ----
    reference = np.concatenate([level.reshape(-1).view(np.uint32) for level in build_max_pyramid(depth)])
    if reference.size != actual.size:
        fail(f'numpy rebuilt {reference.size} words; the dump holds {actual.size}')  # the recurrence and the shader disagree about the layout
    differing = np.nonzero(reference != actual)[0]
    if differing.size != 0:
        first = int(differing[0])
        expected_value = np.float32(reference[first:first + 1].view(np.float32)[0])
        actual_value = np.float32(actual[first:first + 1].view(np.float32)[0])
        fail(f'pyramid mismatch: {differing.size} of {reference.size} word(s) differ; first at {describe_word_position(depth_width, depth_height, first)} '
             f'(reference {expected_value!r} bits 0x{int(reference[first]):08x}, GPU {actual_value!r} bits 0x{int(actual[first]):08x})')

    # ---- the header ----
    header_raw = open(header_path, 'rb').read()
    if len(header_raw) < HZB_HEADER_BYTES:
        fail(f'{header_path}: header is {len(header_raw)} B, expected {HZB_HEADER_BYTES}')
    header = np.frombuffer(header_raw[:HZB_HEADER_BYTES], dtype=np.uint32)

    def expect_word(index, value, what):
        if int(header[index]) != int(value):
            fail(f'header {what} is {int(header[index])} (0x{int(header[index]):08x}), expected {int(value)} (0x{int(value):08x})')

    expect_word(HEADER_WORD_MAGIC, HZB_HEADER_MAGIC, 'magic')
    if int(header[HEADER_WORD_MAGIC]) != int(metadata.get('header_magic', HZB_HEADER_MAGIC)):
        fail(f'header magic disagrees with the metadata\'s header_magic ({int(metadata.get("header_magic"))})')
    expect_word(HEADER_WORD_WIDTH, depth_width, 'width')
    expect_word(HEADER_WORD_HEIGHT, depth_height, 'height')
    expect_word(HEADER_WORD_LEVELS, expected_levels, 'level count')
    reserved = header[HEADER_WORD_RESERVED_BEGIN:HZB_HEADER_WORDS]
    if np.any(reserved != 0):
        fail(f'header reserved words {HEADER_WORD_RESERVED_BEGIN}..{HZB_HEADER_WORDS - 1} are not zero: {[int(v) for v in reserved]}')

    # THE C21 CHECK: the header's matrices are a GPU snapshot of the camera UBO lane the build read, and the
    # metadata's camera is the HOST's snapshot of the same frame. Byte equality (bit patterns) is the criterion,
    # not closeness: the design says "the matrices that built this pyramid", and anything but identical bits is a
    # different matrix.
    def expected_bits(values):
        return np.asarray(values, dtype=np.float32).view(np.uint32)

    for name, begin, count in (('view', HEADER_WORD_VIEW, 16), ('proj', HEADER_WORD_PROJ, 16)):
        if name not in camera:
            fail(f'metadata has no camera.{name}')
        expected = expected_bits(camera[name])
        if expected.size != count:
            fail(f'metadata camera.{name} has {expected.size} floats, expected {count}')
        actual_bits = header[begin:begin + count]
        if np.any(expected != actual_bits):
            first = int(np.nonzero(expected != actual_bits)[0][0])
            fail(f'header {name}[{first}] is {np.float32(actual_bits[first:first + 1].view(np.float32)[0])!r} '
                 f'(bits 0x{int(actual_bits[first]):08x}), but the same frame\'s camera says that element is '
                 f'{np.float32(expected[first:first + 1].view(np.float32)[0])!r} (bits 0x{int(expected[first]):08x})')
    position = camera.get('position', [])
    if len(position) != 3:
        fail(f'metadata camera.position has {len(position)} floats, expected 3')
    actual_position = header[HEADER_WORD_CAMERA_POS:HEADER_WORD_CAMERA_POS + 3]
    expected_position = expected_bits(position)
    if np.any(expected_position != actual_position):
        fail(f'header camera_pos bits differ from the metadata: header {[hex(int(v)) for v in actual_position]}, metadata {[hex(int(v)) for v in expected_position]}')

    return {
        'extent': f'{depth_width}x{depth_height}',
        'levels': expected_levels,
        'words': expected_words,
        'lane_bytes': expected_words * 4,
        'frame_slot': int(metadata.get('frame_slot', -1)),
    }


def main():
    parser = argparse.ArgumentParser(description='Verify the GPU-built HZB pyramid and header against an independent numpy rebuild.')
    parser.add_argument('--depth', help='DEREN_DEPTH_PROBE_CAPTURE output (the same frame)')
    parser.add_argument('--pyramid', help='DEREN_HZB_PROBE_CAPTURE output, .pyramid.bin')
    parser.add_argument('--header', help='DEREN_HZB_PROBE_CAPTURE output, .header.bin')
    parser.add_argument('--meta', help='DEREN_HZB_PROBE_CAPTURE output, .json')
    parser.add_argument('--prefix', help='the DEREN_HZB_PROBE_CAPTURE prefix, if the three HZB paths are not given')
    args = parser.parse_args()

    prefix = args.prefix
    if prefix is not None:
        args.pyramid = args.pyramid or (prefix + '.pyramid.bin')
        args.header = args.header or (prefix + '.header.bin')
        args.meta = args.meta or (prefix + '.json')
    if not (args.depth and args.pyramid and args.header and args.meta):
        parser.error('need --depth plus (--prefix or --pyramid/--header/--meta)')

    try:
        result = verify(args.depth, args.pyramid, args.header, args.meta)
    except VerificationError as error:
        print(f'FAIL hzb_verify: {error}')
        return 1
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f'FAIL hzb_verify: malformed or missing input: {error}')
        return 2
    print(f'PASS hzb_verify: pyramid 0 word difference of {result["words"]} word(s) '
          f'({result["lane_bytes"]} B per lane, {result["levels"]} level(s), {result["extent"]}, frame slot {result["frame_slot"]}); '
          f'header magic/extent/levels/reserved OK; header view/proj/camera_pos BYTE-IDENTICAL to the same frame\'s camera')
    return 0


if __name__ == '__main__':
    sys.exit(main())
