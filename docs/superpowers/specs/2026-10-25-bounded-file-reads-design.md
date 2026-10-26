# Bounded binary file reads design

User approved the utility file-read follow-up on 2026-10-25. Scope is the two existing read_binary_*
functions and their callers; handle recycling is deferred.

Keep names and utility namespace. Both functions require an explicit uintmax_t max_bytes argument and
return std::expected<the existing vessel container, file_error>. file_error carries file_error_code and
std::error_code cause without owning a formatted string. Codes: status_failed, not_regular_file,
open_failed, size_unavailable, too_large, read_failed, file_changed. file_error_name gives a static name.
A stream failure may expose only std::io_errc::stream rather than a platform-native errno.

Only regular files (including symlinks resolving to regular files) are accepted. Check metadata,
open in binary mode, seek/tell on the opened stream for size; reject above max_bytes or container
max_size before allocation or narrowing. Size zero is valid and a zero bound permits only empty files.
Read the initially observed size in at most 1 MiB chunks. A short EOF or an extra byte afterward is
file_changed; actual stream failures are read_failed. Never return partial data. This detects observed
length changes, not same-size edits or a consistent snapshot; it does not lock files or defend against
hostile path replacement between metadata and open. OOM retains existing fast-panic behavior.

Move implementation to source/utility/file_io.cpp; declarations remain in utility.cppm. No file-system
framework or RHI types. Migrate shader loading (only production caller) to a 64 MiB bound, include error
stage/cause in panic, and move the returned payload rather than copying it. Utility source interface
becomes 3.0.0 because old optional/one-argument calls intentionally require migration; RHI ABI stays55.
