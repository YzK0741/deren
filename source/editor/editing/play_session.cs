using System;
using System.Diagnostics;
using System.IO;
using System.Text.Json;
using deren.scripting;

namespace deren.editor {
    internal sealed class play_session : IDisposable {
        private readonly string project_directory;
        private Process? process;
        private string? workspace;
        private DateTime stop_deadline;

        internal play_session(string project_directory) {
            this.project_directory = Path.GetFullPath(project_directory);
        }

        internal bool running => process != null;
        internal bool stopping => stop_deadline != default;

        internal bool start(scene_document document, orbit_camera camera, out string message) {
            if (running) {
                message = "A play session is already running";
                return false;
            }
            if (!document_io.try_serialize(document, out var copy, out var error)) {
                message = error ?? "Could not prepare the scene";
                return false;
            }
            try {
                workspace = Path.Combine(project_directory, "play-sessions", Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(workspace);
                var snapshot = Path.Combine(workspace, "play.scene.toml");
                File.WriteAllText(snapshot, copy);
                File.WriteAllText(Path.Combine(workspace, "camera.json"), JsonSerializer.Serialize(camera, camera_json));
                File.Copy(Path.Combine(project_directory, "editor_worker.toml"), Path.Combine(workspace, "deren.toml"));
                File.WriteAllText(Path.Combine(workspace, "scene.toml"), """
                    [paths]
                    screenshot_dir = "captures"

                    [render]
                    window_title = "deren - Play"
                    window_width = 1280
                    window_height = 720
                    fxaa = true
                    """);
                var executable = Environment.ProcessPath ??
                    throw new InvalidOperationException("The rendering executable path is unavailable.");
                var start_info = new ProcessStartInfo(executable) {
                    WorkingDirectory = workspace,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                };
                start_info.ArgumentList.Add("run");
                start_info.ArgumentList.Add(workspace);
                start_info.ArgumentList.Add("--editor-play");
                start_info.ArgumentList.Add(snapshot);
                process = Process.Start(start_info) ?? throw new InvalidOperationException("The play process did not start.");
                message = "Playing a scene copy";
                return true;
            } catch (Exception failure) {
                host_logger.write($"Editor play: {failure.Message}");
                cleanup();
                message = "Could not start Play";
                return false;
            }
        }

        internal void stop() {
            if (!running || stopping) {
                return;
            }
            stop_deadline = DateTime.UtcNow.AddSeconds(5);
            try {
                File.WriteAllText(Path.Combine(workspace!, "stop.request"), string.Empty);
            } catch (Exception error) {
                host_logger.write($"Editor play: {error.Message}");
            }
        }

        internal bool poll(out string message) {
            message = string.Empty;
            if (process == null) {
                return false;
            }
            if (!process.HasExited) {
                if (!stopping || DateTime.UtcNow < stop_deadline) {
                    return false;
                }
                terminate();
                return false;
            }
            bool failed = !stopping && process.ExitCode != 0;
            message = failed ? $"Play exited with code {process.ExitCode}" : "Play stopped";
            if (failed) {
                var log_path = Path.Combine(workspace!, "deren-debug.log");
                try {
                    if (File.Exists(log_path)) {
                        foreach (string line in File.ReadLines(log_path)) {
                            if (line.StartsWith("scripts: Editor play:", StringComparison.Ordinal)) {
                                message = line["scripts: ".Length..];
                            }
                        }
                    }
                } catch (IOException) { }
                host_logger.write(message);
            }
            cleanup();
            return true;
        }

        internal static readonly JsonSerializerOptions camera_json = new() { IncludeFields = true };

        public void Dispose() {
            if (process is { } active && !active.HasExited) {
                stop();
                if (!active.WaitForExit(1500)) {
                    terminate();
                    active.WaitForExit(1500);
                }
            }
            cleanup();
        }

        private void terminate() {
            try {
                process?.Kill();
            } catch (InvalidOperationException) { }
        }

        private void cleanup() {
            process?.Dispose();
            process = null;
            stop_deadline = default;
            if (workspace != null) {
                try {
                    var parent = Path.Combine(project_directory, "play-sessions");
                    if (Path.GetDirectoryName(Path.GetFullPath(workspace)) == parent) {
                        Directory.Delete(workspace, recursive: true);
                    }
                } catch (Exception error) {
                    host_logger.write($"Editor play cleanup: {error.Message}");
                }
                workspace = null;
            }
        }
    }
}
