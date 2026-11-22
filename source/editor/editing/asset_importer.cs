using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using deren.scripting;

namespace deren.editor {
    internal static class asset_importer {
        internal static string suggested_id(string source) {
            var stem = Path.GetFileNameWithoutExtension(source);
            var cleaned = stem.ToLowerInvariant().ToCharArray();
            for (var index = 0; index < cleaned.Length; ++index) {
                if (!char.IsAsciiLetterOrDigit(cleaned[index]) && cleaned[index] is not '-' and not '_') {
                    cleaned[index] = '-';
                }
            }
            var id = new string(cleaned).Trim('-', '_');
            return id.Length == 0 ? "model" : id;
        }

        internal static bool try_import(asset_authoring catalog, string library, string source, string id, out string error) {
            error = string.Empty;
            string? staged = null;
            string? published = null;
            try {
                source = Path.GetFullPath(source);
                if (!File.Exists(source)) {
                    throw new IOException("The source file does not exist.");
                }
                if (id.Length == 0 || id[0] == '-' || id[0] == '_' ||
                    !Array.TrueForAll(id.ToCharArray(), c => char.IsAsciiLetterOrDigit(c) || c is '-' or '_')) {
                    throw new ArgumentException("Asset ID must start with a letter or number and use letters, numbers, '-' or '_'.");
                }

                var target = Path.Combine(library, id);
                Directory.CreateDirectory(library);
                if (catalog.refresh() != script_status.success) {
                    throw new IOException("The asset library could not be refreshed.");
                }
                var ids = asset_manager.list(out var list_status);
                if (list_status != script_status.success) {
                    throw new IOException($"Could not read the asset catalogue ({list_status}).");
                }
                if (Array.Exists(ids, other => other.Equals(id, StringComparison.OrdinalIgnoreCase)) ||
                    Directory.Exists(target) || File.Exists(target)) {
                    throw new IOException($"Asset ID '{id}' is already in use. Choose a different ID.");
                }

                using var document = read_model(source);
                var model = document.RootElement;
                if (!model.TryGetProperty("asset", out var header) ||
                    !header.TryGetProperty("version", out var version) || version.GetString() != "2.0") {
                    throw new InvalidDataException("The model is not glTF 2.0.");
                }
                var files = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { source };
                var source_root = Path.GetDirectoryName(source)!;
                collect_files(model, "buffers", source_root, files);
                collect_files(model, "images", source_root, files);

                // Hidden directories are skipped by the catalogue even if another editor refreshes
                // during a copy. Directory.Move publishes the model and manifest together.
                staged = Path.Combine(library, $".import-{Guid.NewGuid():N}");
                Directory.CreateDirectory(staged);
                foreach (var file in files) {
                    var relative = Path.GetRelativePath(source_root, file);
                    var destination = Path.Combine(staged, relative);
                    Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                    File.Copy(file, destination);
                }
                var filename = Path.GetFileName(source);
                var name = Path.GetFileNameWithoutExtension(source);
                File.WriteAllText(Path.Combine(staged, "asset.toml"),
                    $"id = {JsonSerializer.Serialize(id)}\nname = {JsonSerializer.Serialize(name)}\n" +
                    $"type = \"pbr\"\nmodel = {JsonSerializer.Serialize(filename)}\n");
                Directory.Move(staged, target);
                staged = null;
                published = target;
                var refreshed = catalog.refresh();
                if (refreshed != script_status.success) {
                    throw new IOException($"The asset library could not be refreshed ({refreshed}).");
                }
                if (!asset_manager.try_load(id, out var handle, out var status)) {
                    throw new InvalidDataException($"The engine could not load the imported model ({status}).");
                }
                asset_manager.release(handle);
                published = null;
                return true;
            } catch (Exception failure) when (failure is IOException or UnauthorizedAccessException or InvalidDataException or JsonException or
                                               ArgumentException or InvalidOperationException or NotSupportedException) {
                error = failure.Message;
            } finally {
                try {
                    if (staged != null && Directory.Exists(staged)) { Directory.Delete(staged, recursive: true); }
                    if (published != null) {
                        try {
                            Directory.Delete(published, recursive: true);
                        } finally {
                            catalog.refresh();
                        }
                    }
                } catch (Exception failure) when (failure is IOException or UnauthorizedAccessException) {
                    error += $" Cleanup failed: {failure.Message}";
                }
            }
            return false;
        }

        private static JsonDocument read_model(string source) {
            using var input = File.OpenRead(source);
            var extension = Path.GetExtension(source);
            if (extension.Equals(".gltf", StringComparison.OrdinalIgnoreCase)) {
                return JsonDocument.Parse(input);
            }
            if (!extension.Equals(".glb", StringComparison.OrdinalIgnoreCase)) {
                throw new InvalidDataException("Choose a .glb or .gltf model.");
            }
            Span<byte> header = stackalloc byte[20];
            input.ReadExactly(header);
            var length = BinaryPrimitives.ReadUInt32LittleEndian(header[12..16]);
            if (!header[..4].SequenceEqual("glTF"u8) ||
                BinaryPrimitives.ReadUInt32LittleEndian(header[4..8]) != 2 ||
                BinaryPrimitives.ReadUInt32LittleEndian(header[8..12]) != input.Length ||
                !header[16..].SequenceEqual("JSON"u8) || length == 0 ||
                length > input.Length - header.Length || length > int.MaxValue) {
                throw new InvalidDataException("The GLB header or JSON chunk is invalid.");
            }
            var json = new byte[(int)length];
            input.ReadExactly(json);
            return JsonDocument.Parse(json);
        }

        private static void collect_files(JsonElement model, string section, string folder, HashSet<string> files) {
            if (!model.TryGetProperty(section, out var values)) { return; }
            foreach (var item in values.EnumerateArray()) {
                if (!item.TryGetProperty("uri", out var value)) { continue; }
                var reference = value.GetString();
                if (reference?.StartsWith("data:", StringComparison.OrdinalIgnoreCase) == true) { continue; }
                if (string.IsNullOrEmpty(reference) || reference.Contains('?') || reference.Contains('#') ||
                    Uri.TryCreate(reference, UriKind.Absolute, out _)) {
                    throw new InvalidDataException($"'{reference}' must refer to a local model file.");
                }
                var relative = Uri.UnescapeDataString(reference);
                if (Path.IsPathRooted(relative) || relative.Contains(':')) {
                    throw new InvalidDataException($"'{reference}' must use a model-relative path.");
                }
                var source = Path.GetFullPath(relative, folder);
                var within = Path.GetRelativePath(folder, source);
                if (within == ".." || within.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal) ||
                    Path.IsPathRooted(within)) {
                    throw new InvalidDataException($"'{reference}' must stay inside the model's directory.");
                }
                if (!File.Exists(source)) {
                    throw new IOException($"Missing model dependency: {within}");
                }
                files.Add(source);
            }
        }
    }
}
