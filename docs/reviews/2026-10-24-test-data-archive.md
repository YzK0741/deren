# Old test-data archive (2026-10-24)

Archived the pre-2026-10-24 experiment/test artifacts to two zstd tar archives; sources retained.

- Location: `D:/deren-assets/archives/2026-10-24-old-test-data/`
- `deren-old-test-data-d.tar.zst` - 1,669,117,912 B compressed,
  3,715 files from `D:/deren-assets/{hzb-b2,hzb-cpu,hzb-recovery,2026-10-21-subtilis-converter,
  2026-10-21-object-lod,2026-10-21-object-lod-probe,2026-10-22-software-raster,2026-10-23-software-raster}`;
  SHA-256 `2636004b8d1510fa692a47a7b1d4a5ba8f778aad5acd3c40245e39d31ff04267`.
- `deren-workspace-tmp.tar.zst` - 702,653,876 B compressed,
  7,692 files from workspace `tmp/`; SHA-256 `3b0a34af97edc5f87534144ca0fa8c9464619377df70e979d267f286f3cf7693`.
- Uncompressed source: 8,862,033,216 B / 11,407 files. Per-file SHA-256:
  `source-sha256-manifest.txt`; archive hashes: `archive-sha256.txt`; index/README: `ARCHIVE-README.md`.
- Verification: `zstd -t` both archives exit 0; `tar -tf` file counts 3,715 + 7,692 = 11,407 = manifest count.
- Scope: bistro assets, gltf-tools/npm caches and current live evidence are excluded. Sources were not
  deleted; the D: evidence paths used by older reviews therefore still resolve.
- Recommendation: after the archive is accepted as canonical, the listed source directories can be removed
  to reclaim ~8.3 GB; do that only after updating any review links that point at the retained D: paths.
