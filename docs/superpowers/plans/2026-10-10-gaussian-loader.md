# Gaussian loader implementation plan

> Execute inline with superpowers:executing-plans. User explicitly requests loader implementation on master.

**Goal:** independent CPU loader for ASCII and binary little-endian INRIA 3DGS PLY.
**Architecture:** deren.gaussian_loader exports immutable-by-convention CPU asset, bounded options, parse/load and structured errors. No GPU dependency.
**Spec:** ../specs/2026-10-10-gaussian-splatting-design.md sections 2, 3 and 9.

## Constraints
16 SH coefficients maximum; scale exp, opacity sigmoid, quaternion wxyz normalization; preserve previous scene on errors. Preserve preexisting DLL deletion. Stop and report at 5% remaining quota.

## Task 1: loader and tests
- [x] Add module target and failing tests for both encodings, conversion and malformed/limited inputs.
- [x] Observe RED, implement bounded parsing, conversion and support bounds.
- [x] Verify file loading, reordered/unknown properties, SH packing, truncation, invalid numbers and budgets.
- [x] Full build, CTest, independent review, document API and commit locally.

Review focus: integer scalar range, binary endian decoding, finite covariance, overflow before allocation, no partially published result.

Validation: full Release build; loader53/53; CTest21/21. Review found inward-rounded support bounds; regression failed before conditional outward nextafter, passes after correction. CI test list and Doxygen INPUT synchronized.
