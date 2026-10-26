"""Validate six residency captures and compare decoded pixels (requires Pillow)."""
import argparse
import json
from pathlib import Path
from PIL import Image, ImageChops, ImageStat


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    rows = json.loads((args.directory / 'results.json').read_text())
    expected = {(camera, enabled) for camera in ('far', 'near', 'sweep') for enabled in (False, True)}
    if len(rows) != 6 or {(r['camera'], r['selective']) for r in rows} != expected:
        raise ValueError('incomplete or duplicate capture matrix')
    results = {}
    for camera in ('far', 'near', 'sweep'):
        images = []
        for kind in ('eager', 'selective'):
            captures = list((args.directory / f'{camera}-{kind}').glob('screenshot_*.png'))
            if len(captures) != 1: raise ValueError('missing or ambiguous screenshot')
            images.append(Image.open(captures[0]).convert('RGB'))
        diff = ImageChops.difference(*images)
        sample = next(r for r in rows if r['camera'] == camera and r['selective'])
        states = sample['residency_samples']
        if not sample['validation_clean'] or not states: raise ValueError('unclean or absent residency evidence')
        last = states[-1]
        results[camera] = dict(rgb_identical=diff.getbbox() is None, mean_rgb_difference=ImageStat.Stat(diff).mean,
            resident_payload_bytes=last['resident_bytes'], total_payload_bytes=last['total_bytes'],
            pinned_payload_bytes=last['pinned_bytes'], reduction_percent=100 * (1-last['resident_bytes']/last['total_bytes']))
    (args.directory / 'analysis.json').write_text(json.dumps(results, indent=2))
    print(json.dumps(results, indent=2))
    if not all(r['rgb_identical'] for r in results.values()): raise ValueError('eager/selective pixel mismatch')


if __name__ == '__main__': main()
