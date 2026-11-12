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
            public readonly List<(entity_handle entity, behaviour script)> instances = new();
        }

        private static readonly Dictionary<ulong, scene_state> scenes = new();

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static script_status start_scene(native_api* api, ulong scene, utf8_view assembly_path, script_binding* bindings, uint count) {
            if (api == null || !api->is_valid() || scene == 0 || assembly_path.data == null ||
                assembly_path.length == 0 || (bindings == null && count != 0) || count > int.MaxValue) {
                return script_status.invalid_argument;
            }
            if (scenes.ContainsKey(scene)) {
                return script_status.scene_exists;
            }

            var state = new scene_state(*api);
            try {
                var path = Path.GetFullPath(assembly_path.decode());
                var context = AssemblyLoadContext.GetLoadContext(typeof(behaviour).Assembly)!;
                var assembly = context.LoadFromAssemblyPath(path);
                var types = new Type[(int)count];
                var entities = new HashSet<(uint index, uint generation)>();
                for (var index = 0; index < types.Length; ++index) {
                    var binding = bindings[index];
                    if (binding.entity.scene != scene || binding.type.data == null || binding.type.length == 0 ||
                        !entities.Add((binding.entity.index, binding.entity.generation))) {
                        return script_status.invalid_argument;
                    }
                    var name = binding.type.decode();
                    var type = assembly.GetType(name);
                    if (type == null || !typeof(behaviour).IsAssignableFrom(type) ||
                        type.IsAbstract || type.ContainsGenericParameters || type.GetConstructor(Type.EmptyTypes) == null) {
                        throw new InvalidOperationException($"'{name}' is not a concrete behaviour with a public parameterless constructor.");
                    }
                    types[index] = type;
                }

                for (var index = 0; index < types.Length; ++index) {
                    var entity = bindings[index].entity;
                    var type = types[index];
                    try {
                        var script = (behaviour)Activator.CreateInstance(type)!;
                        script.attach(state.api, entity);
                        state.instances.Add((entity, script));
                        script.on_create();
                    } catch (Exception error) {
                        log_exception(state.api, entity, type, "on_create", error);
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
        public static script_status update_scene(ulong scene, float delta_time) {
            if (!float.IsFinite(delta_time)) {
                return script_status.invalid_argument;
            }
            if (!scenes.TryGetValue(scene, out var state)) {
                return script_status.scene_not_found;
            }
            foreach (var (entity, script) in state.instances) {
                if (!script.updates_enabled) {
                    continue;
                }
                try {
                    script.on_update(delta_time);
                } catch (Exception error) {
                    script.updates_enabled = false;
                    log_exception(state.api, entity, script.GetType(), "on_update", error);
                }
            }
            return script_status.success;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static script_status stop_scene(ulong scene) {
            if (!scenes.Remove(scene, out var state)) {
                return script_status.scene_not_found;
            }
            return destroy(state);
        }

        private static script_status destroy(scene_state state) {
            var status = script_status.success;
            for (var index = state.instances.Count - 1; index >= 0; --index) {
                var (entity, script) = state.instances[index];
                try {
                    script.on_destroy();
                } catch (Exception error) {
                    log_exception(state.api, entity, script.GetType(), "on_destroy", error);
                    status = script_status.script_failure;
                } finally {
                    script.detach();
                }
            }
            state.instances.Clear();
            return status;
        }

        private static void log_exception(native_api api, entity_handle entity, Type type, string phase, Exception error) {
            var message = $"Scene {entity.scene}, entity {entity.index}:{entity.generation}, {type.FullName}.{phase}: {error.GetBaseException().Message}";
            api.write_log(Encoding.UTF8.GetBytes(message));
        }
    }
}
