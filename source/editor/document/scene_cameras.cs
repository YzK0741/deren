using System;
using System.Collections.Generic;
using System.Numerics;
using deren;

namespace deren.editor;

internal static class scene_cameras {
    internal const float default_fov = 45;

    internal static bool valid_fov(float value) => value is >= 5 and <= 150;

    private const float radians = MathF.PI / 180;

    internal static int count_main(IReadOnlyList<document_node> nodes) {
        int count = 0;
        foreach (var node in nodes) {
            count += (node.main_camera ? 1 : 0) + count_main(node.children);
        }
        return count;
    }

    internal static document_node? main(IReadOnlyList<document_node> nodes) {
        foreach (var node in nodes) {
            if (node.main_camera) { return node; }
            if (main(node.children) is { } child) { return child; }
        }
        return null;
    }

    internal static bool try_parent_matrix(IReadOnlyList<document_node> nodes, document_node camera,
                                           Matrix4x4 world, out Matrix4x4 parent) {
        foreach (var node in nodes) {
            if (node == camera) {
                parent = world;
                return true;
            }
            if (try_parent_matrix(node.children, camera, viewport_gizmo.local_matrix(node.local) * world, out parent)) {
                return true;
            }
        }
        parent = default;
        return false;
    }

    internal static bool try_pose(scene_document document, document_node camera, out orbit_camera pose) {
        pose = default;
        if (!camera.camera || !try_parent_matrix(document.roots, camera, Matrix4x4.Identity, out var parent)) {
            return false;
        }
        var camera_transform = camera.local;
        camera_transform.scale = new vector_3 { x = 1, y = 1, z = 1 };
        var world = viewport_gizmo.local_matrix(camera_transform) * parent;
        var back = Vector3.TransformNormal(Vector3.UnitZ, world);
        if (!try_angles(back, out var yaw, out var pitch)) { return false; }
        const float distance = 2.2f;
        var target = world.Translation - distance * back_vector(yaw, pitch);
        if (!float.IsFinite(target.X) || !float.IsFinite(target.Y) || !float.IsFinite(target.Z)) { return false; }
        pose = new orbit_camera {
            target = vector(target), yaw_degrees = yaw, pitch_degrees = pitch, distance = distance,
            vertical_fov_degrees = camera.camera_fov,
        };
        return true;
    }

    internal static bool try_align(orbit_camera pose, Matrix4x4 parent, ref transform_value local) {
        if (!Matrix4x4.Invert(parent, out var inverse)) { return false; }
        var back = back_vector(pose.yaw_degrees, pose.pitch_degrees);
        var eye = new Vector3(pose.target.x, pose.target.y, pose.target.z) + pose.distance * back;
        var position = Vector3.Transform(eye, inverse);
        if (!float.IsFinite(position.X) || !float.IsFinite(position.Y) || !float.IsFinite(position.Z) ||
            !try_angles(Vector3.TransformNormal(back, inverse), out var yaw, out var pitch)) {
            return false;
        }
        local.position = vector(position);
        local.rotation_degrees = new vector_3 { x = -pitch, y = yaw };
        return true;
    }

    private static bool try_angles(Vector3 back, out float yaw, out float pitch) {
        yaw = pitch = 0;
        float length = back.Length();
        if (!float.IsFinite(length) || length < 1e-6f) { return false; }
        yaw = MathF.Atan2(back.X, back.Z) / radians;
        pitch = Math.Clamp(MathF.Asin(Math.Clamp(back.Y / length, -1f, 1f)) / radians, -89f, 89f);
        return true;
    }

    private static Vector3 back_vector(float yaw, float pitch) {
        yaw *= radians;
        pitch *= radians;
        float horizontal = MathF.Cos(pitch);
        return new Vector3(horizontal * MathF.Sin(yaw), MathF.Sin(pitch), horizontal * MathF.Cos(yaw));
    }

    private static vector_3 vector(Vector3 value) => new() { x = value.X, y = value.Y, z = value.Z };
}
