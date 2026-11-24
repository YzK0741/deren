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
    internal string? subscene;
    internal document_node? instance_parent;
    internal document_node? instance_root;
    internal IReadOnlyDictionary<Guid, Guid>? instance_node_ids;

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
    /// The environment lane this node's materials sample: 0 is the scene-wide map and 1..15 are the
    /// per-scene custom lanes the document stores. Written to the document so a saved scene keeps
    /// the look it was authored with.
    /// </summary>
    internal int environment;

    internal point_light_component? light;
    internal audio_source_component? audio_source;
    internal bool collision_box;
    /// <summary>
    /// True when the solid box follows this node's world transform instead of the transform it had
    /// when the scene loaded. Moving solids cost one matrix rebuild per sweep; static ones reuse the
    /// inverse captured at load.
    /// </summary>
    internal bool moving_solid;
    internal bool box_trigger;
    internal bool trigger_target;
    internal bool camera;
    internal bool main_camera;
    internal float camera_fov = scene_cameras.default_fov;
    internal instance_material? material;

    /// <summary>
    /// The child nodes; the list order is the outliner order and the runtime creation order.
    /// </summary>
    internal readonly List<document_node> children = [];

    /// <summary>
    /// The runtime node created for this document node, or a default handle while detached.
    /// </summary>
    internal deren.node_id runtime;
}
