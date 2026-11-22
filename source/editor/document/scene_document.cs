using System;
using System.Collections.Generic;

namespace deren.editor;

/// <summary>
/// One edited scene document: the root nodes, the file it came from and its unsaved state.
/// </summary>
/// <remarks>
/// The document is the single source of truth for the editor. Runtime handles live on
/// <see cref="document_node.runtime"/> and belong to <see cref="scene_sync"/>; the document itself
/// never touches the renderer. The edit itself is not recorded here: the caller marks the document
/// dirty after a mutation and pushes its own undo command onto an <see cref="undo_stack"/>.
/// </remarks>
internal sealed class scene_document
{
    private readonly List<document_node> root_nodes = [];

    /// <summary>
    /// The path this document was loaded from or last saved to, or null for a new document.
    /// </summary>
    internal string? path;

    /// <summary>
    /// The document name written to the file; "untitled" for a new document.
    /// </summary>
    internal string name = "untitled";

    /// <summary>
    /// True while the document has edits that are not saved.
    /// </summary>
    internal bool dirty { get; private set; }

    /// <summary>
    /// Bumped by every edit that marks the document dirty. Panels use it to notice a value change
    /// that did not move the selection, such as a transform drag or an undo.
    /// </summary>
    internal int revision { get; private set; }

    private string? saved_content;

    /// <summary>
    /// The HDR path of every custom environment lane, indexed by lane minus one: entry 0 is lane 1.
    /// </summary>
    /// <remarks>
    /// Lane 0 is the project-wide map in the project's <c>[lighting] environment_hdr</c>, so it is not
    /// stored here. A null entry keeps whatever the project configured for that lane; an empty string
    /// asks for the procedural sky. The paths are stored as authored, relative to the project root.
    /// </remarks>
    internal readonly string?[] environment_slots = new string?[global_environment.slot_count - 1];

    /// <summary>
    /// The root nodes in outliner order.
    /// </summary>
    internal IReadOnlyList<document_node> roots => this.root_nodes;

    /// <summary>
    /// Finds the node with <paramref name="uid"/>, depth first, or null when the document has none.
    /// </summary>
    internal document_node? find(Guid uid)
    {
        return find_in(this.root_nodes, uid);
    }

    /// <summary>
    /// Marks the document as having unsaved edits.
    /// </summary>
    internal void mark_dirty()
    {
        this.revision++;
        this.dirty = this.saved_content == null ||
            !document_io.try_serialize(this, out var current, out _) || current != this.saved_content;
    }

    /// <summary>
    /// Records that the document was just written to <paramref name="path"/>.
    /// </summary>
    internal void mark_saved(string path, string content)
    {
        this.path = path;
        this.saved_content = content;
        this.dirty = false;
    }

    /// <summary>
    /// Appends a root node. The caller owns the node and the edit that added it.
    /// </summary>
    internal void add_root(document_node node)
    {
        this.root_nodes.Add(node);
    }

    internal void insert_root(int index, document_node node)
    {
        this.root_nodes.Insert(Math.Clamp(index, 0, this.root_nodes.Count), node);
    }

    /// <summary>
    /// Removes a root node, returning false when it was not a root of this document.
    /// </summary>
    internal bool remove_root(document_node node)
    {
        return this.root_nodes.Remove(node);
    }

    internal document_node? find_runtime(node_id id)
    {
        return id.is_valid ? find_runtime_in(this.root_nodes, id) : null;
    }

    private static document_node? find_runtime_in(IReadOnlyList<document_node> nodes, node_id id)
    {
        foreach (var node in nodes) {
            if (node.runtime.Equals(id)) {
                return node;
            }
            var found = find_runtime_in(node.children, id);
            if (found != null) {
                return found;
            }
        }
        return null;
    }

    private static document_node? find_in(IReadOnlyList<document_node> nodes, Guid uid)
    {
        foreach (var node in nodes) {
            if (node.uid == uid) {
                return node;
            }
            var found = find_in(node.children, uid);
            if (found != null) {
                return found;
            }
        }
        return null;
    }
}
