# Asset directory

The sample models and Gaussian clouds in this directory are third-party material. They are
**not covered by the repository's MIT license**; each folder carries its own terms, and the
engine source license does not change them.

Models and Gaussian clouds are stored one folder per asset. The renderer's default model is
`asset/DamagedHelmet/DamagedHelmet.gltf`; the README character screenshot uses
`asset/laevatain/laevatain_game_toon_preview.glb`; the Gaussian screenshot uses
`asset/flowers/flowers_1.ply`.

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
