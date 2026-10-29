# Environment lighting workflow

Approved direction: improve DamagedHelmet's environment lighting using Sample Viewer as a reference.
Build on the existing HDR-to-cube, irradiance, GGX prefilter and BRDF LUT. Preserve the default
procedural sky and ACES look. No atmospheric scattering, fog, SSR or material changes in this scope.

Controls: relative environment strength (default 1, preserving existing baked HDR intensity),
Y-axis rotation in degrees, procedural/solid/environment/blurred background, linear background
colour and background blur. Background selection never changes object lighting. Both IBL lobes
and the environment background use the same rotation and strength. Expose ACES and Khronos PBR
Neutral tone mapping; exposure remains the existing linear multiplier.

An environment menu lists procedural sky, the configured HDR and .hdr files in an adjacent
environments directory. Selection starts CPU generation asynchronously, preserving the displayed
environment until all new images are ready. Publication occurs at a frame boundary using the
existing wait-idle transactional IBL upload. File/decode/publication failures keep the previous
environment and produce a visible status. No dialog/GUI ABI expansion is required.

Keep new shader settings at the LightUBO tail with explicit layout checks. Do not change RHI ABI.
Tone selection is an explicit post constant shared by every post consumer. Sanitize non-finite
configuration values; document linear colour, rotation sign, paths and strength semantics.

Acceptance: CPU tests for controls/rotation/configuration, shader build/layout checks, existing
render tests, default-image regression and DamagedHelmet captures with pinned official HDR assets,
fixed camera/exposure/lighting. The supplied screenshot has no environment/settings metadata;
do not claim pixel equivalence to it. Show actual output and retain capture configs/hashes.
