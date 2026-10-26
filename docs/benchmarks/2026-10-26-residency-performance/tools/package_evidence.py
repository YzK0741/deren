import json
from pathlib import Path
import shutil
import zipfile

root=Path(__file__).resolve().parents[4]
base=root/'tmp/residency-review-pinned-20261026'
stress=root/'tmp/residency-review-pinned-stress-20261026'
out=root/'docs/benchmarks/2026-10-26-residency-performance'
audit=json.loads((base/'audit.json').read_text())
print('Pixel checks:',[(Path(p['eager']).parent.name,p['rgb_identical'],p['changed_pixels'],p['max_channel_error']) for p in audit['pixel_pairs']])
for source,destination in ((base/'audit.json','audit.json'),(base/'manifest.json','baseline-manifest.json'),
    (stress/'manifest.json','stress-manifest.json'),(base/'artifact-identity.json','artifact-identity.json'),
    (base/'results.json','baseline-results.json'),(stress/'results.json','stress-results.json')):
    shutil.copy2(source,out/destination)
for p in (root/'tmp/residency-review-tools').glob('*.py'):
    (out/'tools'/p.name).write_text(p.read_text().replace('parents[4]','parents[4]'))
with zipfile.ZipFile(out/'measurement-traces.zip','w',compression=zipfile.ZIP_DEFLATED) as archive:
    for folder in (base,stress):
        for pattern in ('**/frames.csv','**/memory.jsonl','**/config.toml','**/summary.json'):
            for p in folder.glob(pattern):
                archive.write(p,p.relative_to(root/'tmp'))
print('Evidence saved.')
