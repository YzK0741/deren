using deren.scripting;
using System;
using System.Runtime.InteropServices;

namespace deren {
    // `--collision-selftest`: exercises the ABI 45 collision world against the host's kernel.
    // The world is empty on entry, so the numbers are the kernel's own: a sphere dropped at a unit
    // cube, the slide that stops on the cube's top face, the walkable floor probe, and the clear
    // answers after the only box is removed. The host half of the test asserts how many of each call
    // arrived, so a wrapper that silently skipped one is caught there.
    internal static unsafe class collision_selftest {
        internal static bool requested;

        private static bool checked_once;

        private const float tolerance = 0.01f;

        internal static script_status initialize(managed_api* scripts) {
            scripts->update = &update;
            scripts->stop = &stop;
            return script_status.success;
        }

        [UnmanagedCallersOnly]
        private static script_status update(frame_info* frame) {
            if (checked_once) {
                return script_status.success;
            }
            checked_once = true;
            try {
                return run();
            } catch (Exception error) {
                host_logger.write($"Game: collision self check threw: {error.GetBaseException().Message}");
                return script_status.script_failure;
            }
        }

        private static script_status run() {
            var half = new vector_3 { x = 0.5f, y = 0.5f, z = 0.5f };
            var id = 7u;
            if (!collision_world.reset()) {
                return script_status.script_failure;
            }
            var box = new collision_box(new vector_3(), half,
                                        new vector_3 { x = 1.0f }, new vector_3 { y = 1.0f },
                                        new vector_3 { z = 1.0f }, id);
            if (!collision_world.add_box(in box)) {
                return script_status.script_failure;
            }

            // A sphere two and a half metres away from the cube's -z face: 2.25 of clear travel.
            var sphere = new collision_capsule(new vector_3 { z = -3.0f }, new vector_3 { z = -3.0f }, 0.25f);
            if (!collision_world.sweep(in sphere, new vector_3 { z = 5.0f }, out var blocked) ||
                !blocked.hit || blocked.id != id || MathF.Abs(blocked.time - 0.45f) > tolerance ||
                blocked.normal.z > -0.9f) {
                host_logger.write($"Game: collision self check: the blocked sweep was {blocked}");
                return script_status.script_failure;
            }

            // Straight up is clear: the cube is below the whole path.
            if (!collision_world.sweep(in sphere, new vector_3 { y = 5.0f }, out var clear) || clear.hit) {
                host_logger.write($"Game: collision self check: the clear sweep reported a hit");
                return script_status.script_failure;
            }

            // A slide that never touches anything travels the full request.
            var above = new collision_capsule(new vector_3 { y = 2.0f }, new vector_3 { y = 2.0f }, 0.25f);
            if (!collision_world.move(in above, new vector_3 { y = -1.0f }, out var free) || free.blocked ||
                MathF.Abs(free.displacement.y + 1.0f) > tolerance || free.grounded) {
                host_logger.write($"Game: collision self check: the clear slide was {free}");
                return script_status.script_failure;
            }

            // Dropped two metres onto the cube's top face it stops after 1.25 and calls it ground.
            if (!collision_world.move(in above, new vector_3 { y = -2.0f }, out var landed) || !landed.blocked ||
                !landed.grounded || MathF.Abs(landed.displacement.y + 1.25f) > tolerance ||
                landed.normal.y < 0.9f) {
                host_logger.write($"Game: collision self check: the landing slide was {landed}");
                return script_status.script_failure;
            }

            // The same surface is a walkable floor for a probe that reaches it.
            if (!collision_world.find_floor(in above, 4.0f, out var floor) || !floor.hit || !floor.walkable ||
                floor.id != id || MathF.Abs(floor.distance - 1.25f) > tolerance) {
                host_logger.write($"Game: collision self check: the floor probe was {floor}");
                return script_status.script_failure;
            }

            // With the only box gone every answer is clear again.
            if (!collision_world.remove_box(id) ||
                !collision_world.sweep(in sphere, new vector_3 { z = 5.0f }, out var removed) || removed.hit) {
                host_logger.write($"Game: collision self check: the world kept the removed box");
                return script_status.script_failure;
            }
            return script_status.success;
        }

        [UnmanagedCallersOnly]
        private static script_status stop() {
            return script_status.success;
        }
    }
}
