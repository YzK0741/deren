using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace deren.editor;

/// <summary>
/// Reads and writes the editor document file.
/// </summary>
/// <remarks>
/// The format is a strict subset of TOML owned by the editor. A file starts with one
/// <c>[document]</c> table holding the <c>version</c> and <c>name</c>, followed by one
/// <c>[[node]]</c> table per node in depth-first order, where every parent appears before its
/// children. Reading rejects a duplicate uid, an unknown or duplicated key, a missing or unknown
/// parent, an unsupported version, an unparsable number and an unterminated string, and reports the
/// file, line and reason. The writer keeps a stable key order, writes one blank line between node
/// blocks and writes floats so that they round-trip exactly.
/// </remarks>
internal static class document_io
{
    /// <summary>
    /// The file format version this writer speaks: 2 added the optional per-node <c>script</c> key.
    /// </summary>
    private const int format_version = 2;

    /// <summary>
    /// The oldest version this reader still accepts.
    /// </summary>
    /// <remarks>
    /// Version 1 files keep loading unchanged - they predate the <c>script</c> key, and a key that is
    /// absent keeps its default - so an old project is not refused by the upgrade. A version above
    /// this writer's is refused, because it may carry keys this parser would drop on the next save.
    /// </remarks>
    private const int oldest_format_version = 1;

    private static readonly UTF8Encoding utf8 = new(encoderShouldEmitUTF8Identifier: false);

    /// <summary>
    /// Writes <paramref name="document"/> to <paramref name="path"/> and marks it saved.
    /// </summary>
    internal static bool try_save(scene_document document, string path, out string? error)
    {
        error = null;
        if (document == null) {
            error = "no document to save";
            return false;
        }
        if (string.IsNullOrEmpty(path)) {
            error = "no path to save the document to";
            return false;
        }

        if (!try_serialize(document, out var text, out error)) {
            return false;
        }
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try {
            File.WriteAllText(temporary, text, utf8);
            File.Move(temporary, path, overwrite: true);
        } catch (Exception exception) when (is_io_error(exception)) {
            error = exception.Message;
            return false;
        } finally {
            try { File.Delete(temporary); } catch (Exception exception) when (is_io_error(exception)) { }
        }
        document.mark_saved(path, text);
        return true;
    }

    internal static bool try_serialize(scene_document document, out string content, out string? error)
    {
        content = string.Empty;
        var text = new StringBuilder();
        text.Append("[document]\n");
        text.Append("    version = ").Append(format_version.ToString(CultureInfo.InvariantCulture)).Append('\n');
        text.Append("    name = ").Append(quote(document.name)).Append('\n');

        var known = new HashSet<Guid>();
        if (!append_nodes(text, document.roots, null, known, out error)) {
            return false;
        }

        content = text.ToString();
        return true;
    }

    /// <summary>
    /// Reads one document file. On failure <paramref name="document"/> is null and
    /// <paramref name="error"/> says what is wrong.
    /// </summary>
    internal static bool try_load(string path, out scene_document? document, out string? error)
    {
        document = null;
        error = null;
        if (string.IsNullOrEmpty(path)) {
            error = "no path to load the document from";
            return false;
        }

        string text;
        try {
            text = File.ReadAllText(path, utf8);
        } catch (Exception exception) when (is_io_error(exception)) {
            error = exception.Message;
            return false;
        }

        if (text.Length > 0 && text[0] == '\uFEFF') {
            text = text[1..];
        }
        text = text.Replace("\r\n", "\n", StringComparison.Ordinal).Replace('\r', '\n');

        var built = new scene_document();
        var uids = new Dictionary<Guid, document_node>();
        var document_keys = new HashSet<string>(StringComparer.Ordinal);
        var document_seen = false;
        var version_seen = false;
        node_builder? current = null;

        var lines = text.Split('\n');
        for (var index = 0; index < lines.Length; ++index) {
            var number = index + 1;
            if (!strip_comment(lines[index], path, number, out var line, out error)) {
                return false;
            }
            var content = line.Trim();
            if (content.Length == 0) {
                continue;
            }
            if (content == "[document]") {
                if (document_seen) {
                    error = format_error(path, number, "[document] appears more than once");
                    return false;
                }
                if (current != null) {
                    error = format_error(path, number, "[document] must come before the node blocks");
                    return false;
                }
                document_seen = true;
                continue;
            }
            if (content == "[[node]]") {
                if (!document_seen) {
                    error = format_error(path, number, "[[node]] before [document]");
                    return false;
                }
                if (current != null && !finish_node(current, built, uids, path, out error)) {
                    return false;
                }
                current = new node_builder { line = number };
                continue;
            }
            if (content[0] == '[') {
                error = format_error(path, number, "unsupported section " + quote(content));
                return false;
            }
            if (!split_assignment(content, path, number, out var key, out var value, out error)) {
                return false;
            }
            if (current != null) {
                if (!assign_node(current, key, value, path, number, out error)) {
                    return false;
                }
                continue;
            }
            if (!document_seen) {
                error = format_error(path, number, "key outside any section");
                return false;
            }
            if (!document_keys.Add(key)) {
                error = format_error(path, number, "duplicate key " + quote(key));
                return false;
            }
            if (key == "version") {
                if (!parse_int(value, path, number, out var version, out error)) {
                    return false;
                }
                if (version < oldest_format_version || version > format_version) {
                    error = format_error(path, number,
                        "unsupported document version " + version.ToString(CultureInfo.InvariantCulture));
                    return false;
                }
                version_seen = true;
                continue;
            }
            if (key == "name") {
                if (!parse_quoted(value, path, number, out var name, out error)) {
                    return false;
                }
                built.name = name;
                continue;
            }
            error = format_error(path, number, "unknown key " + quote(key));
            return false;
        }

        if (current != null && !finish_node(current, built, uids, path, out error)) {
            return false;
        }
        if (!document_seen) {
            error = path + ": missing [document] section";
            return false;
        }
        if (!version_seen) {
            error = path + ": missing version in [document]";
            return false;
        }

        if (!try_serialize(built, out var saved, out error)) {
            return false;
        }
        built.mark_saved(path, saved);
        document = built;
        return true;
    }

    private static bool append_nodes(StringBuilder text, IReadOnlyList<document_node> nodes, Guid? parent,
                                     HashSet<Guid> known, out string? error)
    {
        foreach (var node in nodes) {
            if (!known.Add(node.uid)) {
                error = "duplicate node uid " + quote(node.uid.ToString("D"));
                return false;
            }
            text.Append('\n');
            text.Append("[[node]]\n");
            text.Append("    uid = ").Append(quote(node.uid.ToString("D"))).Append('\n');
            text.Append("    name = ").Append(quote(node.name)).Append('\n');
            if (parent.HasValue) {
                text.Append("    parent = ").Append(quote(parent.Value.ToString("D"))).Append('\n');
            }
            if (node.asset != null) {
                text.Append("    asset = ").Append(quote(node.asset)).Append('\n');
            }
            if (node.pipeline != null) {
                text.Append("    pipeline = ").Append(quote(node.pipeline)).Append('\n');
            }
            if (node.script != null) {
                text.Append("    script = ").Append(quote(node.script)).Append('\n');
            }
            text.Append("    visible = ").Append(node.visible ? "true" : "false").Append('\n');
            text.Append("    flags = ").Append(node.flags.ToString(CultureInfo.InvariantCulture)).Append('\n');
            text.Append("    position = ").Append(format_vector(node.local.position)).Append('\n');
            text.Append("    rotation = ").Append(format_vector(node.local.rotation_degrees)).Append('\n');
            text.Append("    scale = ").Append(format_vector(node.local.scale)).Append('\n');
            if (!append_nodes(text, node.children, node.uid, known, out error)) {
                return false;
            }
        }
        error = null;
        return true;
    }

    private static bool finish_node(node_builder builder, scene_document document,
                                    Dictionary<Guid, document_node> uids, string path, out string? error)
    {
        error = null;
        if (!builder.has_uid) {
            error = format_error(path, builder.line, "node block without a uid");
            return false;
        }
        if (!builder.has_name) {
            error = format_error(path, builder.line, "node block without a name");
            return false;
        }
        if (uids.ContainsKey(builder.uid)) {
            error = format_error(path, builder.line, "duplicate node uid " + quote(builder.uid.ToString("D")));
            return false;
        }

        var node = new document_node {
            uid = builder.uid,
            name = builder.name,
            asset = builder.asset,
            pipeline = builder.pipeline,
            script = builder.script,
            local = builder.local,
            visible = builder.visible,
            flags = builder.flags,
        };
        if (builder.has_parent) {
            if (!uids.TryGetValue(builder.parent, out var parent)) {
                error = format_error(path, builder.line, "unknown parent " + quote(builder.parent.ToString("D")));
                return false;
            }
            parent.children.Add(node);
        } else {
            document.add_root(node);
        }
        uids.Add(node.uid, node);
        return true;
    }

    private static bool assign_node(node_builder builder, string key, string value, string path, int number,
                                    out string? error)
    {
        error = null;
        if (!builder.keys.Add(key)) {
            error = format_error(path, number, "duplicate key " + quote(key));
            return false;
        }
        if (key == "uid") {
            if (!parse_uid(value, path, number, out var uid, out error)) {
                return false;
            }
            builder.uid = uid;
            builder.has_uid = true;
            return true;
        }
        if (key == "name") {
            if (!parse_quoted(value, path, number, out var name, out error)) {
                return false;
            }
            builder.name = name;
            builder.has_name = true;
            return true;
        }
        if (key == "parent") {
            if (!parse_uid(value, path, number, out var parent, out error)) {
                return false;
            }
            builder.parent = parent;
            builder.has_parent = true;
            return true;
        }
        if (key == "asset") {
            if (!parse_quoted(value, path, number, out var asset, out error)) {
                return false;
            }
            builder.asset = asset;
            return true;
        }
        if (key == "script") {
            if (!parse_quoted(value, path, number, out var script, out error)) {
                return false;
            }
            builder.script = string.IsNullOrWhiteSpace(script) ? null : script;
            return true;
        }
        if (key == "pipeline") {
            if (!parse_quoted(value, path, number, out var pipeline, out error)) {
                return false;
            }
            builder.pipeline = pipeline;
            return true;
        }
        if (key == "visible") {
            if (!parse_bool(value, path, number, out var visible, out error)) {
                return false;
            }
            builder.visible = visible;
            return true;
        }
        if (key == "flags") {
            if (!parse_flags(value, path, number, out var flags, out error)) {
                return false;
            }
            builder.flags = flags;
            return true;
        }
        if (key == "position") {
            if (!parse_vector(value, path, number, out var position, out error)) {
                return false;
            }
            builder.local.position = position;
            return true;
        }
        if (key == "rotation") {
            if (!parse_vector(value, path, number, out var rotation, out error)) {
                return false;
            }
            builder.local.rotation_degrees = rotation;
            return true;
        }
        if (key == "scale") {
            if (!parse_vector(value, path, number, out var scale, out error)) {
                return false;
            }
            builder.local.scale = scale;
            return true;
        }
        error = format_error(path, number, "unknown key " + quote(key));
        return false;
    }

    private static bool strip_comment(string line, string path, int number, out string content, out string? error)
    {
        content = line;
        error = null;
        var in_string = false;
        var escaped = false;
        for (var index = 0; index < line.Length; ++index) {
            var character = line[index];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (in_string) {
                if (character == '\\') {
                    escaped = true;
                } else if (character == '"') {
                    in_string = false;
                }
                continue;
            }
            if (character == '"') {
                in_string = true;
                continue;
            }
            if (character == '#') {
                content = line[..index];
                return true;
            }
        }
        if (in_string) {
            error = format_error(path, number, "unterminated string");
            return false;
        }
        return true;
    }

    private static bool split_assignment(string content, string path, int number, out string key, out string value,
                                         out string? error)
    {
        key = string.Empty;
        value = string.Empty;
        error = null;
        var in_string = false;
        var escaped = false;
        for (var index = 0; index < content.Length; ++index) {
            var character = content[index];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (in_string) {
                if (character == '\\') {
                    escaped = true;
                } else if (character == '"') {
                    in_string = false;
                }
                continue;
            }
            if (character == '"') {
                in_string = true;
                continue;
            }
            if (character == '=') {
                key = content[..index].Trim();
                value = content[(index + 1)..].Trim();
                if (key.Length == 0) {
                    error = format_error(path, number, "missing key before '='");
                    return false;
                }
                return true;
            }
        }
        error = format_error(path, number, "expected 'key = value'");
        return false;
    }

    private static bool parse_quoted(string text, string path, int number, out string result, out string? error)
    {
        result = string.Empty;
        error = null;
        if (text.Length < 2 || text[0] != '"' || text[^1] != '"') {
            error = format_error(path, number, "expected a quoted string");
            return false;
        }
        var built = new StringBuilder(text.Length);
        var end = text.Length - 1;
        for (var index = 1; index < end; ++index) {
            var character = text[index];
            if (character == '"') {
                error = format_error(path, number, "unescaped quote inside a string");
                return false;
            }
            if (character != '\\') {
                built.Append(character);
                continue;
            }
            ++index;
            if (index >= end) {
                error = format_error(path, number, "unfinished escape inside a string");
                return false;
            }
            switch (text[index]) {
                case '\\':
                    built.Append('\\');
                    break;
                case '"':
                    built.Append('"');
                    break;
                case 'n':
                    built.Append('\n');
                    break;
                case 'r':
                    built.Append('\r');
                    break;
                case 't':
                    built.Append('\t');
                    break;
                case 'b':
                    built.Append('\b');
                    break;
                case 'f':
                    built.Append('\f');
                    break;
                case 'u': {
                    if (index + 4 >= end) {
                        error = format_error(path, number, "short unicode escape inside a string");
                        return false;
                    }
                    if (!ushort.TryParse(text.AsSpan(index + 1, 4), NumberStyles.HexNumber,
                                         CultureInfo.InvariantCulture, out var code)) {
                        error = format_error(path, number, "bad unicode escape inside a string");
                        return false;
                    }
                    built.Append((char)code);
                    index += 4;
                    break;
                }
                default:
                    error = format_error(path, number, "unknown escape inside a string");
                    return false;
            }
        }
        result = built.ToString();
        return true;
    }

    private static bool parse_uid(string text, string path, int number, out Guid value, out string? error)
    {
        value = Guid.Empty;
        if (!parse_quoted(text, path, number, out var raw, out error)) {
            return false;
        }
        if (!Guid.TryParseExact(raw, "D", out value)
            || !string.Equals(raw, value.ToString("D"), StringComparison.Ordinal)) {
            error = format_error(path, number, "expected a lowercase uuid");
            return false;
        }
        return true;
    }

    private static bool parse_bool(string text, string path, int number, out bool value, out string? error)
    {
        value = false;
        error = null;
        if (string.Equals(text, "true", StringComparison.Ordinal)) {
            value = true;
            return true;
        }
        if (string.Equals(text, "false", StringComparison.Ordinal)) {
            return true;
        }
        error = format_error(path, number, "expected true or false");
        return false;
    }

    private static bool parse_vector(string text, string path, int number, out deren.vector_3 value, out string? error)
    {
        value = default;
        error = null;
        if (text.Length < 2 || text[0] != '[' || text[^1] != ']') {
            error = format_error(path, number, "expected a vector like [x, y, z]");
            return false;
        }
        var parts = text[1..^1].Split(',');
        if (parts.Length != 3) {
            error = format_error(path, number, "expected three numbers in a vector");
            return false;
        }
        if (!parse_float(parts[0], path, number, out var x, out error)
            || !parse_float(parts[1], path, number, out var y, out error)
            || !parse_float(parts[2], path, number, out var z, out error)) {
            return false;
        }
        value = new deren.vector_3 { x = x, y = y, z = z };
        return true;
    }

    private static bool parse_float(string text, string path, int number, out float value, out string? error)
    {
        error = null;
        if (!float.TryParse(text.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out value)) {
            error = format_error(path, number, "unparsable number " + quote(text.Trim()));
            return false;
        }
        return true;
    }

    private static bool parse_int(string text, string path, int number, out int value, out string? error)
    {
        error = null;
        if (!int.TryParse(text.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out value)) {
            error = format_error(path, number, "unparsable number " + quote(text.Trim()));
            return false;
        }
        return true;
    }

    private static bool parse_flags(string text, string path, int number, out uint value, out string? error)
    {
        error = null;
        if (!uint.TryParse(text.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out value)) {
            error = format_error(path, number, "unparsable flags " + quote(text.Trim()));
            return false;
        }
        return true;
    }

    private static string format_vector(deren.vector_3 value)
    {
        return "[" + format_float(value.x) + ", " + format_float(value.y) + ", " + format_float(value.z) + "]";
    }

    private static string format_float(float value)
    {
        return value.ToString("R", CultureInfo.InvariantCulture);
    }

    private static string quote(string text)
    {
        var built = new StringBuilder(text.Length + 2);
        built.Append('"');
        foreach (var character in text) {
            switch (character) {
                case '\\':
                    built.Append("\\\\");
                    break;
                case '"':
                    built.Append("\\\"");
                    break;
                case '\n':
                    built.Append("\\n");
                    break;
                case '\r':
                    built.Append("\\r");
                    break;
                case '\t':
                    built.Append("\\t");
                    break;
                case '\b':
                    built.Append("\\b");
                    break;
                case '\f':
                    built.Append("\\f");
                    break;
                default:
                    if (character < ' ') {
                        built.Append("\\u").Append(((int)character).ToString("x4", CultureInfo.InvariantCulture));
                    } else {
                        built.Append(character);
                    }
                    break;
            }
        }
        built.Append('"');
        return built.ToString();
    }

    private static string format_error(string path, int number, string message)
    {
        return path + ": line " + number.ToString(CultureInfo.InvariantCulture) + ": " + message;
    }

    private static bool is_io_error(Exception exception)
    {
        return exception is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException;
    }

    /// <summary>
    /// The keys of one <c>[[node]]</c> block while it is being read.
    /// </summary>
    private sealed class node_builder
    {
        internal int line;
        internal readonly HashSet<string> keys = [];
        internal Guid uid;
        internal bool has_uid;
        internal string name = string.Empty;
        internal bool has_name;
        internal Guid parent;
        internal bool has_parent;
        internal string? asset;
        internal string? pipeline;
        internal string? script;
        internal bool visible = true;
        internal uint flags;
        internal deren.transform_value local = new() {
            scale = new deren.vector_3 { x = 1.0f, y = 1.0f, z = 1.0f },
        };
    }
}
