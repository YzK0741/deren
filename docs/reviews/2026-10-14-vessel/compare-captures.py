from pathlib import Path
import json
import numpy as np
from PIL import Image
repo = Path.cwd()
work = repo / 'build-release-dyn-clang64/shared-assets-check'
baseline = repo / 'docs/reviews/2026-10-14-shared-assets'
results = []
for after in sorted(work.glob('vessel-*')):
    scenario = after.name.removeprefix('vessel-')
    before = baseline / ('candidate-' + scenario)
    a_path = max(before.glob('screenshot_*.png'), key=lambda p: p.stat().st_mtime)
    b_path = max(after.glob('screenshot_*.png'), key=lambda p: p.stat().st_mtime)
    a = np.asarray(Image.open(a_path).convert('RGBA')).astype(np.int16)
    b = np.asarray(Image.open(b_path).convert('RGBA')).astype(np.int16)
    assert a.shape == b.shape
    d = np.abs(a-b)
    results.append(dict(scenario=scenario, different_pixels=int(np.count_nonzero(np.any(d, axis=2))), max_channel_delta=int(d.max()), before=str(a_path.relative_to(repo)), after=str(b_path.relative_to(repo))))
(repo/'docs/reviews/2026-10-14-vessel/pixel-comparison.json').write_text(json.dumps(results,indent=2))
assert len(results) == 15
assert all(r['different_pixels']==0 for r in results), results
print('PASS: 15 scene captures match archived pre-vessel ownership-migration captures pixel for pixel')
