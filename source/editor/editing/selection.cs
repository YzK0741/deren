using System;
using System.Collections.Generic;

namespace deren.editor;

/// <summary>
/// The selected nodes of one document, with the primary node the inspector shows.
/// </summary>
/// <remarks>
/// The selection stores uids only and never resolves them against a document, so the caller decides
/// what an unknown uid means.
/// </remarks>
internal sealed class selection
{
    private readonly List<Guid> uids = [];

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
    }
}
