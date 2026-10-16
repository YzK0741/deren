from pathlib import Path
import hashlib, json, shutil, struct, zlib
import numpy as np
from PIL import Image

repo = next(p for p in Path(__file__).resolve().parents if (p/'CMakeLists.txt').exists())
work = repo/'build-release-dyn-clang64/gaussian-cpu-managed-check'
archive = Path(__file__).resolve().parent
results = []
for scenario in ('static', 'rotation'):
    for mode in ('cpu', 'gpu'):
        before = work/('baseline-'+scenario)/mode
        after = work/('final-'+scenario)/mode
        a = np.asarray(Image.open(next(before.glob('screenshot_*.png'))).convert('RGBA'))
        b = np.asarray(Image.open(next(after.glob('screenshot_*.png'))).convert('RGBA'))
        assert a.shape == b.shape
        different = int(np.count_nonzero(np.any(a != b, axis=2)))
        hdr_equal = (before/'capture.rgba16f').read_bytes() == (after/'capture.rgba16f').read_bytes()
        clean = all(not any(token in (p/'debug.log').read_text(errors='replace') for token in ('VUID-', 'Validation Error', 'SYNC-HAZARD', '[ERROR]')) for p in (before, after))
        result = dict(scenario=scenario, mode=mode, different_pixels=different, hdr_identical=hdr_equal, sync_clean=clean)
        results.append(result)
        assert different == 0 and hdr_equal and clean, result
        for source in (before, after):
            target = archive/source.parent.name/mode
            target.mkdir(parents=True, exist_ok=True)
            for file in source.iterdir():
                if file.suffix in ('.png', '.toml', '.log'):
                    shutil.copy2(file, target/file.name)
for file in (work/'final-resize').iterdir():
    if file.suffix in ('.png', '.toml', '.log'):
        target = archive/'final-resize'
        target.mkdir(exist_ok=True)
        shutil.copy2(file, target/file.name)
shutil.copy2(work/'check-captures.ps1', archive/'check-captures.ps1')
shutil.copy2(repo/'scripts/windows/check_gaussian_resize.ps1', archive/'check-resize.ps1')
for file in archive.rglob('*.png'):
    data, offset, chunks = file.read_bytes(), 8, []
    while offset < len(data):
        size = struct.unpack('>I', data[offset:offset+4])[0]
        chunks.append((data[offset+4:offset+8], data[offset+8:offset+8+size]))
        offset += size+12
    raw = zlib.decompress(b''.join(body for tag, body in chunks if tag == b'IDAT'))
    encoded, written, out = zlib.compress(raw, 9), False, data[:8]
    assert zlib.decompress(encoded) == raw
    for tag, body in chunks:
        if tag == b'IDAT':
            if written: continue
            body, written = encoded, True
        out += struct.pack('>I', len(body))+tag+body+struct.pack('>I', zlib.crc32(tag+body))
    file.write_bytes(out)
(archive/'pixel-comparison.json').write_text(json.dumps(results, indent=2)+'\n')
digests = {}
for root in (work/'runtime.before', repo/'build-release-dyn-clang64'):
    for file in [root/'deren.exe', *root.glob('*.dll'), *(root/'shaders').rglob('*')]:
        if file.is_file(): digests[str(file.relative_to(repo))] = hashlib.sha256(file.read_bytes()).hexdigest()
for file in [*archive.rglob('*'), *work.glob('baseline-*/*/capture.rgba16f'), *work.glob('final-*/*/capture.rgba16f')]:
    if file.is_file() and file.name != 'sha256.json':
        digests[str(file.relative_to(repo))] = hashlib.sha256(file.read_bytes()).hexdigest()
(archive/'sha256.json').write_text(json.dumps(digests, indent=2)+'\n')
print(json.dumps(results, indent=2))

