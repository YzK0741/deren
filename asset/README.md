# Asset directory

The sample models and Gaussian clouds in this directory are third-party material. They are
**not covered by the repository's MIT license**; each folder carries its own terms, and the
engine source license does not change them.

Models and Gaussian clouds are stored one folder per asset. The renderer's default model is
`asset/DamagedHelmet/DamagedHelmet.gltf`; the README character screenshot uses
`asset/laevatain/laevatain_game_toon_preview.glb`; the Gaussian screenshot uses
`asset/flowers/flowers_1.ply`.

## Asset library

This directory is also a folder-backed asset catalogue. Each asset folder carries an
`asset.toml` manifest with a stable `id`, a display `name`, a `type` (`pbr`, `toon`, `unlit`
or `gaussian`), the `model` path relative to the folder, and optional `sidecar`, `license`,
`thumbnail` and `tags` keys. Optional `[toon]`, `[environment]` and `[camera]` tables declare
asset-specific rendering data. A manifest path must stay inside its asset folder; absolute
paths and `..` are rejected.

`library.toml` at this root sets the catalogue policy: `default` names the id used when no
asset is requested, `scan_depth` bounds the folder walk, and `ignore` lists folder prefixes to
skip.

```sh
# start the catalogue's default asset
deren --asset-library asset

# start one catalogue entry
deren --asset-library asset --asset flight-helmet
```

A positional model path still wins over `--asset`, and `--asset` wins over a `model` value
from `config.toml`; `[paths] asset_library` and the top-level `asset` key are the config-file
equivalents. `[toon] game_toon` is explicit manifest metadata: it never switches the
experimental game-toon path on by itself.

| Folder | Contents | Terms |
| --- | --- | --- |
| `DamagedHelmet/` | Khronos DamagedHelmet sample (glTF, textures, license) | See `DamagedHelmet/LICENSE.md` |
| `FlightHelmet/` | Khronos FlightHelmet sample (glTF, textures, license) | CC0-1.0, see `FlightHelmet/LICENSE.md` |
| `flowers/` | NVIDIA Bouquet of Flowers Gaussian splat cloud | CC BY 4.0, see `flowers/README.md` |
| `laevatain/` | Laevatain / GameToon preview model and Toon sidecar | **All rights reserved by Hypergryph Network Technology Co., Ltd.** — see `laevatain/LICENSE.md` |

`laevatain/*.glb` and `flowers/*.ply` are stored with Git LFS. After cloning, install
`git-lfs` and run `git lfs pull` if the assets were not fetched automatically.

The Laevatain asset is included only so deren's game-toon profile path can be exercised
against the same source material as the repository's hand-captured GameToon screenshot.
Its presence in this repository is not a license grant: redistribution, commercial use,
modification and reuse are not granted, and whether the files may be redistributed at all
as part of the repository is unresolved. Do not redistribute them without authorization
from the rights holder. See `laevatain/LICENSE.md`.
