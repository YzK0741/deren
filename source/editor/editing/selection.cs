using System;
using System.Collections.Generic;

namespace deren.editor;

/// <summary>
/// The selected nodes of one document, with the primary node the inspector shows.
/// </summary>
/// <remarks>
/// The selection stores uids only and never resolves them against a document, so the caller decides
/// what an unknown uid means. Every mutation raises <see cref="changed"/> exactly once.
/// </remarks>
internal sealed class selection
{
    private readonly List<Guid> uids = [];

    /// <summary>
    /// Raised after the selection changed.
    /// </summary>
    internal event Action? changed;

    /// <summary>
    /// The node the inspector shows, or <see cref="Guid.Empty"/> when nothing is selected.
    /// </summary>
    internal Guid primary;

    /// <summary>
    /// The selected uids in selection order, as a snapshot the caller may keep while the selection
    /// moves again.
    /// </summary>
    internal IReadOnlyCollection<Guid> all => [.. this.uids];

    /// <summary>
    /// Replaces the selection with <paramref name="uid"/>.
    /// </summary>
    internal void select(Guid uid)
    {
        this.uids.Clear();
        this.uids.Add(uid);
        this.primary = uid;
        this.changed?.Invoke();
    }

    /// <summary>
    /// Adds <paramref name="uid"/> when it is not selected and removes it otherwise. The primary
    /// moves to the toggled node on select and to the next remaining node on deselect.
    /// </summary>
    internal void toggle(Guid uid)
    {
        if (this.uids.Remove(uid)) {
            if (this.primary == uid) {
                this.primary = this.uids.Count > 0 ? this.uids[0] : Guid.Empty;
            }
        } else {
            this.uids.Add(uid);
            this.primary = uid;
        }
        this.changed?.Invoke();
    }

    /// <summary>
    /// Drops the whole selection. Does nothing when it is already empty.
    /// </summary>
    internal void clear()
    {
        if (this.uids.Count == 0 && this.primary == Guid.Empty) {
            return;
        }
        this.uids.Clear();
        this.primary = Guid.Empty;
        this.changed?.Invoke();
    }
}
