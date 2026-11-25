using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using deren.scripting;

namespace deren.editor;

/// <summary>
/// One problem found while checking a project. <c>file</c> is relative to the project root (empty when the
/// problem belongs to the manifest itself) and <c>owner</c> names the node, table or key that owns the
/// broken reference, so the editor and the export tool can point at the thing to repair.
/// </summary>
internal readonly record struct project_problem(string file, string owner, string message) {
    internal string describe() {
        var where = file.Length == 0 ? "project" : file;
        return owner.Length == 0 ? $"{where}: {message}" : $"{where}: {owner}: {message}";
    }
}

internal sealed class project_report {
    internal readonly List<project_problem> problems = [];
    internal int scene_count;
    internal int checked_references;
    internal bool ok => problems.Count == 0;

    internal void add(string file, string owner, string message) {
        var problem = new project_problem(file, owner, message);
        // A scene instance shared by several scenes is checked once per user of it; report each fact once.
        if (!problems.Contains(problem)) {
            problems.Add(problem);
        }
    }

    internal string summary() => ok
        ? $"Project check passed: {scene_count} scene(s), {checked_references} reference(s)."
        : $"Project check failed: {problems.Count} problem(s) across {scene_count} scene(s).";
}

/// <summary>
/// Walks the saved project data and reports references that cannot resolve: the scenes named by the
/// manifest, every scene under <c>scenes/</c>, assets, scene instances, behaviour bindings, audio files,
/// environment lanes and the explicit <c>[export] include</c> list. The editor's project check and the
/// Windows export share this code, so a project that checks clean is exactly a project that can be
/// packaged.
/// </summary>
internal static class project_validator {
    private const int max_instance_depth = 8;

    internal static project_report validate(string project_root, asset_authoring? authoring = null,
                                           script_catalog? scripts = null) {
        var report = new project_report();
        string manifest_path = Path.Combine(project_root, "deren.toml");
        if (!File.Exists(manifest_path)) {
            report.add(string.Empty, "deren.toml", "the project manifest is missing");
            return report;
        }
        var manifest = toml_table.parse(File.ReadAllText(manifest_path), out _);
        if (manifest == null) {
            report.add(string.Empty, "deren.toml", "the project manifest could not be read");
            return report;
        }
        check_manifest(report, project_root, manifest);

        var scenes = new List<string>();
        string scenes_root = Path.Combine(project_root, "scenes");
        if (Directory.Exists(scenes_root)) {
            try {
                scenes.AddRange(Directory.EnumerateFiles(scenes_root, "*.scene.toml", new EnumerationOptions {
                    RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint
                }).Order(StringComparer.OrdinalIgnoreCase));
            } catch (Exception failure) when (failure is IOException or UnauthorizedAccessException) {
                report.add("scenes", string.Empty, $"the scene folder could not be listed: {failure.Message}");
            }
        }
        if (scenes.Count == 0) {
            report.add("scenes", string.Empty, "the project has no saved scene");
        }

        var context = new project_context(project_root, authoring, scripts);
        var visited = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (string scene_path in scenes) {
            check_scene_file(report, context, scene_path, 0, visited);
        }
        return report;
    }

    /// <summary>Checks the project that is already open in the editor, where the native asset catalogue and
    /// the loaded behaviour assemblies are available.</summary>
    internal static project_report validate_in_editor(string project_root) {
        asset_authoring.try_open(out var authoring);
        script_catalog? catalog = null;
        try {
            catalog = new script_catalog(Path.Combine(project_root, "scripts"));
            catalog.reload();
        } catch (Exception failure) when (failure is IOException or UnauthorizedAccessException or BadImageFormatException or
                                          ReflectionTypeLoadException) {
            host_logger.write($"Project check: the script catalog could not be loaded: {failure.Message}");
            catalog = null;
        }
        return validate(project_root, authoring.asset_directory != null ? authoring : null, catalog);
    }

    private sealed class project_context(string root, asset_authoring? authoring, script_catalog? scripts) {
        internal string root { get; } = root;
        internal asset_authoring? authoring { get; } = authoring;
        internal script_catalog? scripts { get; } = scripts;
    }

    private static void check_manifest(project_report report, string root, toml_table manifest) {
        string? startup = manifest.table("project")?.text("main_scene");
        if (startup == null) {
            report.add(string.Empty, "[project] main_scene", "no startup configuration is configured");
        } else if (!File.Exists(resolve(root, startup))) {
            report.add(string.Empty, "[project] main_scene", $"'{startup}' does not exist");
        }
        string? assembly = manifest.table("scripts")?.text("assembly");
        if (assembly == null) {
            report.add(string.Empty, "[scripts] assembly", "no game assembly is configured");
        } else if (!File.Exists(resolve(root, assembly))) {
            report.add(string.Empty, "[scripts] assembly", $"'{assembly}' does not exist");
        }
        string? library = manifest.table("paths")?.text("asset_library");
        if (library == null) {
            report.add(string.Empty, "[paths] asset_library", "no asset library is configured");
        } else if (!Directory.Exists(resolve(root, library))) {
            report.add(string.Empty, "[paths] asset_library", $"'{library}' is not a directory");
        }
        foreach (var (section, label) in new[] { ("editor", "[editor] scene"), ("game", "[game] scene") }) {
            string? scene = manifest.table(section)?.text("scene");
            if (scene == null) {
                if (section == "game") { report.add(string.Empty, label, "no startup scene is configured"); }
                continue;
            }
            if (!File.Exists(resolve(root, scene))) {
                report.add(string.Empty, label, $"'{scene}' does not exist");
            }
        }
        check_environment_path(report, root, string.Empty, "[lighting] environment_hdr",
                               manifest.table("lighting")?.text("environment_hdr"));
        var include = manifest.table("export")?.strings("include");
        if (include != null) {
            foreach (string entry in include) {
                if (entry.Length == 0) { continue; }
                ++report.checked_references;
                string full = resolve(root, entry);
                if (!File.Exists(full) && !Directory.Exists(full)) {
                    report.add(string.Empty, "[export] include", $"'{entry}' does not exist");
                }
            }
        }
    }

    private static void check_scene_file(project_report report, project_context context, string path,
                                         int depth, HashSet<string> visited) {
        string relative = relative_to(context.root, path);
        if (!visited.Add(Path.GetFullPath(path))) {
            report.add(relative, string.Empty, "the scene instance chain loops back to this file");
            return;
        }
        ++report.scene_count;
        if (!document_io.try_load(path, out var document, out string? error) || document == null) {
            report.add(relative, string.Empty, string.IsNullOrEmpty(error) ? "the scene could not be read" : error);
            visited.Remove(Path.GetFullPath(path));
            return;
        }
        for (int lane = 0; lane < document.environment_slots.Length; ++lane) {
            check_environment_path(report, context.root, relative, $"environment lane {lane + 1}",
                                   document.environment_slots[lane]);
        }
        foreach (var root_node in document.roots) {
            check_node(report, context, relative, root_node, depth, visited);
        }
        visited.Remove(Path.GetFullPath(path));
    }

    private static void check_node(project_report report, project_context context, string scene,
                                   document_node node, int depth, HashSet<string> visited) {
        string owner = $"node '{node.name}'";
        if (node.asset != null) {
            ++report.checked_references;
            if (context.authoring is not { } library) {
                report.add(scene, owner, $"asset '{node.asset}' could not be checked: the asset library is unavailable");
            } else if (library.asset_directory(node.asset) == null) {
                report.add(scene, owner, $"asset '{node.asset}' is not in the asset library");
            }
        }
        if (node.subscene != null) {
            ++report.checked_references;
            string target = resolve(context.root, node.subscene);
            if (!File.Exists(target)) {
                report.add(scene, owner, $"scene instance '{node.subscene}' does not exist");
            } else if (depth + 1 > max_instance_depth) {
                report.add(scene, owner, $"scene instances nest deeper than {max_instance_depth} levels");
            } else {
                check_scene_file(report, context, target, depth + 1, visited);
            }
        }
        if (node.script != null) {
            ++report.checked_references;
            check_script(report, context, scene, owner, node.script);
        }
        if (node.audio_source is { file.Length: > 0 } audio) {
            ++report.checked_references;
            if (!File.Exists(resolve(context.root, audio.file))) {
                report.add(scene, owner, $"audio file '{audio.file}' does not exist");
            }
        }
        foreach (var child in node.children) {
            check_node(report, context, scene, child, depth, visited);
        }
    }

    private static void check_script(project_report report, project_context context, string scene,
                                     string owner, string binding) {
        int separator = binding.IndexOf(':');
        if (separator <= 0 || separator == binding.Length - 1) {
            report.add(scene, owner, $"behaviour '{binding}' is not 'Assembly.dll:Namespace.Type'");
            return;
        }
        string assembly = binding[..separator];
        if (!File.Exists(Path.Combine(context.root, "scripts", assembly))) {
            report.add(scene, owner, $"behaviour assembly '{assembly}' is not in scripts/");
            return;
        }
        if (context.scripts == null) {
            report.add(scene, owner, $"behaviour '{binding}' could not be checked: the script catalog is unavailable");
            return;
        }
        if (!context.scripts.try_get(binding, out _)) {
            report.add(scene, owner, $"behaviour '{binding}' was not found in '{assembly}'");
        }
    }

    private static void check_environment_path(project_report report, string root, string file,
                                               string owner, string? path) {
        if (string.IsNullOrEmpty(path)) { return; }
        ++report.checked_references;
        if (path.StartsWith("asset:", StringComparison.Ordinal)) {
            report.add(file, owner, "environment maps are project files, not asset library ids");
            return;
        }
        if (!File.Exists(resolve(root, path))) {
            report.add(file, owner, $"environment map '{path}' does not exist");
        }
    }

    internal static string resolve(string root, string path) {
        try {
            return Path.IsPathRooted(path)
                ? path
                : Path.GetFullPath(path.Replace('/', Path.DirectorySeparatorChar), root);
        } catch (Exception failure) when (failure is ArgumentException or NotSupportedException or PathTooLongException) {
            return path;
        }
    }

    private static string relative_to(string root, string path) {
        try {
            return Path.GetRelativePath(root, path).Replace('\\', '/');
        } catch (ArgumentException) {
            return path;
        }
    }
}
