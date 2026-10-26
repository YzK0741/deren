# Bounded file reads - implementation and verification

Date: 2026-10-25. Base: ece1780b. No commit or push requested for this utility increment.

## Delivered

`deren.utility` source interface 3.0.0 requires `max_bytes` on both `read_binary_to_vector` and
`read_binary_to_string`. Results are `std::expected<the existing vessel container, file_error>`;
`file_error` has a stage code plus available system/stream cause, and `file_error_name` returns a
static diagnostic name. The utility remains independent of RHI; RHI ABI stays 55.

Implementation lives in source/utility/file_io.cpp. It accepts regular files, measures length through
the opened stream, checks payload limit and container max_size before narrowing/allocation, and reads
in chunks of at most 1 MiB. This utility increment made no RHI ABI change (the base was ABI 55); a separate
lazy-resource change later advanced the tree to ABI 57 without changing this file-read contract. Short EOF
or observed trailing data produces file_changed, other stream
failure produces read_failed. No partial payload is returned. Empty files are valid at a zero limit;
strings preserve binary bytes rather than performing text decoding. Bounds concern payload size,
not container capacity rounding, terminators or allocator metadata. OOM remains a fatal allocator event.

The single production consumer, shader bootstrap, uses 64 MiB and logs the stage/cause on failure;
returned bytes are moved to the destination instead of copied. No legacy unbounded overload remains.
CMake and the explicit CI tidy input list include the new implementation file. Public docs cover
source migration and ownership of file-error policy; no file-system framework was introduced.

## Evidence

- RED: new expected/error/limit tests failed compilation against the old one-argument optional APIs.
- Focused test passed after implementation. Final utility test: 196 checks, zero failures.
- Tests cover vector/string result types, embedded NUL/high bytes, exact and undersized limits, zero
  bound on empty/nonempty files, missing path cause, directory rejection and a 1 MiB + 37 byte round trip.
- Allocation failure injection with the large fixture verifies both vector and string reject an
  oversize payload before touching the vessel allocator; the fixture exceeds string inline storage.
- Full Release build with 14 workers: exit 0; final rebuild after tidy fixes also exit 0.
- Final CTest: 71/71 passed (28.55 seconds), including utility, documentation and dependency boundary tests.
- Device probes: 7/7 passed; dynamic runtime device check: 1174 checks, zero failures, ABI 55.
- Independent review found no production defect; its P3 test-coverage finding (string inline storage
  hid a possible allocation-order regression) was addressed with the large-fixture injection check.
- First tidy pass flagged missing braces and implicit int multiplication widening in the new source;
  both were corrected. Final 40-source CI tidy result: exit 0.

Local evidence: tmp/bounded-file-{red,focused-test,full-build,final-build,final-ctest,device,runtime}.log
and tmp/ci-fix-tidy-primary-all.log. These temporary build artifacts are not repository documents.

## Limits

No deterministic mutation/short-read, seek-failure or OS-I/O-fault injection was introduced; those
branches were reviewed, not claimed as dynamically covered. Symlink acceptance is specified but has
no platform-specific regression test. This does not provide a file snapshot, detect same-size edits,
lock files or harden against hostile path replacement between metadata lookup and open.

No local sanitizer rebuild, Linux full-module build or remote CI is claimed. The existing sanitizer
workflow already builds test_utility; no new test executable needs registration. Handle recycling,
file writing and other loaders' independent read paths remain out of scope.
