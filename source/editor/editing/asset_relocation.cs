using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using deren.scripting;

namespace deren.editor {
    internal static class asset_relocation {
        private const string manifest_name = "asset.toml";
        private static readonly StringComparison path_comparison = OperatingSystem.IsWindows()
            ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;

        internal static bool try_apply(asset_authoring authoring, string id, string destination, string display_name,
                                       out string error) {
            error = string.Empty;
            string? original_manifest = null;
            string? source = null;
            string? location = null;
            bool written = false;
            var created_parents = new Stack<string>();
            try {
                if (authoring.refresh() != script_status.success ||
                    authoring.library_root() is not { } root_path ||
                    authoring.asset_directory(id) is not { } source_path ||
                    authoring.display_name(id) is not { } previous_name) {
                    throw new IOException("Refresh the library and select a valid asset.");
                }
                string root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(root_path));
                source = location = Path.TrimEndingDirectorySeparator(Path.GetFullPath(source_path));
                string[] parts = destination.Split(['/', (char)92]);
                if (parts.Any(part => part.Length == 0 || part.StartsWith('.') ||
                    part.EndsWith('.') || part.EndsWith(' ') || part.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0)) {
                    throw new IOException("Enter a library-relative folder path with ordinary folder names.");
                }
                string target = Path.GetFullPath(Path.Combine(root, Path.Combine(parts)));
                bool moving = !string.Equals(source, target, StringComparison.Ordinal);
                if (!inside(root, source) || !inside(root, target) || inside(source, target)) {
                    throw new IOException("The destination must be inside the asset library and outside the asset folder.");
                }
                require_plain_path(root, source);
                require_plain_path(root, target);
                string manifest = Path.Combine(source, manifest_name);
                require_plain_path(root, manifest);
                if (moving && !same_path(source, target) && (Directory.Exists(target) || File.Exists(target))) {
                    throw new IOException("A file or folder already exists at that destination.");
                }
                if (display_name.Length == 0 || display_name.Any(char.IsControl)) {
                    throw new IOException("Enter a display name without control characters.");
                }

                original_manifest = File.ReadAllText(manifest);
                var edits = new Dictionary<string, string>(StringComparer.Ordinal);
                if (display_name != previous_name) { edits.Add("name", display_name); }
                string? motion = authoring.motion_path(id);
                string? expected_motion = motion;
                if (moving) {
                    var parsed = toml_table.parse(original_manifest, out _);
                    if (parsed?.text("id") is not { } written_id) {
                        edits.Add("id", id);
                    } else if (written_id != id) {
                        throw new IOException("The asset ID changed on disk. Refresh the library and try again.");
                    }
                    if (motion != null && inside(source, motion)) {
                        expected_motion = Path.Combine(target, Path.GetRelativePath(source, motion));
                    } else if (motion != null) {
                        edits.Add("motion", Path.GetRelativePath(target, motion).Replace(Path.DirectorySeparatorChar, '/'));
                    }
                    var ids = asset_manager.list(out var status);
                    if (status != script_status.success) { throw new IOException("Could not verify motion references."); }
                    foreach (var other in ids) {
                        if (other != id && authoring.motion_path(other) is { } reference && inside(source, reference)) {
                            throw new IOException($"Asset '{other}' uses motion files inside this asset. Move those files or update its reference first.");
                        }
                    }
                    for (var parent = Path.GetDirectoryName(target)!; !same_path(parent, root); parent = Path.GetDirectoryName(parent)!) {
                        if (File.Exists(Path.Combine(parent, manifest_name))) {
                            throw new IOException("The destination cannot be nested inside another asset package.");
                        }
                    }
                }
                if (!moving && edits.Count == 0) { return true; }

                string replacement = rewrite_manifest(original_manifest, edits);
                if (replacement != original_manifest) {
                    if (File.ReadAllText(manifest) != original_manifest) {
                        throw new IOException("The asset manifest changed on disk. Reopen the panel and try again.");
                    }
                    write_manifest(manifest, replacement);
                    written = true;
                }
                if (moving) {
                    string parent = Path.GetDirectoryName(target)!;
                    create_parents(parent, created_parents);
                    if (same_path(source, target)) {
                        string intermediate = Path.Combine(Path.GetDirectoryName(source)!, $".relocate-{Guid.NewGuid():N}");
                        Directory.Move(location, intermediate);
                        location = intermediate;
                    }
                    Directory.Move(location, target);
                    location = target;
                }
                if (authoring.refresh() != script_status.success ||
                    authoring.asset_directory(id) is not { } indexed || !same_path(indexed, target) ||
                    authoring.display_name(id) != display_name ||
                    !same_path(authoring.motion_path(id), expected_motion)) {
                    throw new IOException("The asset was not indexed at the destination. Check the library scan depth, ignore rules, and relative paths.");
                }
                return true;
            } catch (Exception failure) when (failure is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException) {
                error = failure.Message;
                try {
                    if (source != null && location != null && location != source) {
                        if (same_path(location, source)) {
                            string intermediate = Path.Combine(Path.GetDirectoryName(source)!, $".relocate-{Guid.NewGuid():N}");
                            Directory.Move(location, intermediate);
                            location = intermediate;
                        }
                        Directory.Move(location, source);
                        location = source;
                    }
                    if (written && location != null && original_manifest != null) {
                        write_manifest(Path.Combine(location, manifest_name), original_manifest);
                    }
                    while (created_parents.TryPop(out var parent)) {
                        if (!Directory.EnumerateFileSystemEntries(parent).Any()) { Directory.Delete(parent); }
                    }
                } catch (Exception rollback) when (rollback is IOException or UnauthorizedAccessException) {
                    error += $" Rollback stopped at '{location}': {rollback.Message}";
                }
                authoring.refresh();
                return false;
            }
        }

        private static bool same_path(string? left, string? right) =>
            string.Equals(left?.Replace(Path.AltDirectorySeparatorChar, Path.DirectorySeparatorChar),
                          right?.Replace(Path.AltDirectorySeparatorChar, Path.DirectorySeparatorChar), path_comparison);

        private static bool inside(string parent, string path) =>
            path.StartsWith(Path.EndsInDirectorySeparator(parent) ? parent : parent + Path.DirectorySeparatorChar, path_comparison);

        private static void require_plain_path(string root, string path) {
            for (var current = path; !same_path(current, root); current = Path.GetDirectoryName(current)!) {
                if ((File.Exists(current) || Directory.Exists(current)) &&
                    File.GetAttributes(current).HasFlag(FileAttributes.ReparsePoint)) {
                    throw new IOException("Choose an asset path without symbolic links or junctions.");
                }
            }
        }

        private static void create_parents(string directory, Stack<string> created) {
            if (Directory.Exists(directory)) { return; }
            create_parents(Path.GetDirectoryName(directory)!, created);
            Directory.CreateDirectory(directory);
            created.Push(directory);
        }

        private static string rewrite_manifest(string original, Dictionary<string, string> edits) {
            if (edits.Count == 0) { return original; }
            string newline = original.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n";
            var lines = original.Replace("\r\n", "\n").Split('\n');
            for (var index = 0; index < lines.Length; ++index) {
                var code = toml_table.strip_comment(lines[index]);
                var text = code.Trim();
                if (text.StartsWith('[')) { break; }
                int equals = toml_table.find_unquoted(code, '=');
                if (equals < 0) { continue; }
                string value = code[(equals + 1)..].TrimStart();
                if (value.StartsWith(new string('"', 3), StringComparison.Ordinal) ||
                    value.StartsWith(new string((char)39, 3), StringComparison.Ordinal)) {
                    throw new IOException("This asset manifest uses multiline strings in its main section. Edit those fields in the file.");
                }
                if (toml_table.split_key(code[..equals].Trim()) is [var key] && edits.Remove(key, out var replacement)) {
                    var comment = lines[index][code.Length..];
                    lines[index] = code[..(equals + 1)] + " " + document_io.quote(replacement) +
                        (comment.Length == 0 ? "" : " " + comment);
                } else {
                    while (toml_table.needs_more(value) && index + 1 < lines.Length) {
                        value += " " + toml_table.strip_comment(lines[++index]).Trim();
                    }
                }
            }
            return string.Concat(edits.Select(pair => $"{pair.Key} = {document_io.quote(pair.Value)}{newline}")) +
                string.Join(newline, lines);
        }

        private static void write_manifest(string path, string content) {
            var temporary = path + $".{Guid.NewGuid():N}.tmp";
            try {
                File.WriteAllText(temporary, content);
                File.Move(temporary, path, overwrite: true);
            } finally {
                if (File.Exists(temporary)) { File.Delete(temporary); }
            }
        }
    }
}
