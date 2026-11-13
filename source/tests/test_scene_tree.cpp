// Headless unit tests: scene_tree ECS ownership ==============================
// Covers the stable scene_node::node_id assignment, recursive subtree adoption, clone
// detachment, component lookup, and cascading component destruction.
#include "vk_test.h"

#include <utility>

import deren.engine.scene_tree;

using deren::engine::scene_tree::scene;
using deren::engine::scene_tree::scene_node;
using deren::utility::ecs::entity;

namespace {
    struct marker {
        int value = 0;
    };
} // namespace

int main() {
    using namespace deren::engine::scene_tree;

    // A root added to a scene gets a live ECS entity immediately.
    {
        scene world;
        scene_node& root = world.add_root();
        CHECK(root.node_id.valid());
        CHECK(world.components.alive(root.node_id));
        CHECK(world.components.size() == 1);
        CHECK(world.find_node(root.node_id) == &root);
    }

    // Children added below an attached node are registered without an explicit scene call.
    {
        scene world;
        scene_node& root = world.add_root();
        scene_node& child = root.add_child();
        CHECK(child.node_id.valid());
        CHECK(world.components.alive(child.node_id));
        CHECK(world.find_node(child.node_id) == &child);
        CHECK(child.node_id != root.node_id);
    }

    // A detached subtree is adopted recursively, and its clone starts detached.
    {
        scene world;
        scene_node detached;
        scene_node& detached_child = detached.add_child();
        CHECK(!detached.node_id.valid());
        CHECK(!detached_child.node_id.valid());

        scene_node& adopted = world.add_root(std::move(detached));
        CHECK(adopted.node_id.valid());
        CHECK(adopted.children.size() == 1);
        CHECK(adopted.children[0].node_id.valid());
        CHECK(world.components.size() == 2);

        scene_node copy = adopted.clone();
        CHECK(!copy.node_id.valid());
        CHECK(copy.children.size() == 1);
        CHECK(!copy.children[0].node_id.valid());

        entity const original_root_id = adopted.node_id;
        entity const original_child_id = adopted.children[0].node_id;
        scene_node& cloned = world.add_root(std::move(copy));
        CHECK(cloned.node_id.valid());
        CHECK(cloned.node_id != original_root_id);
        CHECK(cloned.children[0].node_id.valid());
        CHECK(cloned.children[0].node_id != original_child_id);
        CHECK(world.components.size() == 4);
    }

    // A detached subtree moved under an attached parent is adopted there.
    {
        scene world;
        scene_node& root = world.add_root();
        scene_node detached;
        detached.add_child();
        scene_node& moved = root.add_child(std::move(detached));
        CHECK(moved.node_id.valid());
        CHECK(moved.children[0].node_id.valid());
        CHECK(world.find_node(moved.node_id) == &moved);
    }

    // Components live in the scene registry and cascade with the subtree.
    {
        scene world;
        scene_node& root = world.add_root();
        scene_node& child = root.add_child();
        marker* const value = world.components.try_emplace<marker>(child.node_id, 7);
        CHECK(value != nullptr);
        CHECK(value != nullptr && value->value == 7);
        CHECK(world.components.has<marker>(child.node_id));

        entity const root_id = root.node_id;
        entity const child_id = child.node_id;
        CHECK(world.destroy_node(root_id));
        CHECK(!world.components.alive(root_id));
        CHECK(!world.components.alive(child_id));
        CHECK(world.find_node(root_id) == nullptr);
        CHECK(world.find_node(child_id) == nullptr);
        CHECK(world.roots.empty());
        CHECK(world.components.size() == 0);
    }

    // Destroying a child leaves its parent and the parent's components alive.
    {
        scene world;
        scene_node& root = world.add_root();
        scene_node& child = root.add_child();
        CHECK(world.components.try_emplace<marker>(root.node_id, 1) != nullptr);
        CHECK(world.components.try_emplace<marker>(child.node_id, 2) != nullptr);

        entity const root_id = root.node_id;
        entity const child_id = child.node_id;
        CHECK(world.destroy_node(child_id));
        CHECK(root.children.empty());
        CHECK(world.components.alive(root_id));
        CHECK(!world.components.alive(child_id));
        CHECK(world.components.try_get<marker>(root_id) != nullptr);
        CHECK(world.components.try_get<marker>(child_id) == nullptr);
    }

    // clear() destroys every node entity and leaves the scene reusable.
    {
        scene world;
        world.add_root().add_child();
        world.add_root();
        CHECK(world.components.size() == 3);
        world.clear();
        CHECK(world.roots.empty());
        CHECK(world.components.size() == 0);

        scene_node& fresh = world.add_root();
        CHECK(fresh.node_id.valid());
        CHECK(world.components.alive(fresh.node_id));
    }

    // Rejecting null / stale handles is a safe false/null, not a panic.
    {
        scene world;
        CHECK(!world.destroy_node(entity{}));
        CHECK(world.find_node(entity{}) == nullptr);
        scene_node& root = world.add_root();
        entity const stale = root.node_id;
        CHECK(world.destroy_node(stale));
        CHECK(!world.destroy_node(stale));
        CHECK(world.find_node(stale) == nullptr);
    }

    return deren::vk_test::finish("test_scene_tree");
}
