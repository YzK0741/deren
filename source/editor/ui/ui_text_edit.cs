using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace deren.editor;

/// <summary>
/// Editing state of one text field: the string, a caret and an anchor, both indices into the string
/// snapped to grapheme boundaries. Every edit is one undo step, because a field is edited by gestures
/// (a typed run, a paste, a composition commit), never by single code units - that is the same rule
/// the Dope Sheet follows for a key drag.
///
/// The caret stores a string index, never a pixel: layout asks <see cref="caret_x(string,float)"/> for
/// a position and hit testing asks <see cref="caret_at(string,float,float)"/> for an index, so a resize,
/// a scale change or a font swap cannot move the caret to a different character.
/// </summary>
internal sealed unsafe class ui_text_edit {
    private readonly List<snapshot> undo_history = [];
    private readonly List<snapshot> redo_history = [];
    private int history_capacity = 64;

    /// <summary>The text being edited.</summary>
    internal string text { get; private set; } = string.Empty;

    /// <summary>The caret index, always on a grapheme boundary.</summary>
    internal int caret { get; private set; }

    /// <summary>The selection anchor; equal to the caret when nothing is selected.</summary>
    internal int anchor { get; private set; }

    /// <summary>Whether the field has the keyboard focus; a field without focus ignores edits.</summary>
    internal bool focused { get; set; }

    /// <summary>Text of the composition in progress, drawn before the caret as an underline.</summary>
    internal string preedit { get; private set; } = string.Empty;

    /// <summary>Where inside <see cref="preedit"/> the input method wants the caret.</summary>
    internal int preedit_caret { get; private set; }

    internal bool has_selection => caret != anchor;

    internal bool can_undo => undo_history.Count > 0;

    internal bool can_redo => redo_history.Count > 0;

    internal bool dirty { get; private set; }

    internal (int start, int end) selection => caret < anchor ? (caret, anchor) : (anchor, caret);

    internal void reset(string value) {
        text = value;
        caret = value.Length;
        anchor = caret;
        preedit = string.Empty;
        preedit_caret = 0;
        dirty = false;
        undo_history.Clear();
        redo_history.Clear();
    }

    // --- grapheme walking -------------------------------------------------------------------

    /// <summary>
    /// The grapheme boundaries of the text, always starting with 0 and ending with the length, so a
    /// caret can never land between a base character and its combining mark.
    /// </summary>
    internal static int[] boundaries(string value) {
        if (value.Length == 0) { return [0]; }
        var starts = StringInfo.ParseCombiningCharacters(value);
        var result = new int[starts.Length + 1];
        Array.Copy(starts, result, starts.Length);
        result[starts.Length] = value.Length;
        return result;
    }

    private int snap(int index, bool forward) {
        var stops = boundaries(text);
        index = Math.Clamp(index, 0, text.Length);
        for (var i = 0; i < stops.Length; i++) {
            if (stops[i] == index) { return index; }
            if (stops[i] > index) { return forward ? stops[i] : stops[i - 1]; }
        }
        return text.Length;
    }

    private int previous_boundary(int index) {
        var stops = boundaries(text);
        var result = 0;
        foreach (var stop in stops) {
            if (stop >= index) { break; }
            result = stop;
        }
        return result;
    }

    private int next_boundary(int index) {
        foreach (var stop in boundaries(text)) {
            if (stop > index) { return stop; }
        }
        return text.Length;
    }

    /// <summary>Skips back over one word: the previous boundary that follows a non-space run.</summary>
    private int previous_word(int index) {
        var stops = boundaries(text);
        var position = index;
        for (var i = stops.Length - 1; i >= 0; i--) {
            if (stops[i] >= position) { continue; }
            var boundary = stops[i];
            if (boundary == 0) { return 0; }
            var before = text[stops[i - 1]];
            if (!char.IsWhiteSpace(before) && char.IsWhiteSpace(text[boundary - 1])) { return boundary; }
            position = boundary;
        }
        return 0;
    }

    private int next_word(int index) {
        var stops = boundaries(text);
        for (var i = 0; i < stops.Length; i++) {
            if (stops[i] <= index) { continue; }
            if (stops[i] == text.Length) { return text.Length; }
            if (char.IsWhiteSpace(text[stops[i] - 1])) { return stops[i]; }
        }
        return text.Length;
    }

    // --- caret movement ---------------------------------------------------------------------

    internal void move(int direction, bool select, bool by_word) {
        var target = direction < 0
            ? by_word ? previous_word(caret) : previous_boundary(caret)
            : by_word ? next_word(caret) : next_boundary(caret);
        caret = target;
        if (!select) { anchor = caret; }
        preedit = string.Empty;
        preedit_caret = 0;
    }

    internal void move_to(int index, bool select) {
        caret = snap(index, false);
        if (!select) { anchor = caret; }
        preedit = string.Empty;
        preedit_caret = 0;
    }

    internal void move_home(bool select) {
        caret = 0;
        if (!select) { anchor = 0; }
    }

    internal void move_end(bool select) {
        caret = text.Length;
        if (!select) { anchor = caret; }
    }

    internal void select_all() {
        anchor = 0;
        caret = text.Length;
    }

    // --- editing ----------------------------------------------------------------------------

    private void push_history() {
        undo_history.Add(new snapshot(text, caret, anchor));
        if (undo_history.Count > history_capacity) { undo_history.RemoveAt(0); }
        redo_history.Clear();
    }

    private void replace_selection(string replacement, int new_caret) {
        var (start, end) = selection;
        text = string.Concat(text.AsSpan(0, start), replacement, text.AsSpan(end));
        caret = new_caret;
        anchor = new_caret;
        dirty = true;
        preedit = string.Empty;
        preedit_caret = 0;
    }

    /// <summary>Inserts one typed code point, replacing the selection. Returns false for control codes.</summary>
    internal bool insert(uint codepoint) {
        if (!focused || preedit.Length > 0 || codepoint < 0x20U || codepoint == 0x7FU) { return false; }
        var value = char.ConvertFromUtf32((int)codepoint);
        push_history();
        var (start, _) = selection;
        replace_selection(value, start + value.Length);
        return true;
    }

    /// <summary>
    /// Inserts a whole run as ONE undo step: typed runs from an input method and pastes are single
    /// gestures, so one undo must remove all of it.
    /// </summary>
    internal bool insert_run(string value) {
        if (!focused || string.IsNullOrEmpty(value)) { return false; }
        var cleaned = new StringBuilder(value.Length);
        foreach (var rune in value.EnumerateRunes()) {
            if (rune.Value < 0x20U || rune.Value == 0x7FU) { continue; }
            cleaned.Append(char.ConvertFromUtf32(rune.Value));
        }
        if (cleaned.Length == 0) { return false; }
        push_history();
        var (start, _) = selection;
        replace_selection(cleaned.ToString(), start + cleaned.Length);
        return true;
    }

    internal bool backspace() {
        if (!focused) { return false; }
        if (has_selection) {
            push_history();
            replace_selection(string.Empty, selection.start);
            return true;
        }
        if (caret == 0) { return false; }
        var start = previous_boundary(caret);
        push_history();
        text = string.Concat(text.AsSpan(0, start), text.AsSpan(caret));
        caret = start;
        anchor = start;
        dirty = true;
        return true;
    }

    internal bool delete_forward() {
        if (!focused) { return false; }
        if (has_selection) {
            push_history();
            replace_selection(string.Empty, selection.start);
            return true;
        }
        if (caret >= text.Length) { return false; }
        var end = next_boundary(caret);
        push_history();
        text = string.Concat(text.AsSpan(0, caret), text.AsSpan(end));
        anchor = caret;
        dirty = true;
        return true;
    }

    internal bool try_undo() {
        if (undo_history.Count == 0) { return false; }
        redo_history.Add(new snapshot(text, caret, anchor));
        var previous = undo_history[^1];
        undo_history.RemoveAt(undo_history.Count - 1);
        text = previous.text;
        caret = Math.Clamp(previous.caret, 0, text.Length);
        anchor = Math.Clamp(previous.anchor, 0, text.Length);
        dirty = true;
        preedit = string.Empty;
        return true;
    }

    internal bool try_redo() {
        if (redo_history.Count == 0) { return false; }
        undo_history.Add(new snapshot(text, caret, anchor));
        var next = redo_history[^1];
        redo_history.RemoveAt(redo_history.Count - 1);
        text = next.text;
        caret = Math.Clamp(next.caret, 0, text.Length);
        anchor = Math.Clamp(next.anchor, 0, text.Length);
        dirty = true;
        return true;
    }

    /// <summary>Copies the selection (or the whole text when empty) and optionally removes it.</summary>
    internal string copy(bool cut) {
        var (start, end) = has_selection ? selection : (0, text.Length);
        var value = text[start..end];
        if (cut && has_selection && focused) {
            push_history();
            replace_selection(string.Empty, start);
        }
        return value;
    }

    // --- composition ------------------------------------------------------------------------

    internal void set_preedit(string value, int caret_in_preedit) {
        preedit = value ?? string.Empty;
        preedit_caret = Math.Clamp(caret_in_preedit, 0, preedit.Length);
    }

    internal void clear_preedit() {
        preedit = string.Empty;
        preedit_caret = 0;
    }

    // --- layout -----------------------------------------------------------------------------

    /// <summary>The caret x offset inside a field whose text starts at 0 and is shaped at @p size.</summary>
    internal float caret_x(float size, int index) {
        var shaped = ui_text_shape.shape(text, size, out _);
        if (shaped.Length == 0) { return 0.0F; }
        var bytes = Encoding.UTF8.GetByteCount(text.AsSpan(0, Math.Clamp(index, 0, text.Length)));
        var x = 0.0F;
        foreach (var glyph in shaped) {
            if (glyph.cluster >= (uint)bytes) { break; }
            x += glyph.advance;
        }
        return x;
    }

    internal float caret_x(float size) => caret_x(size, caret);

    /// <summary>The index of the grapheme boundary nearest to @p x inside the shaped run.</summary>
    internal int caret_at(float x, float size) {
        var shaped = ui_text_shape.shape(text, size, out _);
        if (shaped.Length == 0) { return x <= 0.0F ? 0 : text.Length; }
        var position = 0.0F;
        var best = 0;
        var best_distance = MathF.Abs(x);
        foreach (var glyph in shaped) {
            // Clusters are byte offsets; convert the boundary that starts this glyph back to an index.
            var index = char_index_at_byte((int)glyph.cluster);
            if (MathF.Abs(x - position) < best_distance) {
                best_distance = MathF.Abs(x - position);
                best = index;
            }
            position += glyph.advance;
        }
        if (MathF.Abs(x - position) < best_distance) {
            best = text.Length;
        }
        return snap(best, false);
    }

    private int char_index_at_byte(int byte_offset) {
        var consumed = 0;
        var index = 0;
        while (index < text.Length && consumed < byte_offset) {
            consumed += Encoding.UTF8.GetByteCount(text.AsSpan(index, 1));
            index++;
        }
        return index;
    }

    private readonly struct snapshot(string text, int caret, int anchor) {
        internal readonly string text = text;
        internal readonly int caret = caret;
        internal readonly int anchor = anchor;
    }
}
