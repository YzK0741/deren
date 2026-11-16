// Headless unit tests: scene_tree ECS ownership ==============================
// Covers the stable scene_node::node_id assignment, recursive subtree adoption, clone
// detachment, component lookup, and cascading component destruction.
#include "vk_test.h"

#include <glm/gtc/matrix_transform.hpp>
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
        CHECK(components().alive(root.node_id));
        CHECK(components().size() == 1);
        CHECK(world.find_node(root.node_id) == &root);
    }

    // Children added below an attached node are registered without an explicit scene call.
    {
        scene world;
        scene_node& root = world.add_root();
        scene_node& child = root.add_child();
        CHECK(child.node_id.valid());
        CHECK(components().alive(child.node_id));
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
        CHECK(components().size() == 2);

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
        CHECK(components().size() == 4);
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

    // Components live in the process-wide registry and cascade with the subtree.
    {
        scene world;
        scene_node& root = world.add_root();
        scene_node& child = root.add_child();
        marker* const value = components().try_emplace<marker>(child.node_id, 7);
        CHECK(value != nullptr);
        CHECK(value != nullptr && value->value == 7);
        CHECK(components().has<marker>(child.node_id));

        entity const root_id = root.node_id;
        entity const child_id = child.node_id;
        CHECK(world.destroy_node(root_id));
        CHECK(!components().alive(root_id));
        CHECK(!components().alive(child_id));
        CHECK(world.find_node(root_id) == nullptr);
        CHECK(world.find_node(child_id) == nullptr);
        CHECK(world.roots.empty());
        CHECK(components().size() == 0);
    }

    // Destroying a child leaves its parent and the parent's components alive.
    {
        scene world;
        scene_node& root = world.add_root();
        scene_node& child = root.add_child();
        CHECK(components().try_emplace<marker>(root.node_id, 1) != nullptr);
        CHECK(components().try_emplace<marker>(child.node_id, 2) != nullptr);

        entity const root_id = root.node_id;
        entity const child_id = child.node_id;
        CHECK(world.destroy_node(child_id));
        CHECK(root.children.empty());
        CHECK(components().alive(root_id));
        CHECK(!components().alive(child_id));
        CHECK(components().try_get<marker>(root_id) != nullptr);
        CHECK(components().try_get<marker>(child_id) == nullptr);
    }

    // clear() destroys every node entity and leaves the scene reusable.
    {
        scene world;
        world.add_root().add_child();
        world.add_root();
        CHECK(components().size() == 3);
        world.clear();
        CHECK(world.roots.empty());
        CHECK(components().size() == 0);

        scene_node& fresh = world.add_root();
        CHECK(fresh.node_id.valid());
        CHECK(components().alive(fresh.node_id));
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

    // reparent_node moves a subtree between parents, root list included, without renaming the nodes.
    {
        scene world;
        world.add_root().add_child().add_child();
        scene_node& second = world.add_root();
        entity const second_id = second.node_id;
        // add_root can reallocate the root list, so every access below goes through ids again.
        entity const subject_id = world.roots[0].children[0].node_id;
        entity const grandchild_id = world.roots[0].children[0].children[0].node_id;
        CHECK(components().size() == 4);

        // Under the other root: the moved node keeps its identity, the whole subtree comes along.
        CHECK(world.reparent_node(subject_id, second_id));
        CHECK(world.find_node(subject_id) != nullptr);
        CHECK(world.roots[0].children.empty());
        CHECK(world.roots[1].children.size() == 1);
        CHECK(world.roots[1].children[0].node_id == subject_id);
        CHECK(world.roots[1].children[0].children.size() == 1);
        CHECK(world.roots[1].children[0].children[0].node_id == grandchild_id);
        CHECK(components().alive(subject_id));
        CHECK(components().alive(grandchild_id));

        // Back to the root list: a null parent means "no parent".
        CHECK(world.reparent_node(subject_id, entity{}));
        CHECK(world.find_node(subject_id) != nullptr);
        CHECK(world.roots.size() == 3);
        CHECK(world.roots[2].node_id == subject_id);

        // Already a root: the repeat still succeeds and does not duplicate the node.
        CHECK(world.reparent_node(subject_id, entity{}));
        CHECK(world.roots.size() == 3);
    }

    // The transform supplied to reparent_node is the local one; the world result follows the new parent.
    {
        scene world;
        world.add_root();
        world.add_root();
        glm::mat4 const parent_local = glm::translate(glm::mat4{1.0f}, glm::vec3{10.0f, 0.0f, 0.0f});
        world.roots[0].local = parent_local;
        glm::mat4 const local = glm::translate(glm::mat4{1.0f}, glm::vec3{1.0f, 2.0f, 3.0f});
        CHECK(world.reparent_node(world.roots[1].node_id, world.roots[0].node_id, &local));
        CHECK(world.roots[0].children.size() == 1);
        CHECK(world.roots[0].children[0].local == local);
    }

    // Cycles are refused: a node cannot move onto itself or under its own descendant.
    {
        scene world;
        scene_node& root = world.add_root();
        scene_node& middle = root.add_child();
        scene_node& leaf = middle.add_child();
        CHECK(!world.reparent_node(root.node_id, root.node_id));
        CHECK(!world.reparent_node(root.node_id, leaf.node_id));
        CHECK(!world.reparent_node(root.node_id, middle.node_id));
        CHECK(world.roots.size() == 1);
        CHECK(world.find_node(root.node_id) == &root);

        // A parent that is not in this scene is refused too.
        scene other;
        scene_node& foreign = other.add_root();
        CHECK(!world.reparent_node(middle.node_id, foreign.node_id));
        CHECK(root.children.size() == 1);
    }

    // flags are opaque storage: clone() carries them, and the scene never consults them.
    {
        scene world;
        scene_node& root = world.add_root();
        root.flags = 0xDEADBEEFu;
        scene_node copy = root.clone();
        CHECK(copy.flags == 0xDEADBEEFu);
        CHECK(!world.reparent_node(entity{}, entity{}));
    }

    return deren::vk_test::finish("test_scene_tree");
}
