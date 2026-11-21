using System.Collections.Generic;
using deren.scripting;

namespace deren.editor;

/// <summary>
/// Mirrors a <see cref="scene_document"/> onto a live scene tree.
/// </summary>
/// <remarks>
/// The document owns the values and this class owns their runtime projection. One runtime node is
/// created per document node and then kept in sync by <see cref="push"/>; <see cref="rebuild"/>
/// creates the whole tree once when a document is attached. Native activation is only allowed while
/// the project scene is starting, so <see cref="rebuild"/> is a startup operation and later edits
/// use <see cref="create_one"/> and <see cref="destroy_one"/> instead of another rebuild.
/// </remarks>
internal static class scene_sync
{
    /// <summary>
    /// Builds a fresh tree from <paramref name="doc"/>, makes it the rendered scene and pushes every
    /// node value onto it. On failure the half-built tree is destroyed and <paramref name="tree"/> is
    /// null.
    /// </summary>
    internal static bool rebuild(scene_document doc, out scene_tree? tree, out script_status status)
    {
        tree = null;
        status = script_status.invalid_argument;
        if (doc == null) {
            return false;
        }
        if (!scene_tree.try_create(out var created, out status) || created == null) {
            return false;
        }
        if (!created.try_activate(out status)) {
            created.destroy();
            return false;
        }
        // Bind the assets only after activation: the native side materialises the render instances of
        // a bound asset when the tree is already active, and a tree that is populated beforehand never
        // gets them.
        if (!create_all(doc, created, doc.roots, null, out status)) {
            created.detach();
            created.destroy();
            return false;
        }
        if (!push_all(doc, created, doc.roots, out status)) {
            created.detach();
            created.destroy();
            return false;
        }
        tree = created;
        status = script_status.success;
        return true;
    }

    /// <summary>
    /// Creates one runtime node for <paramref name="node"/> under <paramref name="parent"/> (null
    /// means a root) and stores the handle in <see cref="document_node.runtime"/>. An asset node is
    /// bound from the library, an empty node becomes a named transform node.
    /// </summary>
    internal static bool create_one(scene_document doc, scene_tree tree, document_node? parent, document_node node,
                                    out script_status status)
    {
        status = script_status.invalid_argument;
        if (doc == null || tree == null || node == null || !tree.is_valid) {
            return false;
        }

        var parent_runtime = default(node_id);
        if (parent != null && parent.runtime.is_valid) {
            parent_runtime = parent.runtime;
        }
        if (node.asset == null) {
            if (!tree.try_create_node(parent_runtime, node.name, node.local, out var created, out status)) {
                return false;
            }
            node.runtime = created;
            status = script_status.success;
            return true;
        }

        if (!asset_manager.try_load(node.asset, out var asset, out status)) {
            return false;
        }
        try {
            // THE DOCUMENT IS NOT REWRITTEN HERE. A null pipeline already means "the asset's declared
            // default" (push() then skips the set and the bind leaves the default in place), so
            // copying the declared name into the node would claim an edit nobody made: the save point
            // would move, a save would write lines the file never had, and the undo of a later
            // pipeline change could not get back to the authored state.
            if (!tree.try_bind_asset(parent_runtime, asset, node.local, out var bound, out status)) {
                return false;
            }
            node.runtime = bound;
            // Importing an asset is authoring, not a request to start its bundled motion.
            // Pause before the next frame consumes the new player's first update.
            if (animation_player.try_get_info(out _) && !animation_player.try_pause()) {
                tree.destroy_node(bound);
                node.runtime = default;
                status = script_status.script_failure;
                return false;
            }
            status = script_status.success;
            return true;
        } finally {
            asset_manager.release(asset);
        }
    }

    /// <summary>
    /// Destroys the runtime node of <paramref name="node"/> and clears the handle on the whole
    /// subtree. Returns false with <see cref="script_status.invalid_node"/> when the node has no
    /// runtime node, for example while the document is detached.
    /// </summary>
    internal static bool destroy_one(scene_document doc, scene_tree tree, document_node node,
                                     out script_status status)
    {
        status = script_status.invalid_argument;
        if (doc == null || tree == null || node == null) {
            return false;
        }
        if (!node.runtime.is_valid) {
            status = script_status.invalid_node;
            return false;
        }
        if (!tree.destroy_node(node.runtime, out status)) {
            return false;
        }
        clear_runtime(node);
        status = script_status.success;
        return true;
    }

    /// <summary>
    /// Pushes one node's transform, visibility, flags and pipeline into its runtime node. The name is
    /// applied last through <see cref="try_set_name"/> and never fails the push.
    /// </summary>
    internal static bool push(scene_document doc, scene_tree tree, document_node node, out script_status status)
    {
        status = script_status.invalid_argument;
        if (doc == null || tree == null || node == null) {
            return false;
        }
        if (!node.runtime.is_valid) {
            status = script_status.invalid_node;
            return false;
        }
        if (!tree.try_set_transform(node.runtime, node.local, out status)) {
            return false;
        }
        if (!tree.try_set_visible(node.runtime, node.visible, out status)) {
            return false;
        }
        if (!tree.try_set_flags(node.runtime, node.flags, uint.MaxValue, out status)) {
            return false;
        }
        if (node.pipeline != null && !tree.try_set_pipeline(node.runtime, node.pipeline, out status)) {
            return false;
        }
        _ = try_set_name(tree, node);
        status = script_status.success;
        return true;
    }

    /// <summary>
    /// Applies the document name to the runtime node through the managed ABI 40 rename entry.
    /// An empty or over-256-byte name is refused by the managed wrapper; that never fails the push.
    /// </summary>
    private static bool try_set_name(scene_tree tree, document_node node)
    {
        if (!tree.is_valid || !node.runtime.is_valid || node.name.Length == 0) {
            return false;
        }
        return tree.try_set_name(node.runtime, node.name);
    }

    private static bool create_all(scene_document doc, scene_tree tree, IReadOnlyList<document_node> nodes,
                                   document_node? parent, out script_status status)
    {
        foreach (var node in nodes) {
            if (!create_one(doc, tree, parent, node, out status)) {
                return false;
            }
            if (!create_all(doc, tree, node.children, node, out status)) {
                return false;
            }
        }
        status = script_status.success;
        return true;
    }

    private static bool push_all(scene_document doc, scene_tree tree, IReadOnlyList<document_node> nodes,
                                 out script_status status)
    {
        foreach (var node in nodes) {
            if (!push(doc, tree, node, out status)) {
                return false;
            }
            if (!push_all(doc, tree, node.children, out status)) {
                return false;
            }
        }
        status = script_status.success;
        return true;
    }

    private static void clear_runtime(document_node node)
    {
        node.runtime = default;
        foreach (var child in node.children) {
            clear_runtime(child);
        }
    }
}
