# Gaussian GUI disappearance investigation

The user reproduced a disappearing/flickering GUI in static Gaussian views. The same pre-Subtilis build (`344c3ef8`) exhibits the problem; its PBR helmet view is normal. This is not evidence of a Subtilis regression.

## Localization

Current-build validation-enabled captures reproduce a missing GUI with a static flower cloud and a visible GUI while the camera sweeps. ImGui generates nonempty draw lists every frame, with valid positions, clip rectangles and framebuffer scale. A temporary clear marker at the end of the GUI secondary is visible even when the GUI is absent: the secondary executes and its attachment is not overwritten afterwards.

The CPU Gaussian producer also reproduces the problem. Omitting the splat draw does not restore the GUI. Disabling frustum culling does restore it; forcing reculling does not. The point-cloud scene currently contains zero mesh leaves, so these observations do not establish a defective camera cache. Static camera orientation alone is not the trigger: a fixed 32-degree view still fails.

A temporary GUI fragment shader without texture sampling restores panel rectangles and character quads. Samplerless texel fetch still fails. Readback of the font atlas immediately after each upload matches every CPU row, including its opaque white texel. Republishing the descriptor after upload and using GENERAL layouts do not fix the symptom. These diagnostic changes have been removed; the vendored ImGui backend and original shaders are unchanged.

## Targeted compatibility fix

The Vulkan GUI adapter explicitly binds the device's resource/sampler heaps in its own secondary before ImGui establishes its legacy descriptor sets. It still does not inherit heaps. Binding errors are reported and the incomplete segment is refused. This initializes the binding state at the engine/GUI boundary without changing Gaussian rendering, culling or caching.

The local evidence supports this as a binding-state compatibility fix; it does not establish that Vulkan universally requires this initialization or prove an NVIDIA driver defect. The specification allows switching binding models and describes their state invalidation rules: [Descriptor heaps](https://docs.vulkan.org/spec/latest/chapters/descriptorheaps.html).

## Acceptance

Clean A/B captures use the original GUI shaders, the original ImGui backend, normal splat draws and frustum culling. `tmp/gui-flicker/control-legacy` temporarily skips only the added heap bind; `fixed-*` enables it. Both use validation layers and a real window-system swapchain, with the window hidden during capture. The control has zero bright glyph pixels in the known panel rectangle; the first fixed static view has 1,842. All five corrected cases pass: static GPU frames8/9, static CPU, PBR and a moving GPU view; all exit0 with zero validation errors. Final production code removes the control flag and all diagnostics; its independent16frame static capture also passes, with no marker remaining. Full build succeeds and CTest passes68/68. [Recorded results](2026-10-24-gaussian-gui-results.json). These captures establish the local regression fix, not universal driver coverage.

Rejected/diagnostic artifacts remain under ignored `tmp/gui-flicker/`; they are not performance measurements. The malformed `with-mesh` configuration is excluded from evidence. No commits or pushes were made.
