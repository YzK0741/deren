using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;
using deren.scripting;

namespace deren.editor {
    /// <summary>Attaches the scene's declared C# behaviours inside the play process.</summary>
    internal sealed class play_behaviours : IDisposable {
        private readonly string directory;
        private readonly Dictionary<string, Assembly> assemblies = new(StringComparer.OrdinalIgnoreCase);
        private readonly List<script_base> attached = [];

        internal play_behaviours(string directory) {
            this.directory = directory;
        }

        private sealed class behaviour_assembly_context(string path) : AssemblyLoadContext {
            private readonly AssemblyDependencyResolver resolver = new(path);

            protected override Assembly? Load(AssemblyName name) {
                var bridge = typeof(script_base).Assembly;
                if (name.Name == bridge.GetName().Name) {
                    return bridge;
                }
                var resolved = resolver.ResolveAssemblyToPath(name);
                return resolved == null ? null : LoadFromAssemblyPath(resolved);
            }
        }

        /// <summary>
        /// Attaches every declared behaviour. On failure the behaviours attached so far are detached
        /// again, so a failed start never leaves a half-populated scene running.
        /// </summary>
        internal bool try_attach_nodes(IReadOnlyList<document_node> nodes, ulong scene, out string message) {
            try {
                attach_all(nodes, scene);
            } catch (Exception error) {
                message = $"Editor play: script attach failed ({error.GetBaseException().Message})";
                detach_all();
                return false;
            }
            message = string.Empty;
            return true;
        }

        private void attach_all(IReadOnlyList<document_node> nodes, ulong scene) {
            foreach (var node in nodes) {
                if (node.script != null) {
                    var behaviour = create(node.script);
                    // Registered before attach, because a throw from attach or on_create must still
                    // find the half-attached object and release its node.
                    attached.Add(behaviour);
                    try {
                        behaviour.attach(scene, node.runtime.handle);
                        behaviour.on_create();
                    } catch {
                        attached.Remove(behaviour);
                        detach(behaviour, destroy: false);
                        throw;
                    }
                    host_logger.write($"Editor play: attached {node.script} to {node.name}");
                }
                attach_all(node.children, scene);
            }
        }

        private script_base create(string binding) {
            int separator = binding.IndexOf(':');
            if (separator <= 0 || separator == binding.Length - 1) {
                throw new InvalidOperationException($"Use assembly.dll:Namespace.Type for script '{binding}'.");
            }
            var file = binding[..separator];
            if (file.IndexOfAny(['/', '\\', ':']) >= 0 || !file.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)) {
                throw new InvalidOperationException($"Script assembly '{file}' must be a DLL in the editor's scripts folder.");
            }
            if (!assemblies.TryGetValue(file, out Assembly? assembly)) {
                var path = Path.Combine(directory, file);
                assembly = new behaviour_assembly_context(path).LoadFromAssemblyPath(path);
                assemblies.Add(file, assembly);
            }
            var type = assembly.GetType(binding[(separator + 1)..], throwOnError: true)!;
            if (type.IsAbstract || !typeof(script_base).IsAssignableFrom(type) ||
                Activator.CreateInstance(type) is not script_base behaviour) {
                throw new InvalidOperationException($"'{binding}' must be a concrete script_base with a public parameterless constructor.");
            }
            return behaviour;
        }

        internal void update(float delta) {
            foreach (var behaviour in attached) {
                behaviour.on_update(delta);
            }
        }

        /// <summary>
        /// Releases one behaviour. Neither callback is trusted: a script that throws here must not stop
        /// the remaining behaviours from being released.
        /// </summary>
        private static void detach(script_base behaviour, bool destroy) {
            if (destroy) {
                try {
                    behaviour.on_destroy();
                } catch (Exception error) {
                    host_logger.write($"Editor play: script shutdown failed ({error.GetBaseException().Message})");
                }
            }
            try {
                behaviour.detach();
            } catch (Exception error) {
                host_logger.write($"Editor play: script detach failed ({error.GetBaseException().Message})");
            }
        }

        private void detach_all() {
            for (int index = attached.Count - 1; index >= 0; --index) {
                detach(attached[index], destroy: true);
            }
            attached.Clear();
        }

        public void Dispose() {
            detach_all();
        }
    }
}
