# Gaussian loader progress — 2026-10-10

Stopped at the user-requested weekly quota threshold: used95%, remaining5%. No reset credits used.

Implemented standalone deren.gaussian_loader CPU module and gaussian_loader CMake target. parse_ply reads memory; load_ply reads files. ASCII and binary little-endian PLY1.0 support SH0..3, named scalar fields, reordered attributes, scale/opacity/quaternion conversion, coefficient-major SH and rotated3-sigma bounds. Structured errors reject malformed, truncated, nonfinite and over-budget data. No GPU work added.

Tests: full Release build succeeded; loader53/53 checks; CTest21/21. CI registration and Doxygen manual updated. Independent review found bounds narrowing at large coordinates; new regression reproduced2 failures, conditional outward nextafter fixed them. Documentation describes exact limits and existing no-exceptions allocation behavior.

No real external asset was downloaded or loaded; current validation uses generated ASCII/binary fixtures. GPU upload, viewer, rendering, sorting and scene integration remain unimplemented. Next step is a real trained PLY smoke test and then the separately planned GPU stage.

Original tracked deletion deren_probe_backend_detach_probe.dll is preserved and excluded from the commit. Work is on master; no push.
