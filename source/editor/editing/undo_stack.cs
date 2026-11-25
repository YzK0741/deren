using System;
using System.Collections.Generic;

namespace deren.editor;

/// <summary>
/// A bounded undo and redo history for one document.
/// </summary>
/// <remarks>
/// Pushing clears the redo side, so the history is linear: undo and redo walk the same commands back
/// and forth. The history keeps at most <see cref="capacity"/> commands and drops the oldest one
/// first. Every member runs on the thread that also runs the script callbacks.
/// </remarks>
internal sealed class undo_stack
{
    /// <summary>
    /// The number of commands the history keeps.
    /// </summary>
    internal const int capacity = 64;

    private readonly List<i_undo_command> undo_entries = [];
    private readonly List<i_undo_command> redo_entries = [];

    /// <summary>
    /// True while <see cref="try_undo"/> can move the document back.
    /// </summary>
    internal bool can_undo => this.undo_entries.Count > 0;

    /// <summary>
    /// True while <see cref="try_redo"/> can move the document forward again.
    /// </summary>
    internal bool can_redo => this.redo_entries.Count > 0;

    /// <summary>
    /// The label of the command <see cref="try_undo"/> runs next, or null when the history is empty.
    /// </summary>
    internal string? next_undo_label => this.undo_entries.Count > 0 ? this.undo_entries[^1].label : null;

    /// <summary>
    /// How many steps can still be undone; the selftest uses it to prove that a gesture pushed one step.
    /// </summary>
    internal int depth => this.undo_entries.Count;

    /// <summary>
    /// Records one finished edit. The oldest command is dropped once the history is full.
    /// </summary>
    internal void push(i_undo_command command)
    {
        ArgumentNullException.ThrowIfNull(command);
        this.undo_entries.Add(command);
        if (this.undo_entries.Count > capacity) {
            this.undo_entries.RemoveAt(0);
        }
        this.redo_entries.Clear();
    }

    /// <summary>
    /// Undoes the newest command. Returns false when there is nothing to undo, or when the command
    /// could not be reverted - the entry then stays on the undo side, so the history never claims a
    /// step the document did not take.
    /// </summary>
    internal bool try_undo()
    {
        if (this.undo_entries.Count == 0) {
            return false;
        }
        var command = this.undo_entries[^1];
        if (!command.undo()) {
            return false;
        }
        this.undo_entries.RemoveAt(this.undo_entries.Count - 1);
        this.redo_entries.Add(command);
        return true;
    }

    /// <summary>
    /// Redoes the command that was undone last. Returns false when there is nothing to redo, or when
    /// the command could not be reapplied - the entry then stays on the redo side.
    /// </summary>
    internal bool try_redo()
    {
        if (this.redo_entries.Count == 0) {
            return false;
        }
        var command = this.redo_entries[^1];
        if (!command.redo()) {
            return false;
        }
        this.redo_entries.RemoveAt(this.redo_entries.Count - 1);
        this.undo_entries.Add(command);
        return true;
    }

    /// <summary>
    /// Drops the whole history, for example when another document is opened.
    /// </summary>
    internal void clear()
    {
        this.undo_entries.Clear();
        this.redo_entries.Clear();
    }
}
