using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;

namespace deren.scripting {
    internal static unsafe class scene_scripts {
        private sealed class scene_state(native_api callbacks) {
            public readonly native_api api = callbacks;
            public readonly List<(node_handle node, script_behaviour script)> instances = [];
            public readonly Dictionary<string, int> actions = new(StringComparer.Ordinal);
        }

        private static readonly Dictionary<ulong, scene_state> scenes = [];

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static script_status start_scene(native_api* api, ulong scene, utf8_view assembly_path, script_binding* bindings, uint count,
            utf8_view* action_names, uint action_count) {
            if (api == null || !api->is_valid() || scene == 0 || assembly_path.data == null ||
                assembly_path.length == 0 || (bindings == null && count != 0) || count > int.MaxValue ||
                (action_names == null && action_count != 0) || action_count > int.MaxValue) {
                return script_status.invalid_argument;
            }
            if (scenes.ContainsKey(scene)) {
                return script_status.scene_exists;
            }

            var state = new scene_state(*api);
            try {
                for (var index = 0; index < (int)action_count; ++index) {
                    var name = action_names[index];
                    if (name.data == null || name.length == 0 || !state.actions.TryAdd(name.decode(), index)) {
                        return script_status.invalid_argument;
                    }
                }
                var path = Path.GetFullPath(assembly_path.decode());
                var context = AssemblyLoadContext.GetLoadContext(typeof(script_behaviour).Assembly)!;
                var assembly = context.LoadFromAssemblyPath(path);
                var types = new Type[(int)count];
                var nodes = new HashSet<(uint index, uint generation)>();
                for (var index = 0; index < types.Length; ++index) {
                    var binding = bindings[index];
                    if (binding.node.scene != scene || binding.type.data == null || binding.type.length == 0 ||
                        !nodes.Add((binding.node.index, binding.node.generation))) {
                        return script_status.invalid_argument;
                    }
                    var name = binding.type.decode();
                    var type = assembly.GetType(name);
                    if (type == null || !typeof(script_behaviour).IsAssignableFrom(type) ||
                        type.IsAbstract || type.ContainsGenericParameters || type.GetConstructor(Type.EmptyTypes) == null) {
                        throw new InvalidOperationException($"'{name}' is not a concrete script_behaviour with a public parameterless constructor.");
                    }
                    types[index] = type;
                }

                for (var index = 0; index < types.Length; ++index) {
                    var node = bindings[index].node;
                    var type = types[index];
                    try {
                        var script = (script_behaviour)Activator.CreateInstance(type)!;
                        script.attach(state.api, node);
                        state.instances.Add((node, script));
                        script.on_create();
                    } catch (Exception error) {
                        log_exception(state.api, node, type, "on_create", error);
                        destroy(state);
                        return script_status.script_failure;
                    }
                }
                scenes.Add(scene, state);
                return script_status.success;
            } catch (Exception error) {
                state.api.write_log(Encoding.UTF8.GetBytes($"Scene {scene} start_scene: {error.GetBaseException().Message}"));
                destroy(state);
                return script_status.script_failure;
            }
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static script_status update_scene(ulong scene, float delta_time, byte* input_frame, uint input_count) {
            if (!float.IsFinite(delta_time)) {
                return script_status.invalid_argument;
            }
            if (!scenes.TryGetValue(scene, out var state)) {
                return script_status.scene_not_found;
            }
            if (input_count != state.actions.Count || (input_count != 0 && input_frame == null)) {
                return script_status.invalid_argument;
            }
            input_state.begin_frame(state.actions, input_frame);
            try {
                foreach (var (node, script) in state.instances) {
                    if (!script.updates_enabled) {
                        continue;
                    }
                    try {
                        script.on_update(delta_time);
                    } catch (Exception error) {
                        script.updates_enabled = false;
                        log_exception(state.api, node, script.GetType(), "on_update", error);
                    }
                }
            } finally {
                input_state.end_frame();
            }
            return script_status.success;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static script_status stop_scene(ulong scene) {
            return !scenes.Remove(scene, out var state) ? script_status.scene_not_found : destroy(state);
        }

        private static script_status destroy(scene_state state) {
            var status = script_status.success;
            for (var index = state.instances.Count - 1; index >= 0; --index) {
                var (node, script) = state.instances[index];
                try {
                    script.on_destroy();
                } catch (Exception error) {
                    log_exception(state.api, node, script.GetType(), "on_destroy", error);
                    status = script_status.script_failure;
                } finally {
                    script.detach();
                }
            }
            state.instances.Clear();
            return status;
        }

        private static void log_exception(native_api api, node_handle node, Type type, string phase, Exception error) {
            var message = $"Scene {node.scene}, node {node.index}:{node.generation}, {type.FullName}.{phase}: {error.GetBaseException().Message}";
            api.write_log(Encoding.UTF8.GetBytes(message));
        }
    }
}
