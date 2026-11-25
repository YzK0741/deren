using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;

namespace deren.editor;

internal static class scene_instances {
    private static readonly StringComparer path_comparer = OperatingSystem.IsWindows()
        ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

    internal static bool try_load(string project, string path, out scene_document? document, out string? error) {
        if (!document_io.try_load(path, out document, out error) || document == null) { return false; }
        if (try_populate(project, document.roots, document.path, out error)) { return true; }
        document = null;
        return false;
    }

    internal static bool try_populate(string project, IReadOnlyList<document_node> nodes, string? owner_path,
                                      out string? error) {
        project = Path.TrimEndingDirectorySeparator(Path.GetFullPath(project));
        var stack = new HashSet<string>(path_comparer);
        if (owner_path != null) { stack.Add(Path.GetFullPath(owner_path)); }
        if (!expand(project, nodes, stack, out error)) { return false; }
        if (scene_lights.count(nodes) <= scene_lighting.max_point_lights) { return true; }
        error = $"A scene supports at most {scene_lighting.max_point_lights} point lights including instances.";
        return false;
    }

    internal static bool references_path(string project, IReadOnlyList<document_node> nodes, string path) {
        foreach (var node in nodes) {
            if (node.subscene is { } linked && scene_runtime.try_resolve_scene(project, linked, out var source) &&
                path_comparer.Equals(path, source)) { return true; }
            if (references_path(project, node.children, path)) { return true; }
        }
        return false;
    }

    private static bool expand(string project, IReadOnlyList<document_node> nodes, HashSet<string> stack,
                               out string? error) {
        foreach (var node in nodes) {
            if (node.subscene is not { } source) {
                if (!expand(project, node.children, stack, out error)) { return false; }
                continue;
            }
            if (node.children.Count != 0) { continue; }
            if (!try_scene_path(project, source, out string path)) {
                error = $"Scene instance '{node.name}' needs a file under scenes/: '{source}'.";
                return false;
            }
            if (stack.Count >= 32 || !stack.Add(path)) {
                error = $"Circular or excessively nested scene instance: '{source}'.";
                return false;
            }
            try {
                if (!document_io.try_load(path, out var linked, out error) || linked == null) { return false; }
                var remap = new Dictionary<Guid, Guid>();
                remap_nodes(linked.roots, node, remap);
                node.instance_node_ids = remap;
                if (!expand(project, linked.roots, stack, out error)) { return false; }
                apply_overrides(node, linked.roots, remap);
                node.children.AddRange(linked.roots);
            } finally {
                stack.Remove(path);
            }
        }
        error = null;
        return true;
    }

    private static bool try_scene_path(string project, string reference, out string path) {
        path = string.Empty;
        if (!reference.StartsWith("scenes/", StringComparison.Ordinal) ||
            reference.Split('/').Any(part => part is "" or "." or "..") ||
            !scene_runtime.try_resolve_scene(project, reference, out var resolved)) { return false; }
        try {
            for (string? current = resolved; current != null && !path_comparer.Equals(current, project);
                 current = Path.GetDirectoryName(current)) {
                if (File.GetAttributes(current).HasFlag(FileAttributes.ReparsePoint)) { return false; }
            }
        } catch (Exception error) when (error is IOException or UnauthorizedAccessException) {
            return false;
        }
        path = resolved;
        return true;
    }

    /// <summary>
    /// Applies the placement's parameter overrides to the freshly expanded nodes and keeps the
    /// source value on every node so the editor can show where a value comes from and restore it.
    /// The override keys are source scene uids, which survive a source edit and a reload.
    /// </summary>
    private static void apply_overrides(document_node instance, IReadOnlyList<document_node> nodes,
                                        IReadOnlyDictionary<Guid, Guid> remap) {
        var overrides = instance.instance_overrides;
        var sources = new Dictionary<Guid, Guid>();
        foreach (var (source, expanded) in remap) { sources[expanded] = source; }
        apply(nodes, sources, overrides);
    }

    private static void apply(IReadOnlyList<document_node> nodes, IReadOnlyDictionary<Guid, Guid> sources,
                              SortedDictionary<Guid, SortedDictionary<string, string>>? overrides) {
        foreach (var node in nodes) {
            if (sources.TryGetValue(node.uid, out var source)) {
                node.source_script_parameters = new SortedDictionary<string, string>(node.script_parameters,
                    StringComparer.Ordinal);
                if (overrides != null && overrides.TryGetValue(source, out var values)) {
                    foreach (var (name, value) in values) { node.script_parameters[name] = value; }
                }
            }
            apply(node.children, sources, overrides);
        }
    }

    private static void remap_nodes(IReadOnlyList<document_node> nodes, document_node instance,
                                    Dictionary<Guid, Guid> remap) {
        Span<byte> input = stackalloc byte[32];
        Span<byte> digest = stackalloc byte[32];
        instance.uid.TryWriteBytes(input[..16]);
        foreach (var node in nodes) {
            node.uid.TryWriteBytes(input[16..]);
            SHA256.HashData(input, digest);
            var uid = new Guid(digest[..16]);
            remap.Add(node.uid, uid);
            node.uid = uid;
            node.instance_parent = instance;
            node.instance_root = instance.instance_root ?? instance;
            node.instance_node_ids = remap;
            node.main_camera = false;
            remap_nodes(node.children, instance, remap);
        }
    }
}
