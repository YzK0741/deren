# Subtilis converter Task 8 evidence archive

Heavy Task-8 measurement artifacts and standard-GLB previews for the offline converter
(`tools/subtilis`). The repository keeps only the small result JSONs, the scripts and the
evidence report (`docs/reviews/2026-10-21-subtilis-converter.md`); everything here is the
out-of-tree evidence that report links to.

## Provenance

- Source checkout: deren master `071cfdd5` (Task 7 committed) plus the uncommitted Task 8
  worktree; the converter binary hash used for the matrix is recorded in
  `results/measurements.json` (`converter.sha256`).
- Khronos glTF-Validator: official release `2.0.0-dev.3.10`, win64. Archive SHA-256
  `c5068f51205deedc28acc3529ee7e11ee60e853454f673093398eba80142202c`, executable SHA-256
  `4388a152ff90b68c6430ae03862e05e257a9d50a500ed7d0eb1cd420dc75ff96`; see the repository
  manifest `scripts/measure/khronos_validator_manifest.json`. Local tool path:
  `D:/deren-assets/gltf-tools-khronos/2.0.0-dev.3.10/gltf_validator.exe`.
- Real asset: `D:/deren-assets/bistro/bistro-merged.gltf` (material-merged, geometry-only
  Bistro derived from the pristine upstream manifest in `results/real-asset.json`). The
  plan-specified `bistro-geometry.gltf` result is in `results/specified-input.json`: its 551
  primitives all carry the unmodeled TANGENT attribute, so the converter profile
  (POSITION/NORMAL/TEXCOORD_0) skips them all and the build exits 4; that is recorded, not
  silently replaced.
- Host: Windows, MSYS2 clang64, Python 3.14 + NumPy for the measurement/verify scripts.
  Note: in this environment a native process cannot create staging files on `D:` (the same
  cross-volume probe limitation the Task 6 checkpoint recorded), so runs wrote to the
  workspace scratch directory and the artifacts were copied here afterwards; the archived
  SHA-256 values are of the final files.

## Contents

- `fixtures/` - deterministic generated measurement inputs (small 32x32 grid = 2,048
  triangles; synthetic large 256x256 grid = 131,072 triangles) and their manifest.
- `outputs/` - the canonical normal converter output per asset. Every per-run output was
  byte-identical across --threads 1 and 12 and across the three rounds; all hashes are in
  `results/measurements.json`, so only one copy is archived.
- `reports/` - all 18 build report JSON files (3 assets x 2 thread budgets x 3 rounds).
- `logs/` - the matching converter stdout/stderr logs.
- `previews/` - export-cut previews for model-space errors 0, 0.01, 0.1 and 1.0 (standard
  GLB, ordinary indices, no DEREN payload). These are for direct visual review only: no
  automatic image comparison was run, so the report makes no pixel-quality claim.
- `results/` - full measurement matrix, acceptance (both validators + LOD0/cut geometry),
  compact summary, asset/fixture/specified-input manifests, gate result.

## Re-derive the acceptance

```powershell
# from the repository root, with build-subtilis Release built and Python 3 + NumPy available
python scripts/measure/make_subtilis_converter_fixtures.py --out-dir .subtilis-work/fixtures
python scripts/measure/measure_subtilis_converter.py --converter build-subtilis/deren-subtilis.exe   --out-dir .subtilis-work/measure   --asset small=.subtilis-work/fixtures/small.glb   --asset large=.subtilis-work/fixtures/large.glb   --asset bistro=D:/deren-assets/bistro/bistro-merged.gltf   --dag-required large --dag-required bistro --threads 1,12 --rounds 3
python scripts/measure/verify_subtilis_converter.py gate   --measurements .subtilis-work/measure/measurements.json --out .subtilis-work/measure/gate.json
python scripts/measure/verify_subtilis_converter.py check   --measurements .subtilis-work/measure/measurements.json   --converter build-subtilis/deren-subtilis.exe   --validator D:/deren-assets/gltf-tools-khronos/2.0.0-dev.3.10/gltf_validator.exe   --validator-manifest scripts/measure/khronos_validator_manifest.json   --work-dir .subtilis-work/acceptance/previews   --out .subtilis-work/acceptance/acceptance.json
```

## Verify the archive

```powershell
python - <<'PY'
import hashlib, json
from pathlib import Path
root = Path('D:/deren-assets/2026-10-21-subtilis-converter')
manifest = json.loads((root / 'SHA256SUMS.json').read_text())
bad = []
for entry in manifest['files']:
    path = root / entry['path']
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != entry['sha256'] or path.stat().st_size != entry['bytes']:
        bad.append(entry['path'])
print('files', manifest['file_count'], 'mismatches', len(bad), bad)
PY
```

Retention: this archive is the durable copy; the workspace scratch directory used while
producing it was deleted after the hashes above were verified.
