# Boost.Int128 private snapshot

Upstream: https://github.com/boostorg/int128

Pinned Git revision: `16f0b3374470e3821c10277775de18cf320cda45`.
This is the revision measured in `docs/benchmarks/2026-10-28-morton-int128.md`,
not an unpinned branch or an assumed Boost release version. The include tree
and `LICENSE` are original Git blob bytes, with no local upstream patches.
`manifest.json` records the SHA-256 of all 34 upstream files. License: Boost
Software License 1.0. Preserve vendor bytes using the root `.gitattributes`.

## Reproduce and verify

Check out the official repository at the revision above, then extract into a
new staging directory (no network or in-place replacement in this script):

```text
python scripts/tools/vendor_boost_int128.py --source <upstream-checkout> --output <new-staging-directory>
python scripts/tools/vendor_boost_int128.py --output source/third_party/boost_int128 --verify
```

The extractor reads pinned Git blobs rather than line-ending-converted working
files, and rejects a different HEAD. Compare manifests before any update.
`test_boost_int128_vendor` verifies the checked-in snapshot offline, including
extra/missing headers and the license. Builds never fetch dependencies.

## Integration boundary

The standalone `boost_int128_headers` CMake target is private to
`static_utility` and the private encoder test. It is separate from the existing
Boost.Graph subset. Only `boost/int128/int128.hpp` is included by the private
BVH helper, in the utility module's global fragment. There is no public
`int128_t`/`uint128` alias and no Boost integer in the RHI or exported utility
interface. The helper returns twelve bytes; BVH storage, quantization and
byte-lexicographic sorting remain unchanged.

Windows Clang64 production and sanitizer validation are recorded in the
integration report. MSVC/Linux are not claimed tested solely because upstream
supports them. Recheck performance and compiler/module compatibility on updates.
