using System;
using System.Collections.Generic;
using System.Linq;
using deren.scripting;

namespace deren.editor;

internal static class input_actions_file {
    private static readonly Dictionary<int, string> named_keys = new() {
        [key_code.space] = "space", [key_code.enter] = "enter", [key_code.tab] = "tab",
        [key_code.left] = "left", [key_code.right] = "right", [key_code.up] = "up", [key_code.down] = "down",
        [key_code.left_shift] = "left_shift", [344] = "right_shift",
        [key_code.left_control] = "left_control", [345] = "right_control",
    };

    internal static string? key_name(int key) {
        if (key is >= 'A' and <= 'Z') { return ((char)(key + 'a' - 'A')).ToString(); }
        if (key is >= '0' and <= '9') { return ((char)key).ToString(); }
        return named_keys.GetValueOrDefault(key);
    }

    internal static bool try_read(string manifest, out SortedDictionary<string, List<string>> actions, out string error) {
        actions = new(StringComparer.Ordinal);
        error = string.Empty;
        var input = toml_table.parse(manifest, out _)?.table("input");
        var table = input?.table("actions");
        if (table == null) {
            if (input?.keys.Contains("actions") != true) { return true; }
            error = "[input.actions] must be a table.";
            return false;
        }
        foreach (var name in table.keys) {
            if (table.strings(name) is not { Count: > 0 } keys) {
                error = $"Action '{name}' needs at least one keyboard binding.";
                return false;
            }
            actions.Add(name, keys);
        }
        return true;
    }

    internal static bool try_save(string path, string source, SortedDictionary<string, List<string>> actions, out string error) {
        if (!try_read(source, out var before, out error)) { return false; }
        foreach (var (name, keys) in actions) {
            if (name.Length == 0 || keys.Count == 0 || keys.Any(key => !valid_key(key))) {
                error = $"Action '{name}' needs at least one supported keyboard binding.";
                return false;
            }
        }
        if (same_actions(before, actions)) { return true; }

        string newline = source.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n";
        var lines = source.Replace("\r\n", "\n").Split('\n').ToList();
        int first = -1;
        int last = lines.Count;
        bool root = true;
        bool inline_input = false;
        for (int index = 0; index < lines.Count; ++index) {
            string code = toml_table.strip_comment(lines[index]).Trim();
            if (code.StartsWith('[') && code.EndsWith(']')) {
                root = false;
                if (first >= 0) { last = index; break; }
                if (toml_table.split_key(code[1..^1].Trim()) is ["input", "actions"]) { first = index + 1; }
                continue;
            }
            int equals = toml_table.find_unquoted(code, '=');
            if (equals < 0) { continue; }
            inline_input |= root && toml_table.split_key(code[..equals].Trim()) is ["input"];
            string value = code[(equals + 1)..];
            while (toml_table.needs_more(value) && index + 1 < lines.Count) {
                value += " " + toml_table.strip_comment(lines[++index]);
            }
        }
        if (first < 0) {
            if (inline_input || toml_table.parse(source, out _)?.table("input")?.table("actions") != null) {
                error = "Move the bindings to an explicit [input.actions] table before editing them here.";
                return false;
            }
            if (lines.Count != 0 && lines[^1].Length != 0) { lines.Add(string.Empty); }
            lines.Add("[input.actions]");
            first = last = lines.Count;
            lines.Add(string.Empty);
        }
        var updated = new List<string>();
        var pending = new SortedDictionary<string, List<string>>(actions, StringComparer.Ordinal);
        for (int index = first; index < last; ++index) {
            var code = toml_table.strip_comment(lines[index]);
            int equals = toml_table.find_unquoted(code, '=');
            if (equals < 0 || toml_table.split_key(code[..equals].Trim()) is not [var name] ||
                !before.TryGetValue(name, out var original)) {
                updated.Add(lines[index]);
                continue;
            }
            int start = index;
            string value = code[(equals + 1)..];
            while (toml_table.needs_more(value) && index + 1 < last) {
                value += " " + toml_table.strip_comment(lines[++index]);
            }
            if (pending.Remove(name, out var replacement) && original.SequenceEqual(replacement)) {
                for (int row = start; row <= index; ++row) { updated.Add(lines[row]); }
                continue;
            }
            if (replacement != null) { updated.Add(code[..(equals + 1)] + " " + array_text(replacement)); }
            for (int row = start; row <= index; ++row) {
                var line = lines[row];
                var without_comment = toml_table.strip_comment(line);
                if (without_comment.Length != line.Length) { updated.Add(line[without_comment.Length..]); }
            }
        }
        updated.AddRange(pending.Select(pair => $"{document_io.quote(pair.Key)} = {array_text(pair.Value)}"));
        lines.RemoveRange(first, last - first);
        lines.InsertRange(first, updated);
        string result = string.Join(newline, lines);
        if (!try_read(result, out var verified, out _) || !same_actions(actions, verified)) {
            error = "The manifest uses a layout the editor could not safely update. Check [input.actions].";
            return false;
        }
        return project_settings_file.try_replace_text(path, source, result, out error);
    }

    private static string array_text(IEnumerable<string> keys) => "[" + string.Join(", ", keys.Select(document_io.quote)) + "]";
    private static bool valid_key(string key) => key.Length == 1 && (key[0] is >= 'a' and <= 'z' or >= '0' and <= '9') ||
                                                  named_keys.ContainsValue(key);

    private static bool same_actions(SortedDictionary<string, List<string>> left, SortedDictionary<string, List<string>> right) =>
        left.Count == right.Count && left.All(pair => right.TryGetValue(pair.Key, out var keys) && pair.Value.SequenceEqual(keys));
}
