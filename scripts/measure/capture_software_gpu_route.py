"""Reproducible capture + verification for the M4 GPU survivor-routing acceptance (opt-in prototype).

One invocation renders one fixture with software raster ON and enables exactly one routing configuration:

  gpu              triangle producer + HZB build/cull/recovery + `DEREN_SOFTWARE_RASTER_GPU=1`
                   (GPU classification/reservation/emission inside HZB compaction; software consumes the
                   compute-indirect arguments). Capture-only partition validation + CPU reference dumps are
                   enabled with `DEREN_SOFTWARE_IMAGES_CAPTURE=<fresh prefix>`.
  cpu              triangle producer only, HZB off: the CPU routing control (also the software producer used by
                   the older raw-image fixtures).
  fallback         GPU requested but the history HZB gate is CLOSED on the screenshot frame (few frames): the
                   runtime must keep the valid CPU routing, never a half-prepared GPU mode.

The harness owns the config, the environment, the expected log markers and the verifier calls, so an acceptance
capture is one command and its evidence is one JSON row. It never touches the production default path: software
raster stays OFF unless this script asks for it, exactly like every other opt-in experiment here.

Examples (one per acceptance item):
    python scripts/measure/capture_software_gpu_route.py --work tmp/acc-slot0 --asset tmp/sw-tie.glb \
        --mode gpu --frames 8  --prefix slot0 --min-covered 100000
    python scripts/measure/capture_software_gpu_route.py --work tmp/acc-recovery --asset tmp/sw-tie.glb \
        --mode gpu --frames 16 --sweep 0.5 --prefix recovery --expect-recovery
    python scripts/measure/capture_software_gpu_route.py --work tmp/acc-budget0 --asset tmp/sw-tie.glb \
        --mode gpu --frames 8 --budget 0 --prefix budget0 --expect-budget
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def shader_bundle_sha256(directory: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(directory.rglob('*.spv')):
        digest.update(path.relative_to(directory).as_posix().encode('utf-8'))
        digest.update(b'\0')
        digest.update(path.read_bytes())
    return digest.hexdigest()

# The same two acceptance cameras the measurement matrices use (yaw,pitch,distance,target.x,target.y,target.z).
CAMERAS = {
    'far': '35,18,140,0,-83.07,0',
    'near': '35,8,42,0,-88,0',
    'fixture': '0,0,4,0,0,0',
}
VALIDATION_DIRTY = re.compile(r'VUID-|Validation Error|\[ERROR\]|\[WARNING\]|panic|recorded out of order')


def make_config(path: Path, asset: Path, exe: Path, screenshot_dir: Path, validation: bool, threshold: float) -> None:
    shaders = (exe.parent / 'shaders').as_posix()
    path.write_text(f'''model = "{asset.as_posix()}"
[paths]
shaders_dir = "{shaders}"
screenshot_dir = "{screenshot_dir.as_posix()}"
[render]
window_width = 1280
window_height = 720
vsync = false
max_fps = 0
validation_layers = {str(validation).lower()}
main_render_graph = true
visibility_buffer = true
subtilis = true
subtilis_error_px = 1.0
subtilis_hysteresis = false
software_raster = true
software_raster_threshold_px = {threshold}
character_forward = false
shadow = false
taa = false
ssao = false
bloom = false
fxaa = false
gpu_timings = true
[gui]
show = false
[lighting]
env_size = 128
env_mip_count = 4
irr_size = 16
lut_size = 128
''', encoding='utf-8')


def environment(mode: str, budget: int | None, capture_prefix: Path, force_oracle_failure: bool = False) -> dict:
    env = os.environ.copy()
    for key in list(env):
        if key.startswith('DEREN_'):
            env.pop(key, None)
    if os.name == 'nt':
        env['PATH'] = 'C:/msys64/clang64/bin;' + env.get('PATH', '')
    env['DEREN_SOFTWARE_RASTER_TRIANGLES'] = '1'
    if mode in ('gpu', 'fallback'):
        env['DEREN_SOFTWARE_RASTER_GPU'] = '1'
    if mode != 'cpu':
        env.update(DEREN_HZB_BUILD='1', DEREN_HZB_CULL='1', DEREN_HZB_RECOVERY='1')
    if budget is not None:
        env['DEREN_SOFTWARE_GPU_BUDGET'] = str(budget)
    if force_oracle_failure:
        env['DEREN_SOFTWARE_ORACLE_FAIL'] = '1'
    env['DEREN_SOFTWARE_IMAGES_CAPTURE'] = str(capture_prefix)
    return env


def check_markers(mode: str, text: str, route: dict | None, require_fallback: bool, require_recovery: bool,
                  require_budget: bool, budget: int | None, min_covered: int, covered: int,
                  capturing: bool = True, allow_empty: bool = False) -> list[str]:
    """The acceptance assertions for one capture. Pure data in/out so --self-test can drive it without a GPU."""
    failures = []
    gpu_active = 'software GPU routing active: HZB compaction, bounded software list' in text
    if mode == 'gpu':
        if not gpu_active:
            failures.append('GPU routing was requested but the HZB compaction lane did not log it')
        if capturing and 'software GPU capture: partition valid' not in text:
            failures.append('the capture-only partition validation did not pass')
    if mode == 'cpu':
        if gpu_active:
            failures.append('GPU routing ran without DEREN_SOFTWARE_RASTER_GPU')
        if 'software raster active: threshold' not in text:
            failures.append('the CPU software producer never activated')
    if mode == 'fallback' or require_fallback:
        if gpu_active:
            failures.append('GPU routing activated on a frame whose history gate is closed')
        if 'the prediction gate is CLOSED' not in text:
            failures.append('the fallback capture did not land on a closed-history frame')
        if 'software raster active: threshold' not in text:
            failures.append('the fallback did not record the valid CPU routing')
    empty_partition = allow_empty and route is not None and route.get('software_candidates', 1) == 0
    if capturing and not empty_partition and mode in ('gpu', 'cpu', 'fallback') and covered < min_covered:
        failures.append(f'only {covered} covered software pixels, expected at least {min_covered}')
    if require_recovery:
        if route is None:
            failures.append('the recovery capture produced no partition report')
        else:
            if route.get('recovered_visible', 0) <= 0:
                failures.append('the recovery capture recorded no recovered visible candidate')
            if route.get('hardware_primary', 0) <= 0:
                failures.append('the recovery capture recorded no primary hardware survivors')
    if require_budget:
        if route is None:
            failures.append('the budget capture produced no partition report')
        elif budget is not None and route.get('software_candidates', -1) > budget:
            failures.append(f'{route.get("software_candidates")} software candidates exceed the budget {budget}')
    if route is not None and route.get('partition_valid') is not True:
        failures.append('the partition report does not declare a valid partition')
    return failures


def run(args: argparse.Namespace) -> dict:
    work = args.work.resolve()
    exe = args.exe.resolve()
    asset = args.asset.resolve()
    work.mkdir(parents=True, exist_ok=False)
    run_dir = work / args.prefix
    run_dir.mkdir()
    prefix = run_dir / 'software'
    config = run_dir / 'config.toml'
    make_config(config, asset, exe, run_dir, args.validation, args.threshold)
    command = [str(exe), '--config', str(config), '--capture-frames', str(args.frames), f'--capture-camera={CAMERAS[args.camera]}']
    if args.sweep != 0.0:
        command.append(f'--capture-sweep={args.sweep}')
    env = environment(args.mode, args.budget, prefix, args.expect_capture_failure)
    if args.no_capture:
        env.pop('DEREN_SOFTWARE_IMAGES_CAPTURE', None)
    started = time.perf_counter()
    completed = subprocess.run(command, cwd=run_dir, env=env, stdout=(run_dir / 'stdout.log').open('wb'),
                               stderr=subprocess.STDOUT, timeout=900, check=False,
                               creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    elapsed = time.perf_counter() - started
    log_path = run_dir / 'debug.log'
    text = log_path.read_text(encoding='utf-8', errors='replace') if log_path.exists() else ''
    route_path = Path(str(prefix) + '-route.json')
    route = json.loads(route_path.read_text(encoding='utf-8')) if route_path.exists() else None
    row = {'prefix': str(prefix), 'mode': args.mode, 'camera': args.camera, 'frames': args.frames, 'sweep': args.sweep,
           'budget': args.budget, 'validation': args.validation, 'exit_code': completed.returncode,
           'process_wall_seconds': elapsed, 'log': str(log_path), 'route': route,
           'executable_sha256': sha256(exe), 'shader_bundle_sha256': shader_bundle_sha256(exe.parent / 'shaders'),
           'validation_clean': not VALIDATION_DIRTY.search(text)}
    failures = []
    if completed.returncode != 0:
        failures.append(f'exit code {completed.returncode}')
    if args.validation and not row['validation_clean']:
        failures.append('validation/log findings in the capture log')
    covered = 0
    if args.partition_only:
        # PARTITION EVIDENCE ONLY: the report is validated before the CPU raster runs, so a scene with incomplete
        # retained geometry still proves the HW/SW partition; the image dumps and the manifest must NOT appear.
        if 'software GPU capture: partition valid' not in text:
            failures.append('the partition validation did not pass')
        if 'screenshot failed: software GPU capture: invalid partition or reference' not in text:
            failures.append('the capture did not report the expected CPU-reference limitation')
        if route is None or route.get('partition_valid') is not True:
            failures.append('no validated partition report')
        for suffix in ('-depth.u32', '-id.u32', '-cpu-depth.u32', '-cpu-id.u32', '-capture.json'):
            if Path(str(prefix) + suffix).exists():
                failures.append(f'partition-only capture published {suffix}')
    elif args.expect_capture_failure:
        # NEGATIVE CONTROL (closeout hardening): a capture whose independent CPU oracle did not rasterize must
        # publish NO GPU images at all - neither a raw dump nor a completeness manifest - and say why.
        if 'screenshot failed: software image capture: CPU oracle failed; GPU images not published' not in text:
            failures.append('the capture did not report the oracle-gated screenshot failure')
        for suffix in ('-depth.u32', '-id.u32', '-cpu-depth.u32', '-cpu-id.u32', '-capture.json', '-route.json'):
            if Path(str(prefix) + suffix).exists():
                failures.append(f'oracle-failed capture published {suffix}')
        screenshots = list(run_dir.glob('screenshot_*.png'))
        if screenshots:
            failures.append('oracle-failed capture wrote a screenshot PNG')
    else:
        screenshots = list(run_dir.glob('screenshot_*.png'))
        if len(screenshots) != 1:
            failures.append(f'{len(screenshots)} screenshots (expected one)')
        if not failures and not args.no_capture:
            sys.path.insert(0, str(Path(__file__).resolve().parent))
            import verify_software_images
            empty_partition = args.allow_empty and route is not None and route.get('software_candidates', 1) == 0
            if empty_partition:
                row['empty_partition'] = True  # the forced-budget capture's evidence is the partition report
            else:
                try:
                    covered, difference, hashes = verify_software_images.verify(prefix, args.verify_pixels, args.max_depth_ulp, require_manifest=True)
                    row.update(covered_pixels=covered, max_depth_ulp=difference, image_sha256=hashes)
                except ValueError as error:
                    failures.append(f'image verification: {error}')
    failures.extend(check_markers(args.mode, text, route, args.require_fallback, args.expect_recovery,
                                  args.expect_budget, args.budget, 0 if args.expect_capture_failure else args.min_covered,
                                  covered, not args.no_capture, args.allow_empty))
    row['failures'] = failures
    row['passed'] = not failures
    (run_dir / 'capture.json').write_text(json.dumps(row, indent=2) + '\n', encoding='utf-8')
    return row


def self_test() -> None:
    good = 'software GPU routing active: HZB compaction, bounded software list\nsoftware GPU capture: partition valid, 4 software candidates, slot 0\n'
    route = {'partition_valid': True, 'slot': 0, 'software_candidates': 4, 'budget': 65536,
             'hardware_primary': 7, 'hardware_recovery': 0, 'recovered_visible': 0}
    assert not check_markers('gpu', good, route, False, False, False, None, 0, 10)
    assert check_markers('gpu', 'no routing here\n', route, False, False, False, None, 0, 10)
    assert check_markers('gpu', good, route, False, False, False, None, 100, 10)
    assert not check_markers('cpu', 'software raster active: threshold 4096 px, 4 software cluster(s)\n', None, False, False, False, None, 0, 10)
    assert check_markers('cpu', good, None, False, False, False, None, 0, 10)
    fallback = 'hzb cull: the prediction gate is CLOSED this frame\nsoftware raster active: threshold 4096 px, 4 software cluster(s))\n'
    assert not check_markers('fallback', fallback, None, True, False, False, None, 0, 10)
    assert check_markers('fallback', good + fallback, None, True, False, False, None, 0, 10)
    recovery = dict(route, recovered_visible=3, hardware_recovery=2)
    assert not check_markers('gpu', good, recovery, False, True, False, None, 0, 10)
    assert check_markers('gpu', good, route, False, True, False, None, 0, 10)
    assert not check_markers('gpu', good, dict(route, software_candidates=1), False, False, True, 1, 0, 10)
    assert check_markers('gpu', good, route, False, False, True, 1, 0, 10)
    dirty = good + 'VUID-vkCmdDraw-None-08609\n'
    assert VALIDATION_DIRTY.search(dirty)
    # The smoke mode runs no diagnostic capture, so the partition log is not expected there.
    assert not check_markers('gpu', 'software GPU routing active: HZB compaction, bounded software list\n', None,
                             False, False, False, None, 0, 0, capturing=False)
    # The forced-budget capture may report an empty software partition and must not require covered pixels.
    empty = dict(route, software_candidates=0)
    assert not check_markers('gpu', good, empty, False, False, True, 0, 100, 0, allow_empty=True)
    assert check_markers('gpu', good, empty, False, False, True, 0, 100, 0)
    # The oracle-gated negative control's marker.
    gated = 'screenshot failed: software image capture: CPU oracle failed; GPU images not published\n'
    assert 'CPU oracle failed; GPU images not published' in gated
    print('PASS: GPU-route capture harness marker/route/budget/fallback/recovery/smoke/gating controls')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--work', type=Path, required=False)
    parser.add_argument('--asset', type=Path, required=False)
    parser.add_argument('--exe', type=Path, default=Path('build-release-dyn-clang64/deren.exe'))
    parser.add_argument('--mode', choices=('gpu', 'cpu', 'fallback'), default='gpu')
    parser.add_argument('--camera', choices=tuple(CAMERAS), default='fixture')
    parser.add_argument('--frames', type=int, default=8)
    parser.add_argument('--sweep', type=float, default=0.0)
    parser.add_argument('--budget', type=int, default=None)
    parser.add_argument('--prefix', default='capture')
    parser.add_argument('--validation', action='store_true')
    parser.add_argument('--expect-recovery', action='store_true')
    parser.add_argument('--expect-budget', action='store_true')
    parser.add_argument('--require-fallback', action='store_true')
    parser.add_argument('--no-capture', action='store_true',
                        help='validation-enabled all-on smoke: do not arm the diagnostic image capture')
    parser.add_argument('--allow-empty', action='store_true',
                        help='a forced-budget capture may legitimately contain zero software candidates')
    parser.add_argument('--expect-capture-failure', action='store_true',
                        help='negative control: the CPU oracle must fail and NO GPU image may be published')
    parser.add_argument('--partition-only', action='store_true',
                        help='accept a partition report without the CPU image oracle (incomplete retained geometry)')
    parser.add_argument('--min-covered', type=int, default=1)
    parser.add_argument('--verify-pixels', type=int, default=1280 * 720)
    parser.add_argument('--max-depth-ulp', type=int, default=8)
    parser.add_argument('--threshold', type=float, default=4096.0)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.work is None or args.asset is None:
        parser.error('--work and --asset are required unless --self-test is used')
    for name, path in (('executable', args.exe), ('asset', args.asset)):
        if not path.is_file():
            parser.error(f'{name} does not exist: {path}')
    if args.frames < 1:
        parser.error('frames must be at least 1')
    if args.budget is not None and not 0 <= args.budget <= 65536:
        parser.error('budget must be within [0, 65536]')
    if args.no_capture and (args.expect_recovery or args.expect_budget or args.expect_capture_failure):
        parser.error('--no-capture cannot be combined with capture-evidence expectations')
    if args.partition_only and args.expect_capture_failure:
        parser.error('--partition-only and --expect-capture-failure are different expectations')
    row = run(args)
    print(json.dumps(row, indent=2))
    return 0 if row['passed'] else 1


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f'software GPU route capture: FAIL {error}', file=sys.stderr)
        raise SystemExit(1)
