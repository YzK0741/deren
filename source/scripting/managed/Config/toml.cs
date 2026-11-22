using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;

namespace deren.scripting {
    /// <summary>
    /// The subset of TOML the project files use, parsed by the managed side so a project's
    /// presentation settings no longer have to travel through the native host.
    ///
    /// Supported: tables (`[a.b]`), arrays of tables (`[[a]]`), dotted keys inside a table, basic
    /// strings with the usual escapes, literal strings, integers, floats, booleans and arrays of
    /// those (single- or multi-line). Everything else - dates, multi-line strings, inline tables -
    /// is reported through `error` and skipped; the caller keeps whatever did parse.
    /// </summary>
    internal sealed class toml_table {
        private readonly Dictionary<string, object> values = new(StringComparer.Ordinal);

        internal IEnumerable<string> keys => values.Keys;

        internal toml_table? table(string key) {
            return values.TryGetValue(key, out var value) ? value as toml_table : null;
        }

        internal string? text(string key) {
            return values.TryGetValue(key, out var value) ? value as string : null;
        }

        internal bool? flag(string key) {
            return values.TryGetValue(key, out var value) && value is bool flag ? flag : null;
        }

        internal float? number(string key) {
            if (!values.TryGetValue(key, out var value)) {
                return null;
            }
            var result = value switch {
                long integer => (float)integer,
                double real => (float)real,
                _ => float.NaN
            };
            return float.IsFinite(result) ? result : null;
        }

        internal int? integer(string key) {
            if (!values.TryGetValue(key, out var value) || value is not long integer) {
                return null;
            }
            return integer is >= int.MinValue and <= int.MaxValue ? (int)integer : null;
        }

        internal List<float>? numbers(string key) {
            if (!values.TryGetValue(key, out var value) || value is not List<object> items) {
                return null;
            }
            var result = new List<float>(items.Count);
            foreach (var item in items) {
                var number = item switch {
                    long integer => (float)integer,
                    double real => (float)real,
                    _ => float.NaN
                };
                if (!float.IsFinite(number)) {
                    return null;
                }
                result.Add(number);
            }
            return result;
        }

        internal List<string>? strings(string key) {
            if (!values.TryGetValue(key, out var value) || value is not List<object> items) {
                return null;
            }
            var result = new List<string>(items.Count);
            foreach (var item in items) {
                if (item is not string text) {
                    return null;
                }
                result.Add(text);
            }
            return result;
        }

        /// <summary>
        /// The <c>[[name]]</c> array-of-tables entry. Null when the key is absent or holds anything
        /// else, so a caller can tell "no such list" from "an empty list".
        /// </summary>
        internal List<toml_table>? tables(string key) {
            if (!values.TryGetValue(key, out var value) || value is not List<object> items) {
                return null;
            }
            var result = new List<toml_table>(items.Count);
            foreach (var item in items) {
                if (item is not toml_table entry) {
                    return null;
                }
                result.Add(entry);
            }
            return result;
        }

        /// <summary>Parses a document; returns null only when nothing could be read at all.</summary>
        internal static toml_table? parse(string document, out string? error) {
            var root = new toml_table();
            var current = root;
            error = null;
            var lines = document.Replace("\r\n", "\n").Replace('\r', '\n').Split('\n');
            for (var index = 0; index < lines.Length; ++index) {
                var line = strip_comment(lines[index]).Trim();
                if (line.Length == 0) {
                    continue;
                }
                if (line[0] == '[') {
                    current = header(root, line, index, ref error);
                    continue;
                }
                var equals = find_unquoted(line, '=');
                if (equals < 0) {
                    error ??= $"line {index + 1}: expected 'key = value'";
                    continue;
                }
                var key = split_key(line[..equals].Trim());
                if (key == null) {
                    error ??= $"line {index + 1}: unsupported key '{line[..equals].Trim()}'";
                    continue;
                }
                var parsed = parse_value(line[(equals + 1)..].Trim(), lines, ref index, out error);
                if (parsed == null) {
                    continue;
                }
                assign(current, key, parsed, index + 1, ref error);
            }
            return root;
        }

        private static toml_table header(toml_table root, string line, int index, ref string? error) {
            var is_array = line.StartsWith("[[", StringComparison.Ordinal);
            var closing = is_array ? "]]" : "]";
            if (!line.EndsWith(closing, StringComparison.Ordinal)) {
                error ??= $"line {index + 1}: unterminated table header";
                return root;
            }
            var name = line[(is_array ? 2 : 1)..^closing.Length].Trim();
            var parts = split_key(name);
            if (parts == null) {
                error ??= $"line {index + 1}: unsupported table name '{name}'";
                return root;
            }
            if (!is_array) {
                return ensure(root, parts, index + 1, ref error);
            }
            var parent = ensure(root, parts[..^1], index + 1, ref error);
            var leaf = parts[^1];
            if (parent.values.TryGetValue(leaf, out var existing)) {
                if (existing is List<object> array) {
                    var created = new toml_table();
                    array.Add(created);
                    return created;
                }
                error ??= $"line {index + 1}: '{leaf}' is already a value, not an array of tables";
                return root;
            }
            var items = new List<object>();
            var table = new toml_table();
            items.Add(table);
            parent.values[leaf] = items;
            return table;
        }

        private static toml_table ensure(toml_table table, string[] parts, int line, ref string? error) {
            foreach (var part in parts) {
                if (table.values.TryGetValue(part, out var existing)) {
                    if (existing is toml_table nested) {
                        table = nested;
                        continue;
                    }
                    error ??= $"line {line}: '{part}' is already a value, not a table";
                    return table;
                }
                var created = new toml_table();
                table.values[part] = created;
                table = created;
            }
            return table;
        }

        private static void assign(toml_table table, string[] parts, object value, int line, ref string? error) {
            var parent = ensure(table, parts[..^1], line, ref error);
            parent.values[parts[^1]] = value;
        }

        private static object? parse_value(string text, string[] lines, ref int index, out string? error) {
            error = null;
            if (text.Length == 0) {
                error = $"line {index + 1}: missing value";
                return null;
            }
            var balanced = text;
            while (needs_more(balanced)) {
                if (++index >= lines.Length) {
                    error = "unterminated value";
                    return null;
                }
                balanced += " " + strip_comment(lines[index]).Trim();
            }
            return scalar(balanced, ref error);
        }

        /// <summary>True while an array or inline table opened but not closed (outside strings).</summary>
        internal static bool needs_more(string text) {
            var depth = 0;
            var quote = '\0';
            for (var i = 0; i < text.Length; ++i) {
                var c = text[i];
                if (quote != '\0') {
                    if (c == '\\' && quote == '"') {
                        ++i;
                    } else if (c == quote) {
                        quote = '\0';
                    }
                    continue;
                }
                switch (c) {
                    case '"' or '\'':
                        quote = c;
                        break;
                    case '[' or '{':
                        ++depth;
                        break;
                    case ']' or '}':
                        --depth;
                        break;
                }
            }
            return depth > 0;
        }

        private static object? scalar(string text, ref string? error) {
            text = text.Trim();
            if (text.Length == 0) {
                error ??= "missing value";
                return null;
            }
            if (text.StartsWith('"')) {
                return basic_string(text, ref error);
            }
            if (text.StartsWith('\'')) {
                return literal_string(text, ref error);
            }
            if (text.StartsWith('[')) {
                return array(text, ref error);
            }
            if (text.Equals("true", StringComparison.Ordinal)) {
                return true;
            }
            if (text.Equals("false", StringComparison.Ordinal)) {
                return false;
            }
            if (text.StartsWith('{')) {
                error ??= "inline tables are not supported";
                return null;
            }
            var numeric = text.Replace("_", string.Empty, StringComparison.Ordinal);
            switch (numeric)
            {
                case "inf" or "+inf":
                    return double.PositiveInfinity;
                case "-inf":
                    return double.NegativeInfinity;
                case "nan" or "+nan" or "-nan":
                    return double.NaN;
            }

            if (long.TryParse(numeric, NumberStyles.Integer, CultureInfo.InvariantCulture, out var integer)) {
                return integer;
            }
            if (double.TryParse(numeric, NumberStyles.Float, CultureInfo.InvariantCulture, out var real)) {
                return real;
            }
            error ??= $"unsupported value '{text}'";
            return null;
        }

        private static string? basic_string(string text, ref string? error) {
            var builder = new StringBuilder(text.Length);
            for (var i = 1; i < text.Length; ++i) {
                var c = text[i];
                if (c == '"') {
                    return builder.ToString();
                }
                if (c != '\\') {
                    builder.Append(c);
                    continue;
                }
                if (++i >= text.Length) {
                    break;
                }
                switch (text[i]) {
                    case 'n':
                        builder.Append('\n');
                        break;
                    case 't':
                        builder.Append('\t');
                        break;
                    case 'r':
                        builder.Append('\r');
                        break;
                    case 'b':
                        builder.Append('\b');
                        break;
                    case 'f':
                        builder.Append('\f');
                        break;
                    case '"':
                        builder.Append('"');
                        break;
                    case '\\':
                        builder.Append('\\');
                        break;
                    case 'u' when i + 4 < text.Length:
                        if (int.TryParse(text.AsSpan(i + 1, 4), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var code)) {
                            builder.Append((char)code);
                            i += 4;
                        } else {
                            error ??= $"unsupported escape '\\{text[i]}'";
                        }
                        break;
                    default:
                        error ??= $"unsupported escape '\\{text[i]}'";
                        break;
                }
            }
            error ??= "unterminated string";
            return null;
        }

        private static string? literal_string(string text, ref string? error) {
            var end = text.IndexOf('\'', 1);
            if (end >= 0) {
                return text[1..end];
            }

            error ??= "unterminated literal string";
            return null;
        }

        private static List<object>? array(string text, ref string? error) {
            var items = new List<object>();
            var inner = text.Trim();
            if (!inner.EndsWith(']')) {
                error ??= "unterminated array";
                return null;
            }
            inner = inner[1..^1];
            foreach (var element in split_top_level(inner)) {
                var item = scalar(element.Trim(), ref error);
                if (item == null) {
                    return null;
                }
                items.Add(item);
            }
            return items;
        }

        private static List<string> split_top_level(string text) {
            var parts = new List<string>();
            var depth = 0;
            var quote = '\0';
            var start = 0;
            for (var i = 0; i < text.Length; ++i) {
                var c = text[i];
                if (quote != '\0') {
                    if (c == '\\' && quote == '"') {
                        ++i;
                    } else if (c == quote) {
                        quote = '\0';
                    }
                    continue;
                }
                switch (c) {
                    case '"' or '\'':
                        quote = c;
                        break;
                    case '[' or '{':
                        ++depth;
                        break;
                    case ']' or '}':
                        --depth;
                        break;
                    case ',' when depth == 0:
                        parts.Add(text[start..i]);
                        start = i + 1;
                        break;
                }
            }
            if (start < text.Length) {
                parts.Add(text[start..]);
            }
            return parts;
        }

        internal static string[]? split_key(string text) {
            if (text.Length == 0) {
                return null;
            }
            var parts = new List<string>();
            var start = 0;
            var quote = '\0';
            for (var i = 0; i < text.Length; ++i) {
                var c = text[i];
                if (quote != '\0') {
                    if (c == '\\' && quote == '"') {
                        ++i;
                    } else if (c == quote) {
                        quote = '\0';
                    }
                    continue;
                }
                switch (c) {
                    case '"' or '\'':
                        quote = c;
                        break;
                    case '.':
                        parts.Add(text[start..i]);
                        start = i + 1;
                        break;
                }
            }
            parts.Add(text[start..]);
            var keys = new string[parts.Count];
            for (var i = 0; i < parts.Count; ++i) {
                var part = parts[i].Trim();
                if (part.Length == 0) {
                    return null;
                }
                string? parse_error = null;
                string? key = part[0] switch {
                    '"' when part.Length > 1 && part[^1] == '"' => basic_string(part, ref parse_error),
                    '\'' when part.Length > 1 && part[^1] == '\'' => literal_string(part, ref parse_error),
                    _ when !part.Contains('"', StringComparison.Ordinal) &&
                           !part.Contains('\'', StringComparison.Ordinal) => part,
                    _ => null,
                };
                if (key == null || parse_error != null) {
                    return null;
                }
                keys[i] = key;
            }
            return keys;
        }

        internal static int find_unquoted(string text, char needle) {
            var quote = '\0';
            for (var i = 0; i < text.Length; ++i) {
                var c = text[i];
                if (quote != '\0') {
                    if (c == '\\' && quote == '"') {
                        ++i;
                    } else if (c == quote) {
                        quote = '\0';
                    }
                    continue;
                }
                if (c is '"' or '\'') {
                    quote = c;
                } else if (c == needle) {
                    return i;
                }
            }
            return -1;
        }

        internal static string strip_comment(string line) {
            var quote = '\0';
            for (var i = 0; i < line.Length; ++i) {
                var c = line[i];
                if (quote != '\0') {
                    if (c == '\\' && quote == '"') {
                        ++i;
                    } else if (c == quote) {
                        quote = '\0';
                    }
                    continue;
                }
                switch (c)
                {
                    case '"' or '\'':
                        quote = c;
                        break;
                    case '#':
                        return line[..i];
                }
            }
            return line;
        }
    }
}
