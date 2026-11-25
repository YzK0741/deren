using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;

namespace deren.editor;

/// <summary>
/// One animated property of a motion clip: the glTF channel's target node, its path and its keys.
/// The keys are copied out of the buffer so the timeline can draw them without holding the file open.
/// </summary>
internal sealed class motion_key_channel {
    internal int node_index;
    internal string node_name = string.Empty;
    internal string path = string.Empty;
    internal string interpolation = "LINEAR";
    /// <summary>Floats per key: 3 for translation/scale, 4 for rotation.</summary>
    internal int components;
    internal int accessor_index;
    /// <summary>
    /// The key-time accessor. glTF exporters often share one input accessor across every channel of a
    /// node, so channels with the same index must be edited together: their key times are one array.
    /// </summary>
    internal int time_accessor_index;
    internal int buffer_view_index;
    internal int byte_offset;
    /// <summary>The byte distance between two keys' value blocks inside the motion buffer.</summary>
    internal int value_stride;
    /// <summary>Where the first key time sits in the motion buffer, and the stride after it.</summary>
    internal int time_byte_offset;
    internal int time_stride;
    internal float[] times = [];
    /// <summary>Key values, `components` floats per key, in glTF order (quaternion x, y, z, w).</summary>
    internal float[] values = [];

    internal int key_count => times.Length;

    internal float start_time => times.Length == 0 ? 0 : times[0];

    internal float end_time => times.Length == 0 ? 0 : times[^1];

    internal string describe() => $"{node_name} / {path}";
}

/// <summary>
/// The parts of a `motion.gltf` the animation editor authors: one clip's channels with their key times
/// and values. The buffer bytes stay in memory so the next slice can patch edited keys back into the
/// same file (glTF keeps animation data in the buffer, not in the JSON text).
/// </summary>
internal sealed class motion_document {
    private readonly record struct buffer_view(int buffer, int byte_offset, int byte_length, int byte_stride);
    private readonly record struct accessor(int buffer_view, int byte_offset, int component_type, int count, int components);

    internal string path = string.Empty;
    /// <summary>The external buffer (`motion.bin`), or null when the bytes live in a data URI.</summary>
    internal string? buffer_path;
    internal byte[] buffer = [];
    internal string clip_name = string.Empty;
    /// <summary>The clip name as it sits in the JSON text, so the writer knows when it has to patch.</summary>
    internal string file_clip_name = string.Empty;
    internal float duration;
    internal readonly List<motion_key_channel> channels = [];
    internal bool dirty;

    internal int total_keys {
        get {
            var keys = 0;
            foreach (var channel in channels) {
                keys += channel.key_count;
            }
            return keys;
        }
    }

    internal motion_key_channel? find(int index) =>
        index >= 0 && index < channels.Count ? channels[index] : null;

    /// <summary>
    /// Loads the first animation of a glTF motion file. Only the layouts deren's converter writes are
    /// supported: one external buffer or a base64 data URI, float accessors, and optional byte strides.
    /// </summary>
    internal static bool try_load(string path, out motion_document? document, out string error) {
        document = null;
        error = string.Empty;
        if (!File.Exists(path)) {
            error = $"'{path}' does not exist";
            return false;
        }
        JsonDocument json;
        try {
            json = JsonDocument.Parse(File.ReadAllBytes(path),
                new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip });
        } catch (JsonException parse_error) {
            error = $"the motion file is not valid glTF JSON: {parse_error.Message}";
            return false;
        }
        using (json) {
            var root = json.RootElement;
            var result = new motion_document { path = path };
            if (!result.read_buffer(root, out error)) {
                return false;
            }
            var views = read_views(root, out error);
            if (views == null) {
                return false;
            }
            var accessors = read_accessors(root, out error);
            if (accessors == null) {
                return false;
            }
            if (!root.TryGetProperty("animations", out var animations) ||
                animations.ValueKind != JsonValueKind.Array || animations.GetArrayLength() == 0) {
                error = "the motion file carries no animation";
                return false;
            }
            var animation = animations[0];
            result.clip_name = animation.TryGetProperty("name", out var name) && name.ValueKind == JsonValueKind.String
                ? name.GetString() ?? "Clip"
                : "Clip 1";
            result.file_clip_name = result.clip_name;
            if (!animation.TryGetProperty("samplers", out var samplers) ||
                samplers.ValueKind != JsonValueKind.Array ||
                !animation.TryGetProperty("channels", out var channel_list) ||
                channel_list.ValueKind != JsonValueKind.Array) {
                error = "the first animation has no samplers or channels";
                return false;
            }
            var nodes = root.TryGetProperty("nodes", out var node_list) && node_list.ValueKind == JsonValueKind.Array
                ? node_list : default;
            for (var i = 0; i < channel_list.GetArrayLength(); ++i) {
                if (!result.read_channel(channel_list[i], samplers, nodes, views, accessors, out var channel, out error)) {
                    return false;
                }
                if (channel == null) {
                    continue;
                }
                result.channels.Add(channel);
                if (channel.end_time > result.duration) {
                    result.duration = channel.end_time;
                }
            }
            if (result.channels.Count == 0) {
                error = "the motion file's first animation has no drawable channels";
                return false;
            }
            document = result;
            return true;
        }
    }

    private bool read_buffer(JsonElement root, out string error) {
        error = string.Empty;
        if (!root.TryGetProperty("buffers", out var buffers) || buffers.ValueKind != JsonValueKind.Array ||
            buffers.GetArrayLength() == 0) {
            error = "the motion file has no buffer";
            return false;
        }
        var first = buffers[0];
        if (!first.TryGetProperty("uri", out var uri) || uri.ValueKind != JsonValueKind.String) {
            error = "the motion file's buffer has no uri";
            return false;
        }
        var text = uri.GetString() ?? string.Empty;
        var base64 = "base64,";
        if (text.StartsWith("data:", StringComparison.OrdinalIgnoreCase)) {
            var comma = text.IndexOf(base64, StringComparison.OrdinalIgnoreCase);
            if (comma < 0) {
                error = "the motion file's buffer data URI is not base64";
                return false;
            }
            try {
                buffer = Convert.FromBase64String(text[(comma + base64.Length)..]);
            } catch (FormatException) {
                error = "the motion file's base64 buffer is malformed";
                return false;
            }
            buffer_path = null;
            return true;
        }
        try {
            text = Uri.UnescapeDataString(text);
        } catch (UriFormatException) {
            // Not percent-encoded; the raw name is used as is.
        }
        var directory = Path.GetDirectoryName(Path.GetFullPath(path)) ?? ".";
        var resolved = Path.GetFullPath(Path.Combine(directory, text));
        if (!File.Exists(resolved)) {
            error = $"the motion file's buffer '{text}' does not exist";
            return false;
        }
        buffer = File.ReadAllBytes(resolved);
        buffer_path = resolved;
        return true;
    }

    private static List<buffer_view>? read_views(JsonElement root, out string error) {
        error = string.Empty;
        var views = new List<buffer_view>();
        if (!root.TryGetProperty("bufferViews", out var list) || list.ValueKind != JsonValueKind.Array) {
            error = "the motion file has no buffer views";
            return null;
        }
        for (var i = 0; i < list.GetArrayLength(); ++i) {
            var item = list[i];
            views.Add(new buffer_view(
                item.TryGetProperty("buffer", out var b) ? b.GetInt32() : 0,
                item.TryGetProperty("byteOffset", out var o) ? o.GetInt32() : 0,
                item.TryGetProperty("byteLength", out var l) ? l.GetInt32() : 0,
                item.TryGetProperty("byteStride", out var s) ? s.GetInt32() : 0));
        }
        return views;
    }

    private static List<accessor>? read_accessors(JsonElement root, out string error) {
        error = string.Empty;
        var accessors = new List<accessor>();
        if (!root.TryGetProperty("accessors", out var list) || list.ValueKind != JsonValueKind.Array) {
            error = "the motion file has no accessors";
            return null;
        }
        for (var i = 0; i < list.GetArrayLength(); ++i) {
            var item = list[i];
            var type = item.TryGetProperty("type", out var t) && t.ValueKind == JsonValueKind.String
                ? t.GetString() : null;
            var components = type switch {
                "SCALAR" => 1,
                "VEC2" => 2,
                "VEC3" => 3,
                "VEC4" => 4,
                "MAT4" => 16,
                _ => 0,
            };
            if (components == 0) {
                error = $"accessor {i} has an unsupported type '{type}'";
                return null;
            }
            accessors.Add(new accessor(
                item.TryGetProperty("bufferView", out var v) ? v.GetInt32() : -1,
                item.TryGetProperty("byteOffset", out var o) ? o.GetInt32() : 0,
                item.TryGetProperty("componentType", out var c) ? c.GetInt32() : 0,
                item.TryGetProperty("count", out var n) ? n.GetInt32() : 0,
                components));
        }
        return accessors;
    }

    private bool read_channel(JsonElement channel, JsonElement samplers, JsonElement nodes,
                              List<buffer_view> views, List<accessor> accessors,
                              out motion_key_channel? result, out string error) {
        result = null;
        error = string.Empty;
        if (!channel.TryGetProperty("sampler", out var sampler_value) || sampler_value.ValueKind != JsonValueKind.Number) {
            return true; // A channel without a sampler animates nothing.
        }
        var sampler_index = sampler_value.GetInt32();
        if (sampler_index < 0 || sampler_index >= samplers.GetArrayLength()) {
            error = $"a channel points at sampler {sampler_index}, which does not exist";
            return false;
        }
        var sampler = samplers[sampler_index];
        if (!sampler.TryGetProperty("input", out var input) || !sampler.TryGetProperty("output", out var output)) {
            error = "a sampler has no input or output accessor";
            return false;
        }
        var entry = new motion_key_channel {
            interpolation = sampler.TryGetProperty("interpolation", out var interpolation) &&
                            interpolation.ValueKind == JsonValueKind.String
                ? interpolation.GetString() ?? "LINEAR"
                : "LINEAR",
        };
        if (channel.TryGetProperty("target", out var target) && target.ValueKind == JsonValueKind.Object) {
            entry.path = target.TryGetProperty("path", out var target_path) &&
                         target_path.ValueKind == JsonValueKind.String
                ? target_path.GetString() ?? string.Empty
                : string.Empty;
            if (target.TryGetProperty("node", out var node) && node.ValueKind == JsonValueKind.Number) {
                entry.node_index = node.GetInt32();
                if (nodes.ValueKind == JsonValueKind.Array && entry.node_index >= 0 &&
                    entry.node_index < nodes.GetArrayLength() &&
                    nodes[entry.node_index].TryGetProperty("name", out var node_name) &&
                    node_name.ValueKind == JsonValueKind.String) {
                    entry.node_name = node_name.GetString() ?? string.Empty;
                }
            }
        }
        if (entry.path.Length == 0) {
            return true; // No target path: glTF allows it, the timeline has nothing to show.
        }
        if (entry.node_name.Length == 0) {
            entry.node_name = $"Node {entry.node_index.ToString(CultureInfo.InvariantCulture)}";
        }
        if (!try_read_floats(input.GetInt32(), views, accessors, out entry.times, out var time_components,
                             out var time_view, out var time_offset, out var time_stride, out error) ||
            !try_read_floats(output.GetInt32(), views, accessors, out entry.values, out entry.components,
                             out entry.buffer_view_index, out entry.byte_offset, out entry.value_stride, out error)) {
            return false;
        }
        entry.time_byte_offset = time_offset;
        entry.time_stride = time_stride;
        _ = time_view;
        if (time_components != 1) {
            error = $"channel '{entry.describe()}' has a key-time accessor that is not a scalar";
            return false;
        }
        entry.accessor_index = output.GetInt32();
        entry.time_accessor_index = input.GetInt32();
        var key_count = entry.values.Length / entry.components;
        if (entry.interpolation == "CUBICSPLINE") {
            key_count /= 3; // in-tangent, value, out-tangent
        }
        if (key_count != entry.times.Length) {
            error = $"channel '{entry.describe()}' has {entry.times.Length} key times but {key_count} values";
            return false;
        }
        result = entry;
        return true;
    }

    private bool try_read_floats(int accessor_index, List<buffer_view> views, List<accessor> accessors,
                                 out float[] values, out int components, out int view_index, out int byte_offset, out int stride, out string error) {
        values = [];
        components = 0;
        view_index = 0;
        byte_offset = 0;
        stride = 0;
        error = string.Empty;
        if (accessor_index < 0 || accessor_index >= accessors.Count) {
            error = $"accessor {accessor_index} does not exist";
            return false;
        }
        var entry = accessors[accessor_index];
        if (entry.component_type != 5126) {
            error = $"accessor {accessor_index} does not hold floats";
            return false;
        }
        if (entry.buffer_view < 0 || entry.buffer_view >= views.Count) {
            error = $"accessor {accessor_index} has no buffer view";
            return false;
        }
        var view = views[entry.buffer_view];
        if (view.buffer != 0) {
            error = "only the motion file's first buffer is supported";
            return false;
        }
        var key_stride = view.byte_stride > 0 ? view.byte_stride : entry.components * sizeof(float);
        var start = view.byte_offset + entry.byte_offset;
        var last = start + key_stride * (entry.count - 1) + entry.components * sizeof(float);
        if (entry.count < 0 || last > buffer.Length || view.byte_offset + view.byte_length > buffer.Length) {
            error = $"accessor {accessor_index} reads past the end of the motion buffer";
            return false;
        }
        values = new float[entry.count * entry.components];
        for (var key = 0; key < entry.count; ++key) {
            for (var part = 0; part < entry.components; ++part) {
                var offset = start + key * key_stride + part * sizeof(float);
                values[key * entry.components + part] =
                    BinaryPrimitives.ReadSingleLittleEndian(buffer.AsSpan(offset, sizeof(float)));
            }
        }
        components = entry.components;
        view_index = entry.buffer_view;
        byte_offset = start;
        stride = key_stride;
        return true;
    }

    /// <summary>
    /// Repacks one channel's keys in time order, moving each value block together with its key time.
    /// A dragged key may pass a neighbour, and glTF requires sampler inputs to be increasing.
    /// </summary>
    internal bool sort_channel(motion_key_channel channel, out string error) {
        error = string.Empty;
        int blocks = channel.interpolation == "CUBICSPLINE" ? 3 : 1;
        int per_key = channel.components * blocks;
        int count = channel.times.Length;
        var order = new int[count];
        for (var key = 0; key < count; ++key) {
            order[key] = key;
        }
        Array.Sort((float[])channel.times.Clone(), order);
        var sorted = true;
        for (var key = 0; key < count; ++key) {
            if (order[key] != key) {
                sorted = false;
                break;
            }
        }
        if (sorted) {
            return true;
        }
        var times = new float[count];
        var values = new float[count * per_key];
        for (var key = 0; key < count; ++key) {
            int source = order[key];
            times[key] = channel.times[source];
            Array.Copy(channel.values, source * per_key, values, key * per_key, per_key);
        }
        channel.times = times;
        channel.values = values;
        return this.write_channel(channel, out error);
    }

    /// <summary>
    /// Packs one channel's in-memory keys back into the buffer bytes, both times and values. A key
    /// occupies one element for LINEAR and STEP, and three for CUBICSPLINE (in-tangent, value,
    /// out-tangent), so the element index is `key * blocks + block`: writing a whole key as one
    /// contiguous run would land the second key on top of the first key's tangent pair.
    /// </summary>
    internal bool write_channel(motion_key_channel channel, out string error) {
        error = string.Empty;
        int blocks = channel.interpolation == "CUBICSPLINE" ? 3 : 1;
        int per_key = channel.components * blocks;
        int count = channel.times.Length;
        if (count == 0) {
            return true;
        }
        if (channel.values.Length < count * per_key) {
            error = $"channel '{channel.describe()}' holds {channel.values.Length} floats for {count} keys";
            return false;
        }
        long time_end = (long)channel.time_byte_offset + (long)channel.time_stride * (count - 1) + sizeof(float);
        long value_end = (long)channel.byte_offset + (long)channel.value_stride * (count * blocks - 1) +
                         channel.components * sizeof(float);
        if (time_end > this.buffer.Length || value_end > this.buffer.Length) {
            error = $"channel '{channel.describe()}' would write past the end of the motion buffer";
            return false;
        }
        for (var key = 0; key < count; ++key) {
            BinaryPrimitives.WriteSingleLittleEndian(
                this.buffer.AsSpan(channel.time_byte_offset + key * channel.time_stride, sizeof(float)),
                channel.times[key]);
            for (var block = 0; block < blocks; ++block) {
                int element = channel.byte_offset + (key * blocks + block) * channel.value_stride;
                for (var part = 0; part < channel.components; ++part) {
                    BinaryPrimitives.WriteSingleLittleEndian(
                        this.buffer.AsSpan(element + part * sizeof(float), sizeof(float)),
                        channel.values[key * per_key + block * channel.components + part]);
                }
            }
        }
        return true;
    }

    /// <summary>
    /// glTF requires a sampler's input accessor to be strictly increasing, so a channel with two keys
    /// at the same time is not a file any tool should accept. Writing is refused rather than writing
    /// something the loader would misread.
    /// </summary>
    internal static bool has_strict_times(motion_key_channel channel, out string error) {
        error = string.Empty;
        for (var key = 1; key < channel.times.Length; ++key) {
            if (!(channel.times[key] > channel.times[key - 1])) {
                error = string.Create(CultureInfo.InvariantCulture,
                                      $"channel '{channel.describe()}' has keys at {channel.times[key - 1]:0.0000} s and {channel.times[key]:0.0000} s; a glTF sampler needs strictly increasing times");
                return false;
            }
        }
        return true;
    }

    /// <summary>
    /// Writes edited keys back to the motion buffer file. glTF keeps animation data in the buffer, so
    /// the JSON text does not change and every glTF tool still reads the file.
    /// </summary>
    internal bool try_save(out string error) {
        error = string.Empty;
        if (this.buffer_path == null) {
            error = "the motion buffer is embedded in the JSON text, which this build cannot write yet";
            return false;
        }
        for (var index = 0; index < this.channels.Count; ++index) {
            if (!has_strict_times(this.channels[index], out error)) {
                return false;
            }
            if (!this.write_channel(this.channels[index], out error)) {
                return false;
            }
        }
        try {
            File.WriteAllBytes(this.buffer_path, this.buffer);
        } catch (IOException write_error) {
            error = $"the motion buffer could not be written: {write_error.Message}";
            return false;
        } catch (UnauthorizedAccessException write_error) {
            error = $"the motion buffer is not writable: {write_error.Message}";
            return false;
        }
        if (this.clip_name != this.file_clip_name) {
            if (!this.try_write_clip_name(this.clip_name, out error)) {
                return false;
            }
            this.file_clip_name = this.clip_name;
        }
        this.dirty = false;
        return true;
    }

    /// <summary>
    /// Replaces the first animation's `name` string in place, so a rename keeps the rest of the file
    /// byte for byte: the loader's own keys, extras, formatting and the buffer layout all survive.
    /// When the animation has no name yet, the property is inserted as its first member.
    /// </summary>
    internal bool try_write_clip_name(string name, out string error) {
        error = string.Empty;
        byte[] text;
        try {
            text = File.ReadAllBytes(this.path);
        } catch (IOException read_error) {
            error = $"the motion file could not be read: {read_error.Message}";
            return false;
        } catch (UnauthorizedAccessException read_error) {
            error = $"the motion file is not readable: {read_error.Message}";
            return false;
        }
        long value_start = -1;
        long value_end = -1;
        long insert_at = -1;
        var stack = new List<(JsonTokenType kind, string? property)>();
        string? pending = null;
        bool in_first_animation = false;
        bool found = false;
        try {
            var reader = new Utf8JsonReader(text, new JsonReaderOptions { CommentHandling = JsonCommentHandling.Skip });
            while (reader.Read()) {
                switch (reader.TokenType) {
                    case JsonTokenType.PropertyName:
                        pending = reader.GetString();
                        break;
                    case JsonTokenType.StartArray:
                        stack.Add((JsonTokenType.StartArray, pending));
                        pending = null;
                        break;
                    case JsonTokenType.StartObject: {
                        bool first_animation = !in_first_animation && stack.Count > 0 &&
                                               stack[^1].kind == JsonTokenType.StartArray &&
                                               stack[^1].property == "animations";
                        stack.Add((JsonTokenType.StartObject, pending));
                        pending = null;
                        if (first_animation) {
                            in_first_animation = true;
                            insert_at = reader.TokenStartIndex + 1;
                        }
                        break;
                    }
                    case JsonTokenType.EndObject:
                    case JsonTokenType.EndArray:
                        stack.RemoveAt(stack.Count - 1);
                        pending = null;
                        break;
                    case JsonTokenType.String:
                        if (in_first_animation && pending == "name" && !found && stack.Count >= 2 &&
                            stack[^1].kind == JsonTokenType.StartObject &&
                            stack[^2].kind == JsonTokenType.StartArray &&
                            stack[^2].property == "animations") {
                            value_start = reader.TokenStartIndex;
                            value_end = string_end(text, value_start);
                            found = true;
                        }
                        pending = null;
                        break;
                    default:
                        pending = null;
                        break;
                }
            }
        } catch (JsonException parse_error) {
            error = $"the motion file's JSON could not be parsed while renaming: {parse_error.Message}";
            return false;
        }
        var encoded = JsonEncodedText.Encode(name).EncodedUtf8Bytes;
        var replacement = new byte[encoded.Length + 2];
        replacement[0] = (byte)'"';
        encoded.CopyTo(replacement.AsSpan(1));
        replacement[^1] = (byte)'"';
        byte[] updated;
        if (found && value_start >= 0 && value_end > value_start) {
            updated = new byte[text.Length - (value_end - value_start) + replacement.Length];
            Array.Copy(text, 0, updated, 0, value_start);
            Array.Copy(replacement, 0, updated, value_start, replacement.Length);
            Array.Copy(text, value_end, updated, value_start + replacement.Length, text.Length - value_end);
        } else if (insert_at > 0) {
            // The object gets `"name": value` as its first member. A comma is needed unless the object
            // is empty, which is checked by looking past the whitespace after the brace.
            bool has_members = false;
            for (long index = insert_at; index < text.Length; ++index) {
                if (!char.IsWhiteSpace((char)text[index])) {
                    has_members = text[index] != (byte)'}';
                    break;
                }
            }
            var key = Encoding.UTF8.GetBytes("\"name\": ");
            var separator = has_members ? Encoding.UTF8.GetBytes(", ") : [];
            updated = new byte[text.Length + key.Length + replacement.Length + separator.Length];
            Array.Copy(text, 0, updated, 0, insert_at);
            Array.Copy(key, 0, updated, insert_at, key.Length);
            Array.Copy(replacement, 0, updated, insert_at + key.Length, replacement.Length);
            Array.Copy(separator, 0, updated, insert_at + key.Length + replacement.Length, separator.Length);
            Array.Copy(text, insert_at, updated, insert_at + key.Length + replacement.Length + separator.Length,
                       text.Length - insert_at);
        } else {
            error = "the motion file has no animation whose name could be written";
            return false;
        }
        try {
            File.WriteAllBytes(this.path, updated);
        } catch (IOException write_error) {
            error = $"the motion file could not be written: {write_error.Message}";
            return false;
        } catch (UnauthorizedAccessException write_error) {
            error = $"the motion file is not writable: {write_error.Message}";
            return false;
        }
        return true;
    }

    /// <summary>The byte just past the closing quote of the string literal that starts at `quote`.</summary>
    private static long string_end(byte[] text, long quote) {
        for (long index = quote + 1; index < text.Length; ++index) {
            if (text[index] == (byte)'\\') {
                ++index;
            } else if (text[index] == (byte)'"') {
                return index + 1;
            }
        }
        return text.Length;
    }
}
