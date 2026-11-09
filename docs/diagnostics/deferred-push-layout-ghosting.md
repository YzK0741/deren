# Deferred push-layout mismatch and motion ghosts

## Symptom

With TAA disabled, rotating the camera produced two displaced character silhouettes.
The same mismatch also affected PBR objects. The user's desktop recording showed
the fault; a scripted fast camera sweep reproduced it in the renderer's PNG.

## Cause

`deferred_pass::push_constants` sends an 88-byte payload. The runtime appends
`frame_slot` and `image_index` after that payload. `DeferredPush` in
`lighting/deferred.slang` omitted the reserved `punctual` float:

| Field | Host offset | Broken SPIR-V offset | Correct SPIR-V offset |
| --- | ---: | ---: | ---: |
| unlit | 80 | 80 | 80 |
| punctual (reserved) | 84 | absent | 84 |
| frame_slot | 88 | 84 | 88 |
| image_index | 92 | 88 | 92 |

The shader read the zero-valued reserved float as its frame slot and the real
frame slot as its image index. A frame slot is not an acquired swapchain image
index. Deferred lighting consequently sampled a different image's G-buffer;
GameToon then drew the current character over the displaced deferred result.
This explains why the fault was visible with a single character and with TAA off.

## Fix and verification

Restore the reserved float in the shader without changing the host payload.
Host static assertions pin the payload offsets. `test_deferred_push_layout.py`
reads the actual compiled SPIR-V member decorations and checks the complete ABI;
it failed on the old binary at offsets 84/88 and passed at 88/92 after the fix.

The private-shader experiment changed only that float. The TAA-off fast camera
sweep then stopped producing duplicate silhouettes. The installed shader was
rebuilt, and the layout, shader-source, pass, and lighting-graph tests passed.
`spirv-val --target-env vulkan1.3` also accepted the rebuilt deferred shader.

Local investigation captures (ignored, not distributed):

- Before: `tmp/ghost-fast-toon/toon-taa0-move1/screenshot_20261103_053500.png`
- Isolated fix: `tmp/ghost-layout-fix/toon-taa0-move1/screenshot_20261103_062156.png`
- Installed-build verification: `tmp/ghost-final/`

The separate TAA reprojection/depth-history limitations were not changed here.
