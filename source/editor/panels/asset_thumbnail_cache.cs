using System;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using deren.scripting;

namespace deren.editor {
    /// <summary>Runs one offscreen preview capture at a time and publishes the completed PNG.</summary>
    internal sealed class asset_thumbnail_cache : IDisposable {
        private readonly string project_directory;
        private Process? worker;
        private string? job_directory;
        private string? asset_id;

        internal asset_thumbnail_cache(string project_directory) {
            this.project_directory = Path.GetFullPath(project_directory);
        }

        internal string path_for(string asset) {
            var key = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(asset)));
            return Path.Combine(project_directory, "ui", "thumbnails", key + ".png");
        }

        internal bool begin(string asset, out string message) {
            if (worker != null) {
                message = $"Rendering thumbnail: {asset_id}";
                return false;
            }
            if (File.Exists(path_for(asset))) {
                message = $"Thumbnail already cached: {asset}";
                return false;
            }
            try {
                var executable = Environment.ProcessPath ??
                    throw new InvalidOperationException("The rendering executable path is unavailable.");
                var directory = Path.Combine(project_directory, "thumbnail-jobs", Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(directory);
                job_directory = directory;
                File.Copy(Path.Combine(project_directory, "editor_worker.toml"), Path.Combine(directory, "deren.toml"));
                File.WriteAllText(Path.Combine(directory, "scene.toml"), """
                    [paths]
                    screenshot_dir = "captures"

                    [render]
                    window_title = "deren asset thumbnail"
                    window_width = 256
                    window_height = 160
                    fxaa = true
                    """);

                var start = new ProcessStartInfo(executable) {
                    WorkingDirectory = directory,
                    UseShellExecute = false,
                    CreateNoWindow = true
                };
                start.ArgumentList.Add("run");
                start.ArgumentList.Add(directory);
                start.ArgumentList.Add("--editor-thumbnail");
                start.ArgumentList.Add(asset);
                worker = Process.Start(start) ?? throw new InvalidOperationException("The renderer did not start.");
                asset_id = asset;
                message = $"Rendering thumbnail: {asset}";
                return true;
            } catch (Exception error) {
                host_logger.write($"Editor thumbnail: {error.Message}");
                clear_job();
                message = $"Could not start the thumbnail renderer for {asset}";
                return false;
            }
        }

        internal bool poll(out string message) {
            message = string.Empty;
            if (worker == null || !worker.HasExited) {
                return false;
            }
            string asset = asset_id!;
            try {
                var captures = Path.Combine(job_directory!, "captures");
                var images = Directory.Exists(captures) ? Directory.GetFiles(captures, "screenshot_*.png") : [];
                if (worker.ExitCode != 0 || images.Length != 1) {
                    throw new InvalidOperationException($"Renderer exited with {worker.ExitCode} and wrote {images.Length} capture(s).");
                }
                var path = path_for(asset);
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.Move(images[0], path, overwrite: true);
                message = $"Thumbnail ready: {asset}";
            } catch (Exception error) {
                host_logger.write($"Editor thumbnail '{asset}': {error.Message}");
                message = $"Could not render thumbnail: {asset}";
            } finally {
                clear_job();
            }
            return true;
        }

        public void Dispose() {
            if (worker != null) {
                try {
                    if (!worker.HasExited) {
                        worker.Kill();
                    }
                    worker.WaitForExit();
                } catch (Exception error) {
                    host_logger.write($"Editor thumbnail: {error.Message}");
                }
            }
            clear_job();
        }

        private void clear_job() {
            worker?.Dispose();
            worker = null;
            asset_id = null;
            if (job_directory != null) {
                try {
                    var expected_parent = Path.Combine(project_directory, "thumbnail-jobs");
                    if (Path.GetDirectoryName(Path.GetFullPath(job_directory)) == expected_parent) {
                        Directory.Delete(job_directory, recursive: true);
                    }
                } catch (Exception error) {
                    host_logger.write($"Editor thumbnail cleanup: {error.Message}");
                }
                job_directory = null;
            }
        }
    }
}
