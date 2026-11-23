using System;
using System.IO;
using System.Linq;
using deren;
using deren.scripting;

namespace deren.editor;

internal readonly record struct audio_source_component(string file, float volume, bool loop) {
    internal static audio_source_component unassigned => new(string.Empty, 1, false);

    internal bool valid => volume is >= 0 and <= 1 && (file.Length == 0 || valid_reference(file));

    internal static bool valid_reference(string file) =>
        file.StartsWith("audio/", StringComparison.Ordinal) && !file.Contains((char)92) &&
        file.Split('/').All(part => part is not ("" or "." or "..") && part.IndexOfAny(Path.GetInvalidFileNameChars()) < 0) &&
        Path.GetExtension(file).ToLowerInvariant() is ".wav" or ".mp3" or ".flac" or ".ogg" or ".opus" or ".qoa";

    internal bool try_create(string project, out audio? player, out script_status status) {
        player = null;
        status = script_status.invalid_argument;
        if (!valid || file.Length == 0) { return false; }
        string path;
        try {
            string root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(project));
            path = Path.GetFullPath(file, root);
            var comparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
            for (string? current = path; current != null && !comparer.Equals(current, root);
                 current = Path.GetDirectoryName(current)) {
                if (File.GetAttributes(current).HasFlag(FileAttributes.ReparsePoint)) { return false; }
            }
        } catch (Exception error) when (error is IOException or ArgumentException or NotSupportedException or UnauthorizedAccessException) {
            status = script_status.asset_not_found;
            return false;
        }
        if (!audio.try_create_from_file(path, out player, out status) || player == null) { return false; }
        if (player.try_set_volume(volume, out status) && player.try_set_looping(loop, out status)) { return true; }
        player.Dispose();
        player = null;
        return false;
    }
}
