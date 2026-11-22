using System;
using System.Collections.Generic;
using System.Linq;
using deren.scripting;

namespace deren.editor {
    internal sealed class play_behaviours : IDisposable {
        private readonly script_catalog catalog;
        private readonly List<(document_node node, script_base behaviour)> attached = [];

        internal play_behaviours(script_catalog catalog) {
            this.catalog = catalog;
        }

        internal bool try_attach_nodes(scene_document document, ulong scene, Func<string, bool> change_scene,
                                       out string message) {
            try {
                attach_all(document, document.roots, scene, change_scene);
                message = string.Empty;
                return true;
            } catch (Exception error) {
                message = $"Editor play: script attach failed ({error.GetBaseException().Message})";
                detach_all();
                return false;
            }
        }

        private void attach_all(scene_document document, IReadOnlyList<document_node> nodes, ulong scene,
                                Func<string, bool> change_scene) {
            foreach (var node in nodes) {
                if (node.script is { } binding) {
                    script_base? behaviour = null;
                    try {
                        if (!catalog.try_get(binding, out var script) || script == null) {
                            throw new InvalidOperationException($"Script '{binding}' is unavailable.");
                        }
                        behaviour = (script_base)Activator.CreateInstance(script.type)!;
                        behaviour.attach(scene, node.runtime.handle, change_scene);
                        foreach (var (key, input) in node.script_parameters) {
                            var parameter = script.parameters.FirstOrDefault(field => field.name == key);
                            if (parameter == null || !parameter.try_value(input, document, behaviour, out var value)) {
                                throw new InvalidOperationException($"Parameter '{key}' cannot use '{input}'.");
                            }
                            parameter.member.SetValue(behaviour, value);
                        }
                        behaviour.on_create();
                        attached.Add((node, behaviour));
                        host_logger.write($"Editor play: attached {binding} to {node.name}");
                    } catch (Exception error) {
                        behaviour?.detach();
                        report(node, "on_create", error);
                        throw;
                    }
                }
                attach_all(document, node.children, scene, change_scene);
            }
        }

        internal void update(float delta) {
            foreach (var (node, behaviour) in attached) {
                try {
                    behaviour.on_update(delta);
                } catch (Exception error) {
                    report(node, "on_update", error);
                    throw;
                }
            }
        }

        private static void report(document_node node, string callback, Exception error) {
            var reason = error.GetBaseException().Message.Replace('\r', ' ').Replace('\n', ' ');
            host_logger.write($"Editor play node {node.uid:N}: {node.name} / {callback}: {reason}");
        }

        private void detach_all() {
            for (int index = attached.Count - 1; index >= 0; --index) {
                var (node, behaviour) = attached[index];
                try {
                    behaviour.on_destroy();
                } catch (Exception error) {
                    report(node, "on_destroy", error);
                } finally {
                    behaviour.detach();
                }
            }
            attached.Clear();
        }

        public void Dispose() {
            detach_all();
        }
    }
}
