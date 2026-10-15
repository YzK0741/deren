from pathlib import Path
import hashlib, json, shutil, struct, zlib
from PIL import Image
import numpy as np

repo = next(parent for parent in Path(__file__).resolve().parents if (parent/'CMakeLists.txt').exists())
work = repo/'build-release-dyn-clang64/tlas-managed-check'
archive = repo / 'docs/reviews/2026-10-14-tlas-resources'
archive.mkdir(exist_ok=True)
stats = []
def compress_png(path):
    data = path.read_bytes()
    chunks = []
    offset = 8
    while offset < len(data):
        size = struct.unpack('>I', data[offset:offset+4])[0]
        tag = data[offset+4:offset+8]
        chunks.append((tag, data[offset+8:offset+8+size]))
        offset += size+12
    original = zlib.decompress(b''.join(body for tag, body in chunks if tag == b'IDAT'))
    encoded = zlib.compress(original, 9)
    assert zlib.decompress(encoded) == original
    out = data[:8]
    inserted = False
    for tag, body in chunks:
        if tag == b'IDAT':
            if inserted: continue
            body = encoded
            inserted = True
        out += struct.pack('>I',len(body))+tag+body+struct.pack('>I',zlib.crc32(tag+body))
    path.write_bytes(out)

for final in sorted(work.glob('candidate-*')):
    if not final.is_dir(): continue
    scenario = final.name.removeprefix('candidate-')
    before = work / ('baseline-'+scenario)
    a = np.asarray(Image.open(next(before.glob('screenshot_*.png'))).convert('RGBA')).astype(np.int16)
    b = np.asarray(Image.open(next(final.glob('screenshot_*.png'))).convert('RGBA')).astype(np.int16)
    delta = np.abs(a-b)
    stats.append(dict(baseline_sync_clean='[ERROR]' not in (before/'debug.log').read_text(errors='replace'), candidate_sync_clean='[ERROR]' not in (final/'debug.log').read_text(errors='replace'), scenario=scenario, shape=list(a.shape), different_pixels=int(np.count_nonzero(np.any(delta,axis=2))), max_channel_delta=int(delta.max()), rmse=float(np.sqrt(np.mean(delta.astype(float)**2)))))
    for source in (before,final):
        target = archive/source.name
        target.mkdir(exist_ok=True)
        for file in source.iterdir():
            if file.suffix not in ('.png','.toml','.log'): continue
            shutil.copy2(file,target/file.name)
            if file.suffix == '.png': compress_png(target/file.name)
for name in ('pbr-resize',):
    target = archive/name
    target.mkdir(exist_ok=True)
    for file in (work/name).iterdir():
        if file.suffix not in ('.png','.toml','.log'): continue
        shutil.copy2(file,target/file.name)
        if file.suffix == '.png': compress_png(target/file.name)
for source in (work/'run-captures.ps1', repo/'build-release-dyn-clang64/ray-resources-check/pbr-resize.ps1', Path(__file__)):
    if source.resolve() != (archive/source.name).resolve():
        shutil.copy2(source,archive/source.name)
(archive/'pixel-comparison.json').write_text(json.dumps(stats,indent=2)+'\n')
digests = {}
for root in (work/'runtime.before',repo/'build-release-dyn-clang64'):
    for file in [root/'deren.exe',*sorted(root.glob('*.dll')),*sorted((root/'shaders').rglob('*'))]:
        if file.is_file(): digests[str(file.relative_to(repo))]=hashlib.sha256(file.read_bytes()).hexdigest()
for file in archive.rglob('*'):
    if file.is_file() and file.name != 'sha256.json': digests[str(file.relative_to(repo))]=hashlib.sha256(file.read_bytes()).hexdigest()
(archive/'sha256.json').write_text(json.dumps(digests,indent=2)+'\n')
print(json.dumps(stats,indent=2))


