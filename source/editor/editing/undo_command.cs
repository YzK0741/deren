using System;

namespace deren.editor;

/// <summary>
/// One reversible edit, as stored by <see cref="undo_stack"/>.
/// </summary>
/// <remarks>
/// A command owns everything it needs to run both ways, so <see cref="undo"/> and <see cref="redo"/>
/// are safe to call any number of times and in either order. A command must not push another
/// command from <see cref="undo"/> or <see cref="redo"/>.
/// <para>
/// Both directions REPORT whether the document and the runtime accepted the change: a runtime
/// command can fail (a destroyed asset, a refused transform), and a history that stepped anyway
/// would leave the two sides disagreeing while the label claimed the edit had been reverted.
/// </para>
/// </remarks>
internal interface i_undo_command
{
    /// <summary>
    /// The short user-facing name shown next to undo and redo.
    /// </summary>
    string label { get; }

    /// <summary>
    /// Reverts the edit. Returns false when the document could not be put back, in which case the
    /// command must stay where it is in the history.
    /// </summary>
    bool undo();

    /// <summary>
    /// Reapplies the edit. Returns false when the document could not be moved forward, in which case
    /// the command must stay where it is in the history.
    /// </summary>
    bool redo();
}

/// <summary>
/// A command built from two delegates, for edits that need no dedicated class.
/// </summary>
internal sealed class delegate_undo_command : i_undo_command
{
    private readonly string text;
    private readonly Func<bool> undo_action;
    private readonly Func<bool> redo_action;

    /// <summary>
    /// Creates a command from its label and the two delegate bodies.
    /// </summary>
    internal delegate_undo_command(string label, Func<bool> undo, Func<bool> redo)
    {
        ArgumentNullException.ThrowIfNull(label);
        ArgumentNullException.ThrowIfNull(undo);
        ArgumentNullException.ThrowIfNull(redo);
        this.text = label;
        this.undo_action = undo;
        this.redo_action = redo;
    }

    /// <inheritdoc/>
    public string label => this.text;

    /// <inheritdoc/>
    public bool undo() => this.undo_action();

    /// <inheritdoc/>
    public bool redo() => this.redo_action();
}
