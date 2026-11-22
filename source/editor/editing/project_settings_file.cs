using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using deren.scripting;

namespace deren.editor {
    /// <summary>Edits owned keys without reserializing the rest of a project TOML file.</summary>
    internal static class project_settings_file {
        internal static bool try_write(string path, string original,
                                       IReadOnlyDictionary<string, Dictionary<string, string>> edits, out string error) {
            error = string.Empty;
            var parsed = toml_table.parse(original, out _);
            var lines = original.Replace("\r\n", "\n").Split('\n').ToList();
            foreach (var (section, values) in edits) {
                if (!update_section(lines, parsed?.table(section), section, values, out error)) {
                    return false;
                }
            }
            var replacement = string.Join(original.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n", lines);
            if (replacement == original) { return true; }
            var temporary = path + $".{Guid.NewGuid():N}.tmp";
            try {
                if (!string.Equals(File.ReadAllText(path), original, StringComparison.Ordinal)) {
                    error = "The file changed on disk. Reopen the panel and try again.";
                    return false;
                }
                File.WriteAllText(temporary, replacement);
                File.Move(temporary, path, overwrite: true);
                return true;
            } catch (Exception failure) when (failure is IOException or UnauthorizedAccessException) {
                error = failure.Message;
                return false;
            } finally {
                if (File.Exists(temporary)) {
                    try { File.Delete(temporary); }
                    catch (Exception failure) when (failure is IOException or UnauthorizedAccessException) {
                        host_logger.write($"Project settings: could not clean up '{temporary}': {failure.Message}");
                    }
                }
            }
        }

        private static bool update_section(List<string> lines, toml_table? table, string section,
                                           Dictionary<string, string> values, out string error) {
            error = string.Empty;
            var remaining = new Dictionary<string, string>(values, StringComparer.Ordinal);
            bool inside = false;
            bool top_level = true;
            int end = -1;
            for (int index = 0; index < lines.Count; ++index) {
                var code = toml_table.strip_comment(lines[index]);
                var text = code.Trim();
                if (text.StartsWith('[')) {
                    top_level = false;
                    if (inside) { end = index; break; }
                    inside = !text.StartsWith("[[", StringComparison.Ordinal) && text.EndsWith(']') &&
                             toml_table.split_key(text[1..^1].Trim()) is [var name] && name == section;
                    if (inside) { end = lines.Count; }
                    continue;
                }
                int equals = toml_table.find_unquoted(code, '=');
                if (equals < 0) { continue; }
                var value = code[(equals + 1)..].TrimStart();
                if (value.StartsWith(new string('"', 3), StringComparison.Ordinal) ||
                    value.StartsWith(new string('\'', 3), StringComparison.Ordinal)) {
                    error = "This file uses multiline TOML strings. Edit the settings in the file.";
                    return false;
                }
                var key = toml_table.split_key(code[..equals].Trim());
                if (top_level && key is { Length: > 0 } && key[0] == section) {
                    error = $"Edit [{section}] in the file because it uses dotted or inline assignments.";
                    return false;
                }
                bool continued = toml_table.needs_more(value);
                if (inside && key is [var field] && values.TryGetValue(field, out var replacement)) {
                    if (continued || !remaining.Remove(field)) {
                        error = $"Edit [{section}] {field} in the file because it spans lines or is duplicated.";
                        return false;
                    }
                    var comment = lines[index][code.Length..];
                    lines[index] = code[..(equals + 1)] + " " + replacement + (comment.Length == 0 ? "" : " " + comment);
                } else {
                    while (continued && index + 1 < lines.Count) {
                        value += " " + toml_table.strip_comment(lines[++index]).Trim();
                        continued = toml_table.needs_more(value);
                    }
                }
            }
            if (remaining.Count == 0) { return true; }
            if (table != null && table.keys.Any(remaining.ContainsKey)) {
                error = $"Edit [{section}] in the file because it uses dotted or inline assignments.";
                return false;
            }
            if (end < 0) {
                if (lines.Count > 0 && lines[^1].Length != 0) { lines.Add(string.Empty); }
                lines.Add($"[{section}]");
                lines.AddRange(remaining.Select(pair => $"{pair.Key} = {pair.Value}"));
                lines.Add(string.Empty);
            } else {
                while (end > 0 && lines[end - 1].Trim().Length == 0) { --end; }
                lines.InsertRange(end, remaining.Select(pair => $"{pair.Key} = {pair.Value}"));
            }
            return true;
        }
    }
}
