using System;
using System.Runtime.InteropServices;

namespace deren.editor;

/// <summary>
/// The system clipboard, for the text fields. Windows gives one string at a time (the unicode_text_format
/// format), which is exactly what a field needs; a field that fails here keeps working and simply
/// copies nothing, because a clipboard is a convenience, never a dependency of editing.
/// </summary>
internal static class ui_clipboard {
    private const uint unicode_text_format = 13;
    private const uint moveable_flags = 0x0002;

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool OpenClipboard(IntPtr owner);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool CloseClipboard();

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EmptyClipboard();

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr GetClipboardData(uint format);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SetClipboardData(uint format, IntPtr memory);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GlobalAlloc(uint flags, UIntPtr bytes);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GlobalFree(IntPtr memory);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr GlobalLock(IntPtr memory);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GlobalUnlock(IntPtr memory);

    /// <summary>Reads the clipboard text; an empty string when the clipboard is unavailable or empty.</summary>
    internal static string read() {
        if (!OpenClipboard(IntPtr.Zero)) { return string.Empty; }
        try {
            var handle = GetClipboardData(unicode_text_format);
            if (handle == IntPtr.Zero) { return string.Empty; }
            var pointer = GlobalLock(handle);
            if (pointer == IntPtr.Zero) { return string.Empty; }
            try {
                return Marshal.PtrToStringUni(pointer) ?? string.Empty;
            } finally {
                GlobalUnlock(handle);
            }
        } finally {
            CloseClipboard();
        }
    }

    /// <summary>Replaces the clipboard text. Returns false when the system refused the clipboard.</summary>
    internal static bool write(string text) {
        if (!OpenClipboard(IntPtr.Zero)) { return false; }
        try {
            if (!EmptyClipboard()) { return false; }
            var bytes = (UIntPtr)((uint)((text.Length + 1) * sizeof(char)));
            var handle = GlobalAlloc(moveable_flags, bytes);
            if (handle == IntPtr.Zero) { return false; }
            var pointer = GlobalLock(handle);
            if (pointer == IntPtr.Zero) {
                GlobalFree(handle);
                return false;
            }
            try {
                var buffer = new char[text.Length + 1];
                text.CopyTo(0, buffer, 0, text.Length);
                Marshal.Copy(buffer, 0, pointer, buffer.Length);
            } finally {
                GlobalUnlock(handle);
            }
            // The clipboard takes ownership of the block on success, so a failure must free it here.
            if (SetClipboardData(unicode_text_format, handle) == IntPtr.Zero) {
                GlobalFree(handle);
                return false;
            }
            return true;
        } finally {
            CloseClipboard();
        }
    }
}
