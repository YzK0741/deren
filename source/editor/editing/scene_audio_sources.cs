using System;
using System.Collections.Generic;
using deren;
using deren.scripting;

namespace deren.editor;

internal sealed class scene_audio_sources : IDisposable {
    private readonly scene_tree tree;
    private readonly Dictionary<node_id, audio_source_component> authored = [];
    private readonly Dictionary<node_id, audio> players = [];
    private readonly List<node_id> removed = [];

    internal scene_audio_sources(scene_document document, scene_tree tree) {
        this.tree = tree;
        collect(document.roots);
    }

    private void collect(IReadOnlyList<document_node> nodes) {
        foreach (var node in nodes) {
            if (node.runtime.is_valid && node.audio_source is { file.Length: > 0 } source) {
                authored.Add(node.runtime, source);
            }
            collect(node.children);
        }
    }

    internal bool try_play(node_id node, bool restart) {
        if (!authored.TryGetValue(node, out var source) || !tree.try_get_transform(node, out _)) { return false; }
        if (!players.TryGetValue(node, out var player)) {
            if (!source.try_create(managed_bootstrap.project_directory, out player, out var status) || player == null) {
                host_logger.write($"Scene audio: could not load '{source.file}' ({status})");
                return false;
            }
            players.Add(node, player);
        }
        return (!restart || player.stop()) && player.play();
    }

    internal bool try_stop(node_id node) {
        if (!authored.ContainsKey(node) || !tree.try_get_transform(node, out _)) { return false; }
        return !players.TryGetValue(node, out var player) || player.stop();
    }

    internal void release_removed_nodes() {
        foreach (var node in players.Keys) {
            if (!tree.try_get_transform(node, out _)) { removed.Add(node); }
        }
        foreach (var node in removed) {
            if (players.Remove(node, out var player)) { player.Dispose(); }
        }
        removed.Clear();
    }

    public void Dispose() {
        foreach (var player in players.Values) { player.Dispose(); }
        players.Clear();
        authored.Clear();
    }
}
