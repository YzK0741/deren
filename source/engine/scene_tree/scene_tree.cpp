module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

module deren.engine.scene_tree;
import deren.vstd;
import deren.utility.shared;

namespace deren::engine::scene_tree {
    namespace {
        void destroy_node_components(scene_node& node, deren::utility::ecs::registry& components) {
            for (scene_node& child : node.children) {
                destroy_node_components(child, components);
            }
            if (node.node_id.valid()) {
                (void)components.destroy(node.node_id);
            }
            node.node_id = deren::utility::ecs::null_entity;
            node.owner_scene = nullptr;
        }

        template <class Nodes>
        bool erase_node_by_id(Nodes& nodes, deren::utility::ecs::entity id, deren::utility::ecs::registry& components) {
            for (auto it = nodes.begin(); it != nodes.end(); ++it) {
                if (it->node_id == id) {
                    destroy_node_components(*it, components);
                    nodes.erase(it);
                    return true;
                }
                if (erase_node_by_id(it->children, id, components)) {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    void update_world(scene_node& node, glm::mat4 const& parent_world) {
        glm::mat4 const world = parent_world * node.local;
        if (node.primitive_leaf) {
            node.primitive_leaf->set_world(world);
        }
        for (scene_node& child : node.children) {
            update_world(child, world);
        }
    }

    // DFS pre-order over every root of the scene: the iterator keeps an explicit stack of
    // {node, depth} frames; ++ pops the visited node and pushes its children (reversed so the
    // first child is visited next). Only valid while the tree structure is frozen (see
    // scene_iterator in scene_tree.cppm): no make/import/clear / child-vector pushes while
    // iterators live.
    scene_iterator::scene_iterator(scene& parent) {
        for (auto it = parent.roots.rbegin(); it != parent.roots.rend(); ++it) {
            this->stack.emplace_back(&*it, 0); // roots sit at depth 0
        }
        this->exhausted = this->stack.empty();
    }

    scene_node& scene_iterator::operator*() const noexcept {
        return *this->stack.back().first;
    }

    scene_node* scene_iterator::operator->() const noexcept {
        return this->stack.back().first;
    }

    scene_iterator& scene_iterator::operator++() {
        scene_node* const current = this->stack.back().first;
        std::size_t const child_depth = this->stack.back().second + 1;
        this->stack.pop_back();
        for (auto it = current->children.rbegin(); it != current->children.rend(); ++it) {
            this->stack.emplace_back(&*it, child_depth);
        }
        if (this->stack.empty()) {
            this->exhausted = true;
        }
        return *this;
    }

    scene_node scene_node::clone() const {
        scene_node copy;
        copy.node_name = this->node_name;
        copy.local = this->local;
        copy.source_index = this->source_index;
        copy.import_instance = this->import_instance;
        copy.visible = this->visible;
        // A clone is a detached subtree: it gets fresh identities when a scene adopts it.
        copy.node_id = deren::utility::ecs::null_entity;
        copy.owner_scene = nullptr;
        copy.children.reserve(this->children.size());
        for (scene_node const& child : this->children) {
            copy.children.push_back(child.clone());
        }
        // primitive leaves are intentionally NOT cloned: the runtime creates and registers them
        // once; a copy keeps the null leaf
        return copy;
    }

    scene_node& scene_node::add_child() {
        this->children.emplace_back();
        scene_node& child = this->children.back();
        if (this->owner_scene != nullptr) {
            this->owner_scene->register_subtree(child);
        }
        return child;
    }

    scene_node& scene_node::add_child(scene_node child) {
        this->children.push_back(std::move(child));
        scene_node& added = this->children.back();
        if (this->owner_scene != nullptr) {
            this->owner_scene->register_subtree(added);
        }
        return added;
    }

    primitive* scene_node::attach(std::unique_ptr<primitive> leaf) {
        if (leaf == nullptr) {
            return nullptr;
        }
        primitive* const result = leaf.get();
        this->primitive_leaf = std::move(leaf);
        return result;
    }

    // iterative DFS (explicit stack, mirrors import_scene's traversal style): visit the node,
    // then its children in order; first name match wins (pre-order).
    scene_node* scene_node::find_node(std::string_view const name) noexcept {
        if (name.empty()) {
            return nullptr; // empty names are the unnamed default; never match
        }
        deren::utility::vessel::vector<scene_node*> stack;
        stack.push_back(this);
        while (!stack.empty()) {
            scene_node* const current = stack.back();
            stack.pop_back();
            if (current->node_name == name) {
                return current;
            }
            // push children reversed so the first child is visited next (DFS pre-order)
            for (auto it = current->children.rbegin(); it != current->children.rend(); ++it) {
                stack.push_back(&*it);
            }
        }
        return nullptr;
    }

    scene_node const* scene_node::find_node(std::string_view const name) const noexcept {
        if (name.empty()) {
            return nullptr;
        }
        deren::utility::vessel::vector<scene_node const*> stack;
        stack.push_back(this);
        while (!stack.empty()) {
            scene_node const* const current = stack.back();
            stack.pop_back();
            if (current->node_name == name) {
                return current;
            }
            for (auto it = current->children.rbegin(); it != current->children.rend(); ++it) {
                stack.push_back(&*it);
            }
        }
        return nullptr;
    }

    void scene::register_subtree(scene_node& node) {
        scene* const previous = node.owner_scene;
        node.owner_scene = this;
        if (previous != this) {
            // Adopting a detached subtree (or moving one between scenes) gives every node a fresh
            // handle in this scene's registry. Nodes already owned here keep their handle.
            node.node_id = this->components.create();
        }
        for (scene_node& child : node.children) {
            this->register_subtree(child);
        }
    }

    scene_node& scene::add_root() {
        this->roots.emplace_back();
        scene_node& root = this->roots.back();
        this->register_subtree(root);
        return root;
    }

    scene_node& scene::add_root(scene_node root) {
        this->roots.push_back(std::move(root));
        scene_node& added = this->roots.back();
        this->register_subtree(added);
        return added;
    }

    scene_node* scene::find_node(std::string_view const name) noexcept {
        for (scene_node& root : this->roots) {
            if (scene_node* const found = root.find_node(name)) {
                return found;
            }
        }
        return nullptr;
    }

    scene_node const* scene::find_node(std::string_view const name) const noexcept {
        for (scene_node const& root : this->roots) {
            if (scene_node const* const found = root.find_node(name)) {
                return found;
            }
        }
        return nullptr;
    }

    scene_node* scene::find_node(deren::utility::ecs::entity const id) noexcept {
        if (!id.valid()) {
            return nullptr;
        }
        deren::utility::vessel::vector<scene_node*> stack;
        for (auto it = this->roots.rbegin(); it != this->roots.rend(); ++it) {
            stack.push_back(&*it);
        }
        while (!stack.empty()) {
            scene_node* const current = stack.back();
            stack.pop_back();
            if (current->node_id == id) {
                return current;
            }
            for (auto it = current->children.rbegin(); it != current->children.rend(); ++it) {
                stack.push_back(&*it);
            }
        }
        return nullptr;
    }

    scene_node const* scene::find_node(deren::utility::ecs::entity const id) const noexcept {
        if (!id.valid()) {
            return nullptr;
        }
        deren::utility::vessel::vector<scene_node const*> stack;
        for (auto it = this->roots.rbegin(); it != this->roots.rend(); ++it) {
            stack.push_back(&*it);
        }
        while (!stack.empty()) {
            scene_node const* const current = stack.back();
            stack.pop_back();
            if (current->node_id == id) {
                return current;
            }
            for (auto it = current->children.rbegin(); it != current->children.rend(); ++it) {
                stack.push_back(&*it);
            }
        }
        return nullptr;
    }

    bool scene::destroy_node(deren::utility::ecs::entity const id) {
        if (!id.valid()) {
            return false;
        }
        return erase_node_by_id(this->roots, id, this->components);
    }

    void scene::clear() {
        for (scene_node& root : this->roots) {
            destroy_node_components(root, this->components);
        }
        this->roots.clear();
        this->components.clear();
    }

    bool find_world(scene const& parent, std::uint64_t const import_instance, std::size_t const source_index, glm::mat4& output) {
        auto const visit = [&](auto&& self, scene_node const& node, glm::mat4 const& parent_world) -> bool {
            glm::mat4 const world = parent_world * node.local;
            if (node.import_instance == import_instance && node.source_index == source_index) {
                output = world;
                return true;
            }
            for (scene_node const& child : node.children) {
                if (self(self, child, world)) {
                    return true;
                }
            }
            return false;
        };
        for (scene_node const& root : parent.roots) {
            if (visit(visit, root, glm::mat4(1.0f))) {
                return true;
            }
        }
        return false;
    }
} // namespace deren::engine::scene_tree
