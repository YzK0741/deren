using System;
using System.Collections.Generic;
using System.Threading;

namespace deren.scripting {
    /// <summary>
    /// Runs managed callables at the two frame boundaries the native host reports.
    ///
    /// The shape is three threads and eight queues. A boundary signal wakes the dispatcher, which
    /// owns the frame order: it hands the blocking batch to the blocking executor and waits for it,
    /// waits for the previous non-blocking batch to finish, hands this frame's non-blocking batch to
    /// the non-blocking executor without waiting, and only then answers the host through
    /// <c>native_api.notify_rendering</c>. The dispatcher never runs a callable itself, so user code
    /// cannot hold up the queue that prepares the next boundary.
    ///
    /// The queues are one per (boundary x repeat x blocking) combination: a once task is removed
    /// when its snapshot is taken, a repeat task stays registered until <c>cancel</c>. Counted tasks
    /// are repeats whose closure decides which boundaries reach the wrapped callable, so they need
    /// no queue of their own. Each executor runs one batch at a time in registration order.
    ///
    /// Two consequences are worth stating up front. Non-blocking does not mean free: the dispatcher
    /// waits for the previous non-blocking batch before it starts this frame's, so a batch that
    /// outlives a frame delays the host's answer for the next boundary, and the frame rate follows.
    /// And a batch is a snapshot: a callable cancelled while its batch is draining still finishes
    /// that run, and a once task registered during a drain waits for the next boundary of its kind.
    /// </summary>
    public static class frame_event_processor {
        private sealed class frame_task {
            internal ulong handle;
            internal Action action = null!;
        }

        /// <summary>The four queues behind one boundary; blocking is a property of each task.</summary>
        private sealed class frame_queues {
            internal readonly List<frame_task> once_blocking = [];
            internal readonly List<frame_task> once_nonblocking = [];
            internal readonly List<frame_task> repeat_blocking = [];
            internal readonly List<frame_task> repeat_nonblocking = [];
        }

        private static readonly Lock tasks_mutex = new();
        private static readonly frame_queues before_queues = new();
        private static readonly frame_queues after_queues = new();
        private static ulong next_handle = 1;

        // The dispatcher: what the host's signal actually wakes. It never runs a callable.
        private static readonly object signal_monitor = new();
        private static readonly Queue<bool> signals = new();
        private static Thread? dispatcher;

        // The blocking executor: the dispatcher waits for it, which is what makes the host wait.
        private static readonly object blocking_monitor = new();
        private static List<Action>? blocking_batch;
        private static bool blocking_idle = true;
        private static Thread? blocking_thread;

        // The non-blocking executor: started and not waited for. The dispatcher only insists that
        // the previous batch finished, so two non-blocking batches never overlap with each other.
        private static readonly object concurrent_monitor = new();
        private static List<Action>? concurrent_batch;
        private static bool concurrent_idle = true;
        private static Thread? concurrent_thread;

        // Set once by shutdown(): every thread finishes what it is doing and returns.
        private static volatile bool stopping;

        private const int shutdown_join_ms = 2000;

        /// <summary>
        /// Runs <paramref name="action"/> at the next before-frame boundary, once.
        /// Returns false when the task was refused (a null action).
        /// </summary>
        public static bool do_before_frame_once(Action action, bool is_blocking = true) {
            return add(before: true, action, is_blocking, repeat: false, out _);
        }

        /// <summary>Runs <paramref name="action"/> at the next after-frame boundary, once.</summary>
        public static bool do_after_frame_once(Action action, bool is_blocking = true) {
            return add(before: false, action, is_blocking, repeat: false, out _);
        }

        /// <summary>
        /// Runs <paramref name="action"/> at every before-frame boundary until cancelled.
        /// Returns a handle for <see cref="cancel"/>, or 0 when the task was refused.
        /// </summary>
        public static ulong do_before_frame_repeat(Action action, bool is_blocking = true) {
            return add(before: true, action, is_blocking, repeat: true, out var handle) ? handle : 0;
        }

        /// <summary>Runs <paramref name="action"/> at every after-frame boundary until cancelled.</summary>
        public static ulong do_after_frame_repeat(Action action, bool is_blocking = true) {
            return add(before: false, action, is_blocking, repeat: true, out var handle) ? handle : 0;
        }

        /// <summary>
        /// Runs <paramref name="action"/> on every <paramref name="frame_count"/>-th before-frame
        /// boundary, counting from registration, until cancelled.
        /// </summary>
        public static ulong do_before_counted_frame_repeat(ulong frame_count, Action action, bool is_blocking = true) {
            return counted(before: true, frame_count, action, is_blocking);
        }

        /// <summary>
        /// Runs <paramref name="action"/> on every <paramref name="frame_count"/>-th after-frame
        /// boundary, counting from registration, until cancelled.
        /// </summary>
        public static ulong do_after_counted_frame_repeat(ulong frame_count, Action action, bool is_blocking = true) {
            return counted(before: false, frame_count, action, is_blocking);
        }

        /// <summary>
        /// Stops a repeating task. Returns false when the handle is unknown, already cancelled, or
        /// belongs to a once task (which has no handle). A task already running finishes first: the
        /// batch that is draining is a snapshot and is no longer reachable from the queues.
        /// </summary>
        public static bool cancel(ulong handle) {
            if (handle == 0) {
                return false;
            }
            lock (tasks_mutex) {
                return remove_from(before_queues, handle) || remove_from(after_queues, handle);
            }
        }

        /// <summary>Drops every registered task without running it.</summary>
        internal static void clear_all() {
            lock (tasks_mutex) {
                clear(before_queues);
                clear(after_queues);
            }
        }

        /// <summary>
        /// Ends all three threads at shutdown. The host calls this through the stop entry point, so
        /// a batch queued before it either runs or is dropped before the native side tears down. A
        /// batch already running gets a bounded moment to finish: it cannot be interrupted.
        /// </summary>
        internal static void shutdown() {
            stopping = true;
            lock (signal_monitor) {
                signals.Clear();
                Monitor.PulseAll(signal_monitor);
            }
            lock (blocking_monitor) {
                blocking_batch = null;
                Monitor.PulseAll(blocking_monitor);
            }
            lock (concurrent_monitor) {
                concurrent_batch = null;
                Monitor.PulseAll(concurrent_monitor);
            }
            dispatcher?.Join(shutdown_join_ms);
            blocking_thread?.Join(shutdown_join_ms);
            concurrent_thread?.Join(shutdown_join_ms);
        }

        /// <summary>Posted by the native boundary entry point: it only hands the order over.</summary>
        internal static void dispatch_before() {
            post(before: true);
        }

        internal static void dispatch_after() {
            post(before: false);
        }

        /// <summary>The host's wait escape hatch: answer even when the batch could not be prepared.</summary>
        internal static unsafe void notify_host() {
            try {
                if (managed_bootstrap.core.notify_rendering == null) {
                    return;
                }
                var status = managed_bootstrap.core.notify_rendering();
                if (status != script_status.success) {
                    nothrow_log.write($"Frame events: the host rejected the completion notice ({status}).");
                }
            } catch (Exception error) {
                // This runs from a finally block, so it must not throw: the notice is already lost.
                nothrow_log.write($"Frame events: the completion notice failed: {error.GetBaseException().Message}");
            }
        }

        private static void post(bool before) {
            if (stopping) {
                // Shutdown already ended the threads: answer so the host is never left waiting.
                notify_host();
                return;
            }
            lock (signal_monitor) {
                signals.Enqueue(before);
                if (dispatcher == null) {
                    dispatcher = new Thread(dispatcher_loop) { IsBackground = true, Name = "deren frame events (dispatch)" };
                    dispatcher.Start();
                }
                Monitor.Pulse(signal_monitor);
            }
        }

        private static void dispatcher_loop() {
            while (true) {
                bool before;
                lock (signal_monitor) {
                    while (signals.Count == 0) {
                        if (stopping) {
                            return;
                        }
                        Monitor.Wait(signal_monitor);
                    }
                    before = signals.Dequeue();
                }
                try {
                    process(before);
                } catch (Exception error) {
                    nothrow_log.write($"Frame events: the boundary failed: {error.GetBaseException().Message}");
                } finally {
                    // One signal, one answer, even when the batch could not be prepared: a skipped
                    // notice would leave the render loop waiting for its timeout instead.
                    notify_host();
                }
            }
        }

        /// <summary>
        /// One boundary, in the order the frame needs: blocking first (the host waits for it), then
        /// the previous non-blocking batch, then this frame's. The answer to the host is the
        /// dispatcher's, in a finally block, so no failure inside the batch can skip it.
        /// </summary>
        private static void process(bool before) {
            take(before, out var blocking, out var concurrent);
            if (blocking.Count > 0) {
                run_blocking(blocking);
            }
            if (concurrent.Count > 0) {
                start_concurrent(concurrent);
            }
        }

        private static bool add(bool before, Action? action, bool is_blocking, bool repeat, out ulong handle) {
            handle = 0;
            if (stopping) {
                // The threads are gone: a task accepted here would never run.
                return false;
            }
            if (action == null) {
                nothrow_log.write("Frame events: a null callable was refused.");
                return false;
            }
            var entry = new frame_task { action = action };
            lock (tasks_mutex) {
                // A once task keeps handle 0, which is how the snapshot recognises it.
                if (repeat) {
                    entry.handle = next_handle++;
                    handle = entry.handle;
                }
                list_for(before, repeat, is_blocking).Add(entry);
            }
            return true;
        }

        private static ulong counted(bool before, ulong frame_count, Action? action, bool is_blocking) {
            if (action == null || frame_count == 0) {
                nothrow_log.write(frame_count == 0
                    ? "Frame events: a counted task needs a non-zero frame count."
                    : "Frame events: a counted task was refused.");
                return 0;
            }
            // Counted is a thin wrapper: the repeat stays the unit that can be cancelled, and the
            // closure decides which boundaries reach the wrapped callable. The counter only moves
            // on the executing thread, so it needs no extra lock.
            ulong seen = 0;
            Action wrapped = () => {
                seen++;
                if (seen % frame_count == 0) {
                    action();
                }
            };
            return add(before, wrapped, is_blocking, repeat: true, out var handle) ? handle : 0;
        }

        private static List<frame_task> list_for(bool before, bool repeat, bool blocking) {
            var queues = before ? before_queues : after_queues;
            if (repeat) {
                return blocking ? queues.repeat_blocking : queues.repeat_nonblocking;
            }
            return blocking ? queues.once_blocking : queues.once_nonblocking;
        }

        private static bool remove_from(frame_queues queues, ulong handle) {
            return remove_with_handle(queues.once_blocking, handle) ||
                   remove_with_handle(queues.once_nonblocking, handle) ||
                   remove_with_handle(queues.repeat_blocking, handle) ||
                   remove_with_handle(queues.repeat_nonblocking, handle);
        }

        private static bool remove_with_handle(List<frame_task> tasks, ulong handle) {
            for (var index = 0; index < tasks.Count; ++index) {
                if (tasks[index].handle == handle) {
                    tasks.RemoveAt(index);
                    return true;
                }
            }
            return false;
        }

        private static void clear(frame_queues queues) {
            queues.once_blocking.Clear();
            queues.once_nonblocking.Clear();
            queues.repeat_blocking.Clear();
            queues.repeat_nonblocking.Clear();
        }

        /// <summary>
        /// Takes the boundary's snapshot. Once queues are emptied, repeat queues are copied and stay
        /// registered; the blocking flag decides which executor the action ends up on.
        /// </summary>
        private static void take(bool before, out List<Action> blocking, out List<Action> concurrent) {
            blocking = [];
            concurrent = [];
            lock (tasks_mutex) {
                var queues = before ? before_queues : after_queues;
                collect(queues.once_blocking, blocking, remove: true);
                collect(queues.once_nonblocking, concurrent, remove: true);
                collect(queues.repeat_blocking, blocking, remove: false);
                collect(queues.repeat_nonblocking, concurrent, remove: false);
            }
        }

        private static void collect(List<frame_task> tasks, List<Action> actions, bool remove) {
            if (tasks.Count == 0) {
                return;
            }
            // Order is registration order either way. When the whole list is being taken, clear it
            // instead of removing from the front, which would shift the tail once per entry.
            foreach (var task in tasks) {
                actions.Add(task.action);
            }
            if (remove) {
                tasks.Clear();
            }
        }

        private static void run_blocking(List<Action> actions) {
            lock (blocking_monitor) {
                while (!blocking_idle && !stopping) {
                    Monitor.Wait(blocking_monitor);
                }
                if (stopping) {
                    return;
                }
                blocking_batch = actions;
                blocking_idle = false;
                if (blocking_thread == null) {
                    blocking_thread = new Thread(blocking_loop) { IsBackground = true, Name = "deren frame events (blocking)" };
                    blocking_thread.Start();
                }
                Monitor.Pulse(blocking_monitor);
                // The host waits for this batch, so the dispatcher waits for the executor.
                while (!blocking_idle && !stopping) {
                    Monitor.Wait(blocking_monitor);
                }
            }
        }

        private static void start_concurrent(List<Action> actions) {
            lock (concurrent_monitor) {
                while (!concurrent_idle && !stopping) {
                    Monitor.Wait(concurrent_monitor);
                }
                if (stopping) {
                    return;
                }
                concurrent_batch = actions;
                concurrent_idle = false;
                if (concurrent_thread == null) {
                    concurrent_thread = new Thread(concurrent_loop) { IsBackground = true, Name = "deren frame events (non-blocking)" };
                    concurrent_thread.Start();
                }
                Monitor.Pulse(concurrent_monitor);
            }
        }

        private static void blocking_loop() {
            while (true) {
                List<Action>? batch;
                lock (blocking_monitor) {
                    while (blocking_batch == null) {
                        if (stopping) {
                            return;
                        }
                        Monitor.Wait(blocking_monitor);
                    }
                    batch = blocking_batch;
                    blocking_batch = null;
                }
                run(batch, "callable failed");
                lock (blocking_monitor) {
                    blocking_idle = true;
                    Monitor.PulseAll(blocking_monitor);
                }
            }
        }

        private static void concurrent_loop() {
            while (true) {
                List<Action>? batch;
                lock (concurrent_monitor) {
                    while (concurrent_batch == null) {
                        if (stopping) {
                            return;
                        }
                        Monitor.Wait(concurrent_monitor);
                    }
                    batch = concurrent_batch;
                    concurrent_batch = null;
                }
                // No notify: the host was already released and must not be answered twice.
                run(batch, "non-blocking callable failed");
                lock (concurrent_monitor) {
                    concurrent_idle = true;
                    Monitor.PulseAll(concurrent_monitor);
                }
            }
        }

        private static void run(List<Action> batch, string failure) {
            foreach (var action in batch) {
                try {
                    action();
                } catch (Exception error) {
                    nothrow_log.write($"Frame events: {failure}: {error.GetBaseException().Message}");
                }
            }
        }
    }
}