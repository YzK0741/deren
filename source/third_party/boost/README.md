# Boost.Graph header subset

Boost 1.92.0, extracted with upstream BCP 1.92.0. This directory contains
1,665 unmodified headers (16,824,687 bytes), their Boost Software License 1.0,
a SHA-256 manifest, and the extraction log. It is not the complete Boost release.

Source: the MSYS2 clang64 package
`mingw-w64-clang-x86_64-boost-1.92.0-4-any.pkg.tar.zst`, from
https://repo.msys2.org/mingw/clang64/ . Every vendored header was compared
byte-for-byte with that cached package, not merely the mutable installed tree.
Package SHA-256:
`2236ec2a5a52b9543e5bd278bc9ce6fceab72f59d736ca7eb70293135d6469e3`.
No deren patches or namespace rewriting.
The repository marks this subtree -text so Git's autocrlf cannot change the
bytes checked by the manifest on Windows checkouts.

Extractor source: https://github.com/boostorg/bcp/tree/boost-1.92.0 .
The codeload `tar.gz/refs/tags/boost-1.92.0` archive SHA-256 was
`7a08aab334d65adca21deb3c9b03ccbeaa301b0f593de6ac870eb6c2c47c4248`.
BCP is an extraction-time tool; builds do not download or run it.

## Reproduce and verify

Prepare a source root containing the pinned `boost/` tree and its
`LICENSE_1_0.txt`. Do not point BCP at a mixed system include directory:
otherwise it can also collect system headers. Build BCP from the pinned tag,
then run from the deren repository root, using a fresh staging directory:

```text
python scripts/tools/vendor_boost_graph.py --boost-root <source-root> --bcp <bcp-executable> --output <empty-staging-directory>
python scripts/tools/vendor_boost_graph.py --output source/third_party/boost --verify
```

The script fixes the six entry headers in headers.json, validates the version,
preserves original bytes, and records every copied header's SHA-256. Compare
the staging manifest with the checked-in manifest before replacing the vendor
directory. Official upstream source and downstream packages may differ; the
package and manifest above define this exact snapshot.

BCP logs warnings for conditional macro includes, including distributed Graph
headers absent from the source package. MPI/distributed Graph is unsupported;
do not enable BOOST_GRAPH_USE_MPI. The tested serial includes compile entirely
from this subtree, without falling back to system Boost headers. Other Boost
facilities in the transitive closure are not independently supported APIs.

## Integration contract

The CMake INTERFACE target boost_graph_headers provides this include root as
SYSTEM plus BOOST_NO_EXCEPTIONS and BOOST_ALL_NO_LIB. It links no Boost binary
library. Keep it private to graph implementation units; no Boost type belongs
in the RHI ABI or public graph interface.

test_boost_graph verifies the private adapter shared with render_graph_core:

- bidirectional adjacency_list, custom vessel vector/list selectors;
- graph-property operator new/delete using utility::memory;
- reverse_graph, filtered_graph, and breadth_first_visit with explicit vessel
  color storage and work queue;
- project stable Kahn scheduling, reporting cycles through expected rather
  than invoking the throwing topological_sort;
- effective ordinary-new observation, including a stock-BGL positive control;
- a 20,000-node chain, and injected utility allocation failure in a child.
- allocation observation of the actual CPU graph module, in addition to the adapter.

The custom random-access selector specializes boost::detail::is_random_access,
an internal trait verified against this pinned version. Recheck it on upgrades.
Default DFS workspace, default property allocation and arbitrary BGL mutations
are not covered by the zero-ordinary-new result. Do not assume every BGL
algorithm automatically uses the project allocator. Unexpected Boost exception
hooks terminate; normal graph cycle errors must not reach those hooks.

Windows Clang64 Release and ASan/UBSan probes passed. A separate temporary C++
module/client probe also passed without exposing Boost in the client. Linux
and MSVC integration have not been executed locally.

## Boost.Int128 headers

`boost/int128.hpp` and the `boost/int128/` headers are a separate pinned
Boost.Int128 snapshot, folded into the same `boost/` include tree. Upstream:
https://github.com/boostorg/int128, pinned Git revision
`16f0b3374470e3821c10277775de18cf320cda45`. The headers and
`LICENSE_1_0.txt` are original Git blob bytes, with no local upstream patches;
`int128-headers.json` records the SHA-256 of all 34 upstream files. The root
Boost Software License 1.0 covers both sets. Preserve vendor bytes using the
root `.gitattributes`.

Reproduce and verify:

```text
python scripts/tools/vendor_boost_int128.py --source <upstream-checkout> --output <new-staging-directory>
python scripts/tools/vendor_boost_int128.py --output source/third_party/boost --verify
```

The extractor reads pinned Git blobs rather than line-ending-converted working
files, and rejects a different HEAD. Compare manifests before any update.
`test_boost_int128_vendor` verifies the checked-in snapshot offline, including
extra/missing headers. Builds never fetch dependencies.

## Boost.Int128 integration boundary

The standalone `boost_int128_headers` CMake target is private to
`static_utility` and the private encoder test. Only `boost/int128/int128.hpp`
is included by the private BVH helper, in the utility module's global fragment.
There is no public `int128_t`/`uint128` alias and no Boost integer in the RHI or
exported utility interface. The helper returns twelve bytes; BVH storage,
quantization and byte-lexicographic sorting remain unchanged.
