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
/// <c>[document]</c> table holding the <c>version</c> and <c>name</c>, then the optional
/// <c>[environment]</c> table with one <c>slot_N</c> entry per custom environment lane, followed by
/// one <c>[[node]]</c> table per node in depth-first order, where every parent appears before its
/// children. Reading rejects a duplicate uid, an unknown or duplicated key, a missing or unknown
/// parent, an unsupported version, an unparsable number and an unterminated string, and reports the
/// file, line and reason. The writer keeps a stable key order, writes one blank line between node
/// blocks and writes floats so that they round-trip exactly.
/// </remarks>
internal static class document_io
{
    /// <summary>
    /// The file format version this writer speaks: 5 adds point lights. Version 4 added environment lanes and the
    /// per-node lane index.
    /// </summary>
    private const int format_version = 10;
    private const int audio_format_version = 9;
    private const int trigger_format_version = 8;
    private const int instance_format_version = 7;
    private const int authoring_format_version = 6;
    private const int point_light_format_version = 5;
    private const int environment_format_version = 4;

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

        if (has_environment(document)) {
            text.Append("\n[environment]\n");
            for (var lane = 1; lane < global_environment.slot_count; ++lane) {
                var map = document.environment_slots[lane - 1];
                if (map != null) {
                    text.Append("    slot_").Append(lane.ToString(CultureInfo.InvariantCulture))
                        .Append(" = ").Append(quote(map)).Append('\n');
                }
            }
        }

        if (scene_lights.count(document.roots) > scene_lighting.max_point_lights) {
            error = $"A scene supports at most {scene_lighting.max_point_lights} point lights.";
            return false;
        }
        if (scene_cameras.count_main(document.roots) > 1) {
            error = "Only one camera can be the main camera.";
            return false;
        }
        var known = new HashSet<Guid>();
        if (!append_nodes(text, document.roots, null, known, out error)) {
            return false;
        }

        content = text.ToString();
        return true;
    }

    private static bool has_environment(scene_document document)
    {
        for (var lane = 1; lane < global_environment.slot_count; ++lane) {
            if (document.environment_slots[lane - 1] != null) {
                return true;
            }
        }
        return false;
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
        var environment_keys = new HashSet<string>(StringComparer.Ordinal);
        var document_seen = false;
        var version_seen = false;
        var environment_seen = false;
        var nodes_seen = false;
        var version = oldest_format_version;
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
            if (content == "[environment]") {
                if (!document_seen) {
                    error = format_error(path, number, "[environment] before [document]");
                    return false;
                }
                if (nodes_seen) {
                    error = format_error(path, number, "[environment] must come before the node blocks");
                    return false;
                }
                if (environment_seen) {
                    error = format_error(path, number, "[environment] appears more than once");
                    return false;
                }
                environment_seen = true;
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
                nodes_seen = true;
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
            if (environment_seen && !nodes_seen) {
                if (!assign_environment(built, environment_keys, key, value, path, number, out error)) {
                    return false;
                }
                continue;
            }
            if (!document_keys.Add(key)) {
                error = format_error(path, number, "duplicate key " + quote(key));
                return false;
            }
            if (key == "version") {
                if (!parse_int(value, path, number, out var declared, out error)) {
                    return false;
                }
                if (declared < oldest_format_version || declared > format_version) {
                    error = format_error(path, number,
                        "unsupported document version " + declared.ToString(CultureInfo.InvariantCulture));
                    return false;
                }
                version = declared;
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
        if (version < point_light_format_version && scene_lights.count(built.roots) != 0) {
            error = path + ": point lights need document version 5";
            return false;
        }
        if (version < authoring_format_version && has_authoring_components(built.roots)) {
            error = path + ": cameras and material overrides need document version 6";
            return false;
        }
        if (!validate_versioned_nodes(built.roots, version, path, out error)) { return false; }
        if (!validate_environment(built, path, version, environment_seen, out error)) {
            return false;
        }

        if (!try_serialize(built, out var saved, out error)) {
            return false;
        }
        built.mark_saved(path, saved);
        document = built;
        return true;
    }

    /// <summary>
    /// Checks the environment data of a loaded document: the section and the per-node lane only exist
    /// from version 4 on, and a node must never point at a lane the document does not carry a map for.
    /// </summary>
    /// <remarks>
    /// The second rule matters because lane 0 is the scene-wide map while a lane index above 0 turns the
    /// material override on: a node pointing at an empty lane would sample an undefined descriptor and
    /// render unlit, which is the one state the editor never produces itself.
    /// </remarks>
    private static bool validate_environment(scene_document document, string path, int version, bool stated,
                                             out string? error)
    {
        if (version < environment_format_version && stated) {
            error = path + ": [environment] needs document version " + environment_format_version.ToString(CultureInfo.InvariantCulture);
            return false;
        }
        string? failure = null;
        foreach (var root in document.roots) {
            visit(root);
        }
        error = failure;
        return failure == null;

        void visit(document_node node)
        {
            if (failure != null) {
                return;
            }
            if (node.environment != 0) {
                if (version < environment_format_version) {
                    failure = path + ": the environment lane of a node needs document version " +
                        environment_format_version.ToString(CultureInfo.InvariantCulture);
                    return;
                }
                if (node.environment < 0 || node.environment >= global_environment.slot_count) {
                    failure = path + ": environment lane " + node.environment.ToString(CultureInfo.InvariantCulture) +
                        " is outside 0.." + (global_environment.slot_count - 1).ToString(CultureInfo.InvariantCulture);
                    return;
                }
                if (string.IsNullOrEmpty(document.environment_slots[node.environment - 1])) {
                    failure = path + ": node " + quote(node.name) + " samples environment lane " +
                        node.environment.ToString(CultureInfo.InvariantCulture) + ", but the document does not set slot_" +
                        node.environment.ToString(CultureInfo.InvariantCulture);
                    return;
                }
            }
            foreach (var child in node.children) {
                visit(child);
            }
        }
    }

    /// <summary>
    /// Reads one key of the <c>[environment]</c> table: <c>slot_N</c> holds the HDR path of lane N,
    /// an empty string asking for the procedural sky.
    /// </summary>
    private static bool assign_environment(scene_document document, HashSet<string> keys, string key, string value,
                                           string path, int number, out string? error)
    {
        error = null;
        if (!keys.Add(key)) {
            error = format_error(path, number, "duplicate key " + quote(key));
            return false;
        }
        const string slot_prefix = "slot_";
        if (!key.StartsWith(slot_prefix, StringComparison.Ordinal) ||
            !parse_int(key[slot_prefix.Length..], path, number, out var lane, out error)) {
            error ??= format_error(path, number, "unknown key " + quote(key));
            return false;
        }
        if (lane < 1 || lane >= global_environment.slot_count) {
            error = format_error(path, number,
                $"environment lane {lane} is outside 1..{global_environment.slot_count - 1}");
            return false;
        }
        if (!parse_quoted(value, path, number, out var map, out error)) {
            return false;
        }
        document.environment_slots[lane - 1] = map;
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
            foreach (var (key, value) in node.script_parameters) {
                text.Append("    script_parameter.").Append(quote(key)).Append(" = ").Append(quote(value)).Append('\n');
            }
            text.Append("    visible = ").Append(node.visible ? "true" : "false").Append('\n');
            text.Append("    flags = ").Append(node.flags.ToString(CultureInfo.InvariantCulture)).Append('\n');
            if (node.environment != 0) {
                text.Append("    environment = ").Append(node.environment.ToString(CultureInfo.InvariantCulture))
                    .Append('\n');
            }
            if (node.audio_source is { } source) {
                if (!source.valid) {
                    error = $"Invalid audio source on node '{node.name}'.";
                    return false;
                }
                text.Append("    audio_file = ").Append(quote(source.file)).Append((char)10);
                text.Append("    audio_volume = ").Append(project_look.number(source.volume)).Append((char)10);
                text.Append("    audio_loop = ").Append(source.loop ? "true" : "false").Append((char)10);
            }
            if (node.collision_box) { text.Append("    collision_box = true\n"); }
            if (node.box_trigger) { text.Append("    box_trigger = true\n"); }
            if (node.trigger_target) { text.Append("    trigger_target = true\n"); }
            if (node.light is { } light) {
                if (!light.valid) {
                    error = $"Point light '{node.name}' has invalid settings.";
                    return false;
                }
                text.Append("    point_light = [").Append(project_look.vector(light.colour)).Append(", ")
                    .Append(project_look.number(light.intensity)).Append(", ").Append(project_look.number(light.range)).Append("]\n");
            }
            if (!node.camera && (node.main_camera || node.camera_fov != scene_cameras.default_fov)) {
                error = $"Node '{node.name}' has camera settings but no camera.";
                return false;
            }
            if (node.camera) {
                if (!scene_cameras.valid_fov(node.camera_fov)) {
                    error = $"Camera '{node.name}' field of view must be between 5 and 150 degrees.";
                    return false;
                }
                text.Append("    camera = ").Append(quote(node.main_camera ? "main" : "perspective")).Append('\n');
                text.Append("    camera_fov = ").Append(project_look.number(node.camera_fov)).Append('\n');
            }
            if (node.material is { } material) {
                if (node.asset == null || !material.valid) {
                    error = $"Node '{node.name}' needs a model and valid material override values.";
                    return false;
                }
                text.Append("    material_override = [").Append(project_look.vector(material.tint)).Append(", ")
                    .Append(project_look.number(material.metallic)).Append(", ")
                    .Append(project_look.number(material.roughness)).Append(", ")
                    .Append(project_look.number(material.emissive)).Append("]\n");
            }
            text.Append("    position = ").Append(format_vector(node.local.position)).Append('\n');
            text.Append("    rotation = ").Append(format_vector(node.local.rotation_degrees)).Append('\n');
            text.Append("    scale = ").Append(format_vector(node.local.scale)).Append('\n');
            if (node.subscene is { } linked) {
                if (node.asset != null || node.children.Exists(child => child.instance_root == null) ||
                    !linked.StartsWith("scenes/", StringComparison.Ordinal) ||
                    !linked.EndsWith(".scene.toml", StringComparison.OrdinalIgnoreCase)) {
                    error = $"Invalid scene instance '{node.name}'.";
                    return false;
                }
                text.Append("    subscene = ").Append(quote(linked)).Append((char)10);
            } else if (!append_nodes(text, node.children, node.uid, known, out error)) {
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

        if (builder.audio_file == null && (builder.keys.Contains("audio_volume") || builder.keys.Contains("audio_loop"))) {
            error = format_error(path, builder.line, "audio settings need an audio_file");
            return false;
        }

        var node = new document_node {
            uid = builder.uid,
            name = builder.name,
            asset = builder.asset,
            subscene = builder.subscene,
            pipeline = builder.pipeline,
            script = builder.script,
            script_parameters = builder.script_parameters,
            local = builder.local,
            visible = builder.visible,
            flags = builder.flags,
            environment = builder.environment,
            light = builder.light,
            audio_source = builder.audio_file != null
                ? new audio_source_component(builder.audio_file, builder.audio_volume, builder.audio_loop) : null,
            collision_box = builder.collision_box,
            box_trigger = builder.box_trigger,
            trigger_target = builder.trigger_target,
            camera = builder.camera,
            main_camera = builder.main_camera,
            camera_fov = builder.camera_fov,
            material = builder.material,
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
            // The runtime refuses a name past its byte cap, so a file holding one would load a value
            // the scene can never show. Rejecting it here keeps the line number in the error.
            if (scene_sync.name_utf8_byte_count(name) > scene_sync.max_name_utf8_bytes) {
                error = format_error(path, number, $"node name over {scene_sync.max_name_utf8_bytes} UTF-8 bytes");
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
        const string parameter_prefix = "script_parameter.";
        if (key.StartsWith(parameter_prefix, StringComparison.Ordinal)) {
            if (!parse_quoted(key[parameter_prefix.Length..], path, number, out var parameter, out error) ||
                !parse_quoted(value, path, number, out var raw, out error)) {
                return false;
            }
            if (parameter.Length == 0 || !builder.script_parameters.TryAdd(parameter, raw)) {
                error = format_error(path, number, "empty or duplicate script parameter");
                return false;
            }
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
        if (key == "environment") {
            if (!parse_int(value, path, number, out var environment, out error)) {
                return false;
            }
            if (environment < 0 || environment >= global_environment.slot_count) {
                error = format_error(path, number,
                    $"environment lane {environment} is outside 0..{global_environment.slot_count - 1}");
                return false;
            }
            builder.environment = environment;
            return true;
        }
        if (key == "audio_file") {
            if (!parse_quoted(value, path, number, out var name, out error)) { return false; }
            builder.audio_file = name.Replace((char)92, '/');
            return true;
        }
        if (key == "audio_volume") {
            return parse_float(value, path, number, out builder.audio_volume, out error);
        }
        if (key == "audio_loop") {
            return parse_bool(value, path, number, out builder.audio_loop, out error);
        }
        if (key == "collision_box") {
            return parse_bool(value, path, number, out builder.collision_box, out error);
        }
        if (key == "box_trigger") {
            return parse_bool(value, path, number, out builder.box_trigger, out error);
        }
        if (key == "trigger_target") {
            return parse_bool(value, path, number, out builder.trigger_target, out error);
        }
        if (key == "subscene") {
            if (!parse_quoted(value, path, number, out var source, out error)) { return false; }
            builder.subscene = source.Replace((char)92, '/');
            return true;
        }
        if (key == "camera_fov") {
            return parse_float(value, path, number, out builder.camera_fov, out error);
        }
        if (key == "camera") {
            if (!parse_quoted(value, path, number, out var kind, out error)) { return false; }
            if (kind is not ("perspective" or "main")) {
                error = format_error(path, number, "camera must be perspective or main");
                return false;
            }
            builder.camera = true;
            builder.main_camera = kind == "main";
            return true;
        }
        if (key == "material_override") {
            var parts = value.StartsWith('[') && value.EndsWith(']') ? value[1..^1].Split(',') : [];
            if (parts.Length != 6) {
                error = format_error(path, number, "material_override needs RGB, metallic, roughness and emissive");
                return false;
            }
            Span<float> values = stackalloc float[6];
            for (var index = 0; index < values.Length; ++index) {
                if (!parse_float(parts[index], path, number, out values[index], out error)) { return false; }
            }
            builder.material = new instance_material(new deren.vector_3 { x = values[0], y = values[1], z = values[2] },
                                                     values[3], values[4], values[5]);
            return true;
        }
        if (key == "point_light") {
            var parts = value.StartsWith('[') && value.EndsWith(']') ? value[1..^1].Split(',') : [];
            if (parts.Length != 5) {
                error = format_error(path, number, "point_light must contain RGB, intensity and range");
                return false;
            }
            Span<float> values = stackalloc float[5];
            for (var index = 0; index < values.Length; ++index) {
                if (!parse_float(parts[index], path, number, out values[index], out error)) { return false; }
            }
            var light = new point_light_component(new deren.vector_3 { x = values[0], y = values[1], z = values[2] },
                                                  values[3], values[4]);
            if (!light.valid) {
                error = format_error(path, number, "point_light needs RGB from 0 to 1, non-negative intensity and positive range");
                return false;
            }
            builder.light = light;
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

    internal static string quote(string text)
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

    private static bool validate_versioned_nodes(IReadOnlyList<document_node> nodes, int version, string path, out string? error) {
        foreach (var node in nodes) {
            if (node.subscene != null && version < instance_format_version) {
                error = path + ": scene instances need document version 7";
                return false;
            }
            if ((node.box_trigger || node.trigger_target) && version < trigger_format_version) {
                error = path + ": trigger components need document version 8";
                return false;
            }
            if (node.audio_source.HasValue && version < audio_format_version) {
                error = path + ": audio sources need document version 9";
                return false;
            }
            if (node.collision_box && version < format_version) {
                error = path + ": collision boxes need document version 10";
                return false;
            }
            if (!validate_versioned_nodes(node.children, version, path, out error)) { return false; }
        }
        error = null;
        return true;
    }

    private static bool has_authoring_components(IReadOnlyList<document_node> nodes) {
        foreach (var node in nodes) {
            if (node.camera || node.material.HasValue || has_authoring_components(node.children)) { return true; }
        }
        return false;
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
        internal string? subscene;
        internal string? pipeline;
        internal string? script;
        internal readonly SortedDictionary<string, string> script_parameters = new(StringComparer.Ordinal);
        internal bool visible = true;
        internal uint flags;
        internal int environment;
        internal point_light_component? light;
        internal string? audio_file;
        internal float audio_volume = 1;
        internal bool audio_loop;
        internal bool collision_box;
        internal bool box_trigger;
        internal bool trigger_target;
        internal bool camera;
        internal bool main_camera;
        internal float camera_fov = scene_cameras.default_fov;
        internal instance_material? material;
        internal deren.transform_value local = new() {
            scale = new deren.vector_3 { x = 1.0f, y = 1.0f, z = 1.0f },
        };
    }
}
