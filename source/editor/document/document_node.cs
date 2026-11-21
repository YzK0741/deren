using System;
using System.Collections.Generic;

namespace deren.editor;

/// <summary>
/// One node of an edited scene document.
/// </summary>
/// <remarks>
/// A document node mirrors one runtime node: <see cref="document_io"/> writes its editable values to
/// the document file and <see cref="scene_sync"/> creates the matching runtime node while the
/// document is attached to a live scene tree. The document owns the tree shape and the runtime
/// handle only; nothing here runs across frames and every member is used on one thread.
/// </remarks>
internal sealed class document_node
{
    /// <summary>
    /// The stable node identity, written to the document file and captured by undo commands.
    /// </summary>
    internal Guid uid = Guid.NewGuid();

    /// <summary>
    /// The node name shown by the outliner. Renaming reaches the runtime scene through
    /// <see cref="scene_sync"/>.
    /// </summary>
    internal string name = string.Empty;

    /// <summary>
    /// The asset library id this node imports, or null when the node is an empty transform node.
    /// </summary>
    internal string? asset;

    /// <summary>
    /// The renderer pipeline applied to this node, or null when the asset default applies.
    /// </summary>
    internal string? pipeline;

    /// <summary>The optional assembly.dll:Namespace.Type behaviour played on this node.</summary>
    internal string? script;
    internal SortedDictionary<string, string> script_parameters = new(StringComparer.Ordinal);

    /// <summary>
    /// The local transform relative to the parent node.
    /// </summary>
    internal deren.transform_value local = new() {
        scale = new deren.vector_3 { x = 1.0f, y = 1.0f, z = 1.0f },
    };

    /// <summary>
    /// True while the node and its children are rendered.
    /// </summary>
    internal bool visible = true;

    /// <summary>
    /// The application flags carried by the runtime node.
    /// </summary>
    internal uint flags;

    /// <summary>
    /// The child nodes; the list order is the outliner order and the runtime creation order.
    /// </summary>
    internal readonly List<document_node> children = [];

    /// <summary>
    /// The runtime node created for this document node, or a default handle while detached.
    /// </summary>
    internal deren.node_id runtime;
}
