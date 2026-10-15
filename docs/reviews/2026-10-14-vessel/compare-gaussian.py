from pathlib import Path
import gzip,json
import numpy as np
from PIL import Image
repo=Path.cwd()
results=[]
for mode in ('cpu','gpu'):
    before=repo/'docs/reviews/2026-10-14-shared-assets/gaussian-after'/mode
    after=repo/'build-release-dyn-clang64/vessel-gaussian-check'/mode
    a_path=next(before.glob('screenshot_*.png'))
    b_path=max(after.glob('screenshot_*.png'),key=lambda p:p.stat().st_mtime)
    a=np.asarray(Image.open(a_path).convert('RGBA'))
    b=np.asarray(Image.open(b_path).convert('RGBA'))
    pixels=int(np.count_nonzero(np.any(a!=b,axis=2)))
    hdr_equal=gzip.decompress((before/'capture.rgba16f.gz').read_bytes())==(after/'capture.rgba16f').read_bytes()
    results.append(dict(mode=mode,different_pixels=pixels,hdr_bytes_identical=hdr_equal))
(repo/'docs/reviews/2026-10-14-vessel/gaussian-comparison.json').write_text(json.dumps(results,indent=2))
assert all(r['different_pixels']==0 and r['hdr_bytes_identical'] for r in results),results
print('PASS: CPU and GPU flower captures separately match their pre-vessel RGBA pixels and HDR bytes')
