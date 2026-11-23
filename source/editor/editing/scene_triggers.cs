using System;
using System.Collections.Generic;
using System.Numerics;
using deren;

namespace deren.editor;

internal sealed class scene_triggers {
    private readonly record struct trigger_node(document_node node, int parent);
    private readonly record struct trigger_contact(int trigger, int target);

    private readonly List<trigger_node> nodes = [];
    private readonly List<int> volumes = [];
    private readonly List<int> targets = [];
    private readonly Matrix4x4[] matrices;
    private readonly bool[] valid;
    private HashSet<trigger_contact> active = [];
    private HashSet<trigger_contact> next = [];

    internal scene_triggers(scene_document document) {
        collect(document.roots, -1);
        matrices = new Matrix4x4[nodes.Count];
        valid = new bool[nodes.Count];
    }

    private void collect(IReadOnlyList<document_node> source, int parent) {
        foreach (var node in source) {
            int index = nodes.Count;
            nodes.Add(new trigger_node(node, parent));
            if (node.box_trigger) { volumes.Add(index); }
            if (node.trigger_target) { targets.Add(index); }
            collect(node.children, index);
        }
    }

    internal void update(scene_tree tree, Action<document_node, document_node, bool> notify) {
        if (volumes.Count == 0 || targets.Count == 0) { return; }
        for (int index = 0; index < nodes.Count; ++index) {
            var (node, parent) = nodes[index];
            valid[index] = false;
            if (!node.visible || (parent >= 0 && !valid[parent]) || !tree.try_get_transform(node.runtime, out var local)) {
                continue;
            }
            valid[index] = true;
            matrices[index] = viewport_gizmo.local_matrix(local);
            if (parent >= 0) { matrices[index] *= matrices[parent]; }
        }
        next.Clear();
        foreach (int volume in volumes) {
            if (!valid[volume] || !Matrix4x4.Invert(matrices[volume], out var world_to_box)) { continue; }
            foreach (int target in targets) {
                if (target == volume || !valid[target] || !contains(world_to_box, matrices[target].Translation)) { continue; }
                var pair = new trigger_contact(volume, target);
                next.Add(pair);
                if (!active.Contains(pair)) { notify(nodes[volume].node, nodes[target].node, true); }
            }
        }
        foreach (var pair in active) {
            if (!next.Contains(pair) && valid[pair.trigger]) {
                notify(nodes[pair.trigger].node, nodes[pair.target].node, false);
            }
        }
        (active, next) = (next, active);
    }

    internal static bool contains(Matrix4x4 world_to_box, Vector3 point) {
        var local = Vector3.Transform(point, world_to_box);
        return MathF.Abs(local.X) <= 0.5f && MathF.Abs(local.Y) <= 0.5f && MathF.Abs(local.Z) <= 0.5f;
    }
}
