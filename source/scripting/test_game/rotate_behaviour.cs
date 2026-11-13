using System;
using deren;

namespace game {
    public sealed class rotate_behaviour : script_base {
        private float degrees_per_second;

        public override void on_create() {
            if (!self.is_valid || scene.self != self || !self.is_alive() || default(node_id).is_alive()) {
                throw new InvalidOperationException("scene self query failed");
            }
            if (!self.try_get_transform(out var queried) ||
                queried.position.x != transform.position.x ||
                queried.position.y != transform.position.y ||
                queried.position.z != transform.position.z ||
                queried.scale.x != transform.scale.x ||
                queried.scale.y != transform.scale.y ||
                queried.scale.z != transform.scale.z) {
                throw new InvalidOperationException("scene transform read failed");
            }
            if (!self.try_set_transform(queried) ||
                default(node_id).try_get_transform(out _) ||
                default(node_id).try_set_transform(queried)) {
                throw new InvalidOperationException("scene transform write failed");
            }
            var zero_scale = queried;
            zero_scale.scale = default;
            if (self.try_set_transform(zero_scale)) {
                throw new InvalidOperationException("scene transform accepted a zero scale");
            }
            if (!scene_tree.try_create(out var owned_tree) || owned_tree is null || !owned_tree.is_valid) {
                throw new InvalidOperationException("scene tree creation failed");
            }
            var tree_pose = transform;
            tree_pose.position.x = 0.25f;
            if (!owned_tree.try_create_node(default, "root", tree_pose, out var root) || !root.is_valid) {
                throw new InvalidOperationException("scene tree root creation failed");
            }
            if (!owned_tree.try_get_transform(root, out var tree_read) ||
                tree_read.position.x != tree_pose.position.x) {
                throw new InvalidOperationException("scene tree transform read failed");
            }
            tree_pose.position.y = 0.5f;
            if (!owned_tree.try_set_transform(root, tree_pose) ||
                !owned_tree.try_get_transform(root, out tree_read) ||
                tree_read.position.y != tree_pose.position.y) {
                throw new InvalidOperationException("scene tree transform write failed");
            }
            if (!owned_tree.try_create_node(root, "child", tree_pose, out var child) || !child.is_valid ||
                !owned_tree.destroy_node(child)) {
                throw new InvalidOperationException("scene tree child lifecycle failed");
            }
            if (!asset_manager.try_load("damaged-helmet", out var asset) || !asset.is_valid) {
                throw new InvalidOperationException("asset load failed");
            }
            if (Array.IndexOf(asset_manager.list(), "damaged-helmet") < 0) {
                throw new InvalidOperationException("asset list failed");
            }
            if (!owned_tree.try_bind_asset(root, asset, tree_pose, out var asset_root) || !asset_root.is_valid) {
                throw new InvalidOperationException("asset bind failed");
            }
            if (!asset_manager.release(asset)) {
                throw new InvalidOperationException("asset release failed");
            }
            if (!owned_tree.destroy() || owned_tree.is_valid) {
                throw new InvalidOperationException("scene tree destruction failed");
            }
            degrees_per_second = transform.position.x < 0.0f ? 30.0f : 60.0f;
        }

        public override void on_update(float delta_time) {
            if (!self.is_alive()) {
                throw new InvalidOperationException("scene node is gone");
            }
            var pose = transform;
            pose.rotation_degrees.y += degrees_per_second * delta_time;
            transform = pose;
        }

        public override void on_destroy() {
            var pose = transform;
            pose.position.z += 1.0f;
            transform = pose;
        }
    }
}
