using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Loader;
using deren.scripting;

namespace deren.editor {
    internal enum parameter_kind { integer, number, boolean, vector, node, asset }

    internal sealed record script_parameter(FieldInfo member, string label, parameter_kind kind) {
        internal string name => member.Name;

        internal bool try_value(string input, scene_document document, script_base? owner, out object? value) {
            value = null;
            var culture = CultureInfo.InvariantCulture;
            switch (kind) {
                case parameter_kind.integer:
                    if (int.TryParse(input, NumberStyles.Integer, culture, out var integer)) { value = integer; }
                    break;
                case parameter_kind.number:
                    if (float.TryParse(input, NumberStyles.Float, culture, out var number) && float.IsFinite(number)) {
                        value = number;
                    }
                    break;
                case parameter_kind.boolean:
                    if (bool.TryParse(input, out var flag)) { value = flag; }
                    break;
                case parameter_kind.vector:
                    var parts = input.Split(',');
                    if (parts.Length == 3 && parse_number(parts[0], out var x) &&
                        parse_number(parts[1], out var y) && parse_number(parts[2], out var z)) {
                        value = new vector_3 { x = x, y = y, z = z };
                    }
                    break;
                case parameter_kind.node:
                    if (Guid.TryParse(input, out var uid) && document.find(uid) is { } target) {
                        value = new node_id(target.runtime.handle, owner);
                    }
                    break;
                case parameter_kind.asset:
                    if (input.Length > 0) { value = new asset_reference(input); }
                    break;
            }
            return value != null;
        }

        private static bool parse_number(string value, out float number) =>
            float.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out number) && float.IsFinite(number);
    }

    internal sealed record script_definition(string binding, Type type, script_parameter[] parameters);

    /// <summary>Discovers user-authored types without constructing any behaviour in the editor.</summary>
    internal sealed class script_catalog : IDisposable {
        private readonly string directory;
        private readonly List<project_script_context> contexts = [];
        private readonly Dictionary<string, script_definition> definitions = new(StringComparer.Ordinal);
        private readonly List<string> problems = [];

        internal script_catalog(string directory) {
            this.directory = directory;
        }

        internal IReadOnlyCollection<script_definition> entries => definitions.Values;
        internal IReadOnlyList<string> errors => problems;
        internal bool try_get(string binding, out script_definition? definition) => definitions.TryGetValue(binding, out definition);

        internal void reload() {
            Dispose();
            problems.Clear();
            if (!Directory.Exists(directory)) {
                return;
            }
            foreach (string file in Directory.EnumerateFiles(directory, "*.dll").Order(StringComparer.OrdinalIgnoreCase)) {
                if (Path.GetFileName(file).Equals("Deren.Managed.dll", StringComparison.OrdinalIgnoreCase)) {
                    continue;
                }
                try {
                    var context = new project_script_context(file);
                    contexts.Add(context);
                    using var input = File.OpenRead(file);
                    var assembly = context.LoadFromStream(input);
                    foreach (var type in assembly.GetExportedTypes().OrderBy(type => type.FullName, StringComparer.Ordinal)) {
                        if (type.IsAbstract || type.ContainsGenericParameters || !typeof(script_base).IsAssignableFrom(type) ||
                            type.GetConstructor(Type.EmptyTypes) == null) {
                            continue;
                        }
                        var fields = type.GetFields(BindingFlags.Public | BindingFlags.Instance)
                            .Where(field => !field.IsInitOnly)
                            .Select(parameter).OfType<script_parameter>()
                            .OrderBy(value => value.name, StringComparer.Ordinal).ToArray();
                        if (fields.Select(field => field.name).Distinct(StringComparer.Ordinal).Count() != fields.Length) {
                            problems.Add($"{type.FullName}: exported field names must be unique");
                            continue;
                        }
                        string binding = Path.GetFileName(file) + ":" + type.FullName;
                        definitions.Add(binding, new script_definition(binding, type, fields));
                    }
                } catch (Exception error) when (error is IOException or UnauthorizedAccessException or
                                                BadImageFormatException or ReflectionTypeLoadException or TypeLoadException) {
                    problems.Add($"{Path.GetFileName(file)}: {error.GetBaseException().Message}");
                }
            }
        }

        private static script_parameter? parameter(FieldInfo field) {
            var attribute = field.GetCustomAttribute<editor_parameter>();
            if (attribute == null) { return null; }
            var type = field.FieldType;
            parameter_kind? kind = type == typeof(int) ? parameter_kind.integer :
                type == typeof(float) ? parameter_kind.number : type == typeof(bool) ? parameter_kind.boolean :
                type == typeof(vector_3) ? parameter_kind.vector : type == typeof(node_id) ? parameter_kind.node :
                type == typeof(asset_reference) ? parameter_kind.asset : null;
            return kind is { } supported ? new script_parameter(field, attribute.label ?? field.Name, supported) : null;
        }

        public void Dispose() {
            definitions.Clear();
            foreach (var context in contexts) {
                context.Unload();
            }
            contexts.Clear();
        }

        private sealed class project_script_context(string path) : AssemblyLoadContext(isCollectible: true) {
            private readonly AssemblyDependencyResolver resolver = new(path);

            protected override Assembly? Load(AssemblyName name) {
                var bridge = typeof(script_base).Assembly;
                if (name.Name == bridge.GetName().Name) {
                    return bridge;
                }
                string candidate = Path.Combine(Path.GetDirectoryName(path)!, name.Name + ".dll");
                string? resolved = resolver.ResolveAssemblyToPath(name) ?? (File.Exists(candidate) ? candidate : null);
                if (resolved == null) {
                    return null;
                }
                using var input = File.OpenRead(resolved);
                return LoadFromStream(input);
            }
        }
    }
}
