using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using deren.scripting;

namespace deren.editor {
    internal sealed class play_session : IDisposable {
        private readonly string project_directory;
        private readonly string cache_directory;
        private Process? process;
        // THE CHILD MUST NOT OUTLIVE THE EDITOR: a stop file is a cooperative handshake, and it never
        // runs when the editor crashes, is killed from the task manager, or throws out of its update.
        // The job object closes with this process, and JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE then takes
        // the renderer's whole tree down with it instead of leaving a window and a GPU context behind.
        private IntPtr job;
        private string? workspace;
        private DateTime stop_deadline;
        private bool forced;
        private play_log_reader log = new();

        internal play_session(string project_directory, string cache_directory) {
            this.project_directory = Path.GetFullPath(project_directory);
            this.cache_directory = Path.GetFullPath(cache_directory);
        }

        /// Deletes the workspaces a previous editor process left behind - a crash cannot run the
        /// cooperative stop, so the next launch sweeps them instead of accumulating session copies.
        internal static void sweep_stale_workspaces(string project_directory) {
            foreach (var name in new[] { "play-sessions", "thumbnail-jobs" }) {
                var parent = Path.Combine(Path.GetFullPath(project_directory), name);
                if (!Directory.Exists(parent)) {
                    continue;
                }
                try {
                    foreach (var stale in Directory.EnumerateDirectories(parent)) {
                        try {
                            Directory.Delete(stale, recursive: true);
                        } catch (Exception error) {
                            host_logger.write($"Editor sweep: {error.Message}");
                        }
                    }
                } catch (Exception error) {
                    host_logger.write($"Editor sweep: {error.Message}");
                }
            }
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
                log = new play_log_reader();
                workspace = Path.Combine(cache_directory, "play-sessions", Guid.NewGuid().ToString("N"));
                Directory.CreateDirectory(workspace);
                var snapshot = Path.Combine(workspace, "play.scene.toml");
                File.WriteAllText(snapshot, copy);
                File.WriteAllText(Path.Combine(workspace, "camera.json"), JsonSerializer.Serialize(camera, camera_json));
                var executable = Environment.ProcessPath ??
                    throw new InvalidOperationException("The rendering executable path is unavailable.");
                var start_info = new ProcessStartInfo(executable) {
                    WorkingDirectory = workspace,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                };
                start_info.ArgumentList.Add("run");
                start_info.ArgumentList.Add(project_directory);
                start_info.ArgumentList.Add("--editor-worker=play");
                start_info.ArgumentList.Add("--editor-play");
                start_info.ArgumentList.Add(snapshot);
                job = create_job();
                process = Process.Start(start_info) ?? throw new InvalidOperationException("The play process did not start.");
                if (job != IntPtr.Zero) {
                    try {
                        if (!AssignProcessToJobObject(job, process.Handle)) {
                            host_logger.write($"Editor play: the job object refused the renderer ({Marshal.GetLastWin32Error()})");
                        }
                    } catch (Exception assignment) {
                        // The renderer may already be gone; the cooperative stop below still applies.
                        host_logger.write($"Editor play: {assignment.Message}");
                    }
                }
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

        internal bool poll(Action<Guid, string, bool> receive, out string message, out bool failed) {
            message = string.Empty;
            failed = false;
            if (process == null) {
                return false;
            }
            bool finished = process.HasExited;
            log.read(Path.Combine(workspace!, "deren-debug.log"), finished, receive);
            if (!finished) {
                if (!stopping || DateTime.UtcNow < stop_deadline) {
                    return false;
                }
                terminate();
                return false;
            }
            failed = !stopping && process.ExitCode != 0;
            message = failed ? $"Play exited with code {process.ExitCode}"
                : forced ? "Play stopped (the renderer was terminated after the stop deadline)" : "Play stopped";
            receive(Guid.Empty, message, failed);
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

        private const int job_object_extended_limit_information = 9;
        private const uint job_object_limit_kill_on_job_close = 0x2000;

        [StructLayout(LayoutKind.Sequential)]
        private struct job_basic_limit {
            public long per_process_user_time_limit;
            public long per_job_user_time_limit;
            public uint limit_flags;
            public UIntPtr minimum_working_set_size;
            public UIntPtr maximum_working_set_size;
            public uint active_process_limit;
            public UIntPtr affinity;
            public uint priority_class;
            public uint scheduling_class;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct job_io_counters {
            public ulong read_operation_count;
            public ulong write_operation_count;
            public ulong other_operation_count;
            public ulong read_transfer_count;
            public ulong write_transfer_count;
            public ulong other_transfer_count;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct job_extended_limit_information {
            public job_basic_limit basic_limit_information;
            public job_io_counters io_info;
            public UIntPtr process_memory_limit;
            public UIntPtr job_memory_limit;
            public UIntPtr peak_process_memory_used;
            public UIntPtr peak_job_memory_used;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateJobObject(IntPtr attributes, string? name);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetInformationJobObject(IntPtr job, int information_class, IntPtr information, uint length);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr handle);

        private static IntPtr create_job() {
            var handle = CreateJobObject(IntPtr.Zero, null);
            if (handle == IntPtr.Zero) {
                host_logger.write($"Editor play: no job object ({Marshal.GetLastWin32Error()})");
                return IntPtr.Zero;
            }
            var limits = new job_extended_limit_information {
                basic_limit_information = new job_basic_limit { limit_flags = job_object_limit_kill_on_job_close },
            };
            var size = Marshal.SizeOf<job_extended_limit_information>();
            var buffer = Marshal.AllocHGlobal(size);
            try {
                Marshal.StructureToPtr(limits, buffer, false);
                if (!SetInformationJobObject(handle, job_object_extended_limit_information, buffer, (uint)size)) {
                    host_logger.write($"Editor play: the job object refused its limits ({Marshal.GetLastWin32Error()})");
                    CloseHandle(handle);
                    return IntPtr.Zero;
                }
            } finally {
                Marshal.FreeHGlobal(buffer);
            }
            return handle;
        }

        private void terminate() {
            forced = true;
            try {
                process?.Kill();
            } catch (Exception error) when (error is InvalidOperationException or Win32Exception or NotSupportedException) {
                // The renderer exited between the check and the kill; there is nothing left to stop.
            }
        }

        private void cleanup() {
            if (job != IntPtr.Zero) {
                // Closing the last handle runs the kill-on-close limit: a renderer that ignored the
                // cooperative stop dies here rather than outliving the editor.
                CloseHandle(job);
                job = IntPtr.Zero;
            }
            process?.Dispose();
            process = null;
            stop_deadline = default;
            forced = false;
            if (workspace != null) {
                try {
                    var parent = Path.Combine(cache_directory, "play-sessions");
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
