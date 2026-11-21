using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using deren.scripting;

namespace deren.editor {
    internal sealed unsafe partial class editor_host {
        private Guid outliner_anchor;
        private Guid outliner_press;
        private Vector2 outliner_pointer;
        private uint outliner_modifiers;
        private bool outliner_dragging;

        private readonly record struct hierarchy_slot(document_node? parent, int index, transform_value local);
        private readonly record struct hierarchy_move(document_node node, hierarchy_slot before, hierarchy_slot after);

        private bool handle_outliner_control(string id) {
            if (!outliner_panel.try_row(id, out var uid, out var part) ||
                document?.find(uid) is not { } node || outliner == null) {
                return false;
            }
            if (part == "_vis") {
                set_node_visibility(node, !node.visible);
            } else if (part == "_fold" && node.children.Count != 0) {
                outliner.toggle(uid);
                refresh_panels();
            }
            return true;
        }

        private void reset_outliner_drag() {
            outliner_press = Guid.Empty;
            outliner_dragging = false;
            outliner?.highlight_drop(Guid.Empty, false);
        }

        private bool handle_outliner_pointer(rml_event input) {
            bool row = outliner_panel.try_row(input.id, out var uid, out var part);
            bool node_hit = row && part is "" or "_name";
            var pointer = new Vector2(input.x, input.y);
            if (input.type == rml_event_type.pointer_cancel) {
                reset_outliner_drag();
                return false;
            }
            if (input.type == rml_event_type.pointer_down && input.button == 0 && node_hit && open_menu.Length == 0) {
                outliner_press = uid;
                outliner_pointer = pointer;
                outliner_modifiers = input.modifiers;
                return true;
            }
            if (outliner_press == Guid.Empty) {
                return false;
            }
            bool moved = Vector2.DistanceSquared(pointer, outliner_pointer) > 16;
            if (input.type == rml_event_type.pointer_move) {
                if (outliner_modifiers == 0 && moved) {
                    outliner_dragging = true;
                }
                if (outliner_dragging) {
                    outliner?.highlight_drop(row ? uid : Guid.Empty, input.id == "outliner_root_drop");
                }
                return true;
            }
            if (input.type != rml_event_type.pointer_up || input.button != 0) {
                return false;
            }
            var picked = outliner_press;
            if (outliner_dragging || (moved && outliner_modifiers == 0)) {
                var target = row ? document?.find(uid) : null;
                if (target != null || input.id == "outliner_root_drop") {
                    if (selection_state?.contains(picked) == false) {
                        selection_state.select(picked);
                        outliner_anchor = picked;
                    }
                    reparent_selection(target);
                }
            } else if (!moved && node_hit && uid == picked) {
                select_outliner_row(uid, outliner_modifiers);
            }
            reset_outliner_drag();
            return true;
        }

        private void select_outliner_row(Guid uid, uint modifiers) {
            const uint ctrl = 1u << 0, shift = 1u << 1;
            if (selection_state == null || outliner == null) {
                return;
            }
            if ((modifiers & shift) != 0) {
                selection_state.replace(outliner.range(outliner_anchor, uid), uid);
            } else {
                if ((modifiers & ctrl) != 0) {
                    selection_state.toggle(uid);
                } else {
                    selection_state.select(uid);
                }
                outliner_anchor = uid;
            }
            refresh_panels();
        }

        private void reveal_in_outliner(document_node node) {
            for (document_node? current = node; current != null && try_parent(current, out var parent); current = parent) {
                if (parent != null) {
                    outliner?.expand(parent.uid);
                }
            }
        }

        private List<document_node> selection_roots() {
            var nodes = new List<document_node>();
            if (document != null && selection_state != null) {
                collect_selection_roots(document.roots, selection_state, nodes);
            }
            return nodes;
        }

        private static void collect_selection_roots(IReadOnlyList<document_node> nodes, selection picked,
                                                    List<document_node> roots) {
            foreach (var node in nodes) {
                if (picked.contains(node.uid)) {
                    roots.Add(node);
                } else {
                    collect_selection_roots(node.children, picked, roots);
                }
            }
        }

        private bool reparent_selection(document_node? parent) {
            if (document == null || tree == null) {
                return false;
            }
            var selected = selection_roots();
            for (var ancestor = parent; ancestor != null; ) {
                if (selected.Contains(ancestor) || !try_parent(ancestor, out ancestor)) {
                    notice = "Cannot move a group into its own subtree";
                    return false;
                }
            }
            if (!Matrix4x4.Invert(world_matrix(parent), out var inverse)) {
                notice = "The target transform cannot be inverted";
                return false;
            }
            var moves = new List<hierarchy_move>();
            int new_index = parent?.children.Count ?? document.roots.Count;
            foreach (var node in selected) {
                if (!locate_node(node, out var old_parent, out int old_index) || old_parent == parent) {
                    continue;
                }
                if (!try_local_pose(world_matrix(node) * inverse, out var local)) {
                    notice = "Cannot preserve the full transform under this parent";
                    return false;
                }
                moves.Add(new hierarchy_move(node, new(old_parent, old_index, node.local), new(parent, new_index++, local)));
            }
            if (moves.Count == 0 || !apply_hierarchy_moves(moves, forward: true)) {
                return false;
            }
            if (parent != null) {
                outliner?.expand(parent.uid);
            }
            record_undo("Reparent Nodes", () => apply_hierarchy_moves(moves, forward: false),
                        () => apply_hierarchy_moves(moves, forward: true));
            refresh_panels(force_details: true);
            return true;
        }

        private bool apply_hierarchy_moves(List<hierarchy_move> moves, bool forward) {
            for (int index = 0; index < moves.Count; ++index) {
                var move = moves[index];
                if (move_runtime(move.node, forward ? move.before : move.after, forward ? move.after : move.before)) {
                    continue;
                }
                for (int previous = index - 1; previous >= 0; --previous) {
                    var undo = moves[previous];
                    if (!move_runtime(undo.node, forward ? undo.after : undo.before, forward ? undo.before : undo.after)) {
                        host_logger.write($"Editor: could not roll back hierarchy change for {undo.node.name}");
                    }
                }
                notice = $"Cannot reparent {move.node.name}";
                return false;
            }
            // Remove every source first so the saved sibling indices apply unchanged.
            foreach (var move in moves) {
                remove_document_node(forward ? move.before.parent : move.after.parent, move.node);
            }
            foreach (var move in moves) {
                var slot = forward ? move.after : move.before;
                move.node.local = slot.local;
                insert_document_node(slot.parent, slot.index, move.node);
            }
            mark_dirty_and_refresh();
            return true;
        }

        private bool move_runtime(document_node node, hierarchy_slot from, hierarchy_slot to) {
            if (tree == null || !tree.try_reparent(node.runtime, to.parent?.runtime ?? default, keep_transform: false)) {
                return false;
            }
            if (tree.try_set_transform(node.runtime, to.local)) {
                return true;
            }
            if (!tree.try_reparent(node.runtime, from.parent?.runtime ?? default, keep_transform: false) ||
                !tree.try_set_transform(node.runtime, from.local)) {
                host_logger.write($"Editor: could not restore the runtime parent of {node.name}");
            }
            return false;
        }

        private Matrix4x4 world_matrix(document_node? node) {
            var result = Matrix4x4.Identity;
            while (node != null) {
                result *= viewport_gizmo.local_matrix(node.local);
                try_parent(node, out node);
            }
            return result;
        }

        private static bool try_local_pose(Matrix4x4 matrix, out transform_value pose) {
            pose = default;
            if (!Matrix4x4.Decompose(matrix, out var scale, out var rotation, out var position) ||
                MathF.Min(MathF.Abs(scale.X), MathF.Min(MathF.Abs(scale.Y), MathF.Abs(scale.Z))) < 1e-7f) {
                return false;
            }
            pose = new transform_value {
                position = new vector_3 { x = position.X, y = position.Y, z = position.Z },
                rotation_degrees = viewport_gizmo.euler(Matrix4x4.CreateFromQuaternion(rotation)),
                scale = new vector_3 { x = scale.X, y = scale.Y, z = scale.Z },
            };
            return near_matrix(matrix, viewport_gizmo.local_matrix(pose));
        }

        private static bool near_matrix(Matrix4x4 a, Matrix4x4 b) {
            for (int row = 0; row < 4; ++row) {
                for (int col = 0; col < 4; ++col) {
                    float x = a[row, col], y = b[row, col];
                    if (!float.IsFinite(x) || !float.IsFinite(y) ||
                        MathF.Abs(x - y) > 0.0001f * MathF.Max(1, MathF.Max(MathF.Abs(x), MathF.Abs(y)))) {
                        return false;
                    }
                }
            }
            return true;
        }

        private bool duplicate_selection() {
            if (selection_state == null || document == null) {
                return false;
            }
            var previous = selection_state.all;
            Guid primary = selection_state.primary;
            var copies = new List<(document_node node, document_node? parent)>();
            foreach (var node in selection_roots()) {
                if (!try_parent(node, out var parent)) {
                    return false;
                }
                var clone = clone_subtree(node);
                clone.name += " Copy";
                copies.Add((clone, parent));
            }
            if (copies.Count == 0 || !insert_copies(copies)) {
                return false;
            }
            record_undo("Duplicate Nodes", () => {
                var removed = new List<(document_node node, document_node? parent)>();
                foreach (var copy in copies) {
                    if (!destroy_runtime(copy.node)) {
                        insert_copies(removed);
                        return false;
                    }
                    remove_document_node(copy.parent, copy.node);
                    removed.Add(copy);
                }
                selection_state.replace(previous, primary);
                mark_dirty_and_refresh();
                return true;
            }, () => insert_copies(copies));
            return true;
        }

        private bool insert_copies(List<(document_node node, document_node? parent)> copies) {
            if (document == null || selection_state == null) {
                return false;
            }
            var inserted = new List<(document_node node, document_node? parent)>();
            foreach (var (node, parent) in copies) {
                if (!create_subtree(parent, node)) {
                    if (node.runtime.is_valid) { destroy_runtime(node); }
                    foreach (var (created, owner) in inserted) {
                        destroy_runtime(created);
                        remove_document_node(owner, created);
                    }
                    notice = $"Cannot duplicate {node.name}";
                    return false;
                }
                insert_document_node(parent, parent?.children.Count ?? document.roots.Count, node);
                inserted.Add((node, parent));
            }
            if (copies.Count != 0) {
                selection_state.replace(copies.Select(copy => copy.node.uid), copies[^1].node.uid);
            }
            mark_dirty_and_refresh();
            return true;
        }

        private static document_node clone_subtree(document_node source) {
            var copy = new document_node {
                name = source.name,
                asset = source.asset,
                pipeline = source.pipeline,
                script = source.script,
                script_parameters = new(source.script_parameters, StringComparer.Ordinal),
                local = source.local,
                visible = source.visible,
                flags = source.flags,
            };
            foreach (var child in source.children) {
                copy.children.Add(clone_subtree(child));
            }
            return copy;
        }
    }
}
