using System;
using deren.scripting;

namespace deren {
    /// <summary>
    /// OFFSCREEN REFLECTION-PROBE CAPTURE (ABI 30). The host installs a cube-capture camera and runs
    /// the readback pass; everything above that lives here: the six faces, the frame cadence (a face
    /// needs a settled frame before its pixels are requested, and the request is collected one frame
    /// later), and the output file.
    ///
    /// The capture renders at the RENDER EXTENT, not at an arbitrary probe size: the face is copied
    /// out of the frame the window is already rendering. A cube therefore needs a square window whose
    /// side is <see cref="min_size"/>..<see cref="max_size"/> and a power of two, and a mismatched
    /// extent is reported instead of being silently stretched.
    ///
    /// Every call has an <c>out script_status</c> overload: validation done here reports
    /// <see cref="script_status.invalid_argument"/>, a refused host call reports what the host said.
    /// </summary>
    public static unsafe class reflect_probe {
        public const uint min_size = 16;
        public const uint max_size = 512;
        /// The face is installed on frame 0 and its radiance requested on this frame, so the capture
        /// sees a settled frame rather than the frame the camera changed on (HEAD's cadence).
        public const int request_frame = 8;
        /// The request is collected on the frame after the one it was armed before.
        public const int read_frame = 9;

        public static bool try_install(vector_3 position, uint face, float near_plane = 0.05f,
                                       float far_plane = 100.0f) {
            return try_install(position, face, near_plane, far_plane, out _);
        }

        public static bool try_install(vector_3 position, uint face, float near_plane, float far_plane,
                                       out script_status status) {
            if (face > 5) {
                status = script_status.invalid_argument;
                return reject($"face must be 0..5, got {face}");
            }
            if (!(near_plane > 0.0f) || !(far_plane > near_plane)) {
                status = script_status.invalid_argument;
                return reject($"near/far must bracket the scene, got {near_plane}/{far_plane}");
            }
            var eye = stackalloc float[3];
            eye[0] = position.x;
            eye[1] = position.y;
            eye[2] = position.z;
            status = scene_api.assign_capture_camera(1, eye, face, near_plane, far_plane);
            return accept(status, $"install face {face} at ({position.x}, {position.y}, {position.z})");
        }

        /// Restores the orbit camera and turns the capture pass off.
        public static bool clear() {
            return clear(out _);
        }

        public static bool clear(out script_status status) {
            status = scene_api.assign_capture_camera(0, null, 0, 0.0f, 0.0f);
            return accept(status, "clear");
        }

        public static bool try_request() {
            return try_request(out _);
        }

        public static bool try_request(out script_status status) {
            status = scene_api.assign_radiance_request();
            return accept(status, "request");
        }

        /// Copies one face out. `pixels` holds the face as RGBA floats, so its length is size*size*4.
        public static bool try_read(uint size, float[] pixels) {
            return try_read(size, pixels, out _);
        }

        public static bool try_read(uint size, float[] pixels, out script_status status) {
            if (!valid_size(size)) {
                status = script_status.invalid_argument;
                return reject($"size must be a power of two in {min_size}..{max_size}, got {size}");
            }
            if (pixels == null || pixels.Length != (int)(size * size * 4)) {
                status = script_status.invalid_argument;
                return reject($"read buffer must hold {size * size * 4} floats");
            }
            uint width = 0;
            uint height = 0;
            fixed (float* target = pixels) {
                status = scene_api.read_radiance_image(&width, &height, target, (uint)pixels.Length);
                if (status != script_status.success) {
                    return reject($"read failed ({status})");
                }
            }
            if (width != size || height != size) {
                status = script_status.invalid_argument;
                return reject($"the render extent is {width}x{height}, not the {size}x{size} the probe needs " +
                              "(run the window at the probe size)");
            }
            return true;
        }

        /// Drops the temporal history so the next face is not blended with the previous one.
        public static bool reset_history() {
            return reset_history(out _);
        }

        public static bool reset_history(out script_status status) {
            status = scene_api.assign_temporal_reset();
            return accept(status, "reset temporal history");
        }

        /// Writes six collected faces as an IBL1 cube file.
        public static bool try_write(string path, uint size, float[] cube) {
            return try_write(path, size, cube, out _);
        }

        public static bool try_write(string path, uint size, float[] cube, out script_status status) {
            if (string.IsNullOrEmpty(path)) {
                status = script_status.invalid_argument;
                return reject("an output path is required");
            }
            if (!valid_size(size)) {
                status = script_status.invalid_argument;
                return reject($"size must be a power of two in {min_size}..{max_size}, got {size}");
            }
            if (cube == null || cube.Length != (int)(6 * size * size * 4)) {
                status = script_status.invalid_argument;
                return reject($"cube buffer must hold {6 * size * size * 4} floats");
            }
            using var text = native_string.create(path);
            fixed (float* pixels = cube) {
                status = scene_api.publish_radiance_cube(text.view, size, pixels, (uint)cube.Length);
                return accept(status, $"write '{path}'");
            }
        }

        internal static bool valid_size(uint size) {
            return size >= min_size && size <= max_size && (size & (size - 1)) == 0;
        }

        private static bool accept(script_status status, string what) {
            if (status == script_status.success) {
                return true;
            }
            return reject($"{what} failed ({status})");
        }

        private static bool reject(string message) {
            log.write($"Game: reflect probe: {message}");
            return false;
        }
    }

    /// <summary>
    /// The six-face loop as a per-frame state machine. call <see cref="step"/> once per frame; every
    /// sixth completed face writes the cube and sets <see cref="complete"/>.
    /// </summary>
    public sealed class reflect_probe_session {
        private readonly vector_3 position;
        private readonly string path;
        private readonly float near_plane;
        private readonly float far_plane;
        private uint size;
        private float[]? cube;
        private uint face;
        private int frame;
        private bool running;
        private bool finished;

        public reflect_probe_session(vector_3 position, uint size, string path, float near_plane = 0.05f,
                                     float far_plane = 100.0f) {
            this.position = position;
            this.size = size;
            this.path = path;
            this.near_plane = near_plane;
            this.far_plane = far_plane;
            failure = reflect_probe.valid_size(size)
                ? null
                : $"size must be a power of two in {reflect_probe.min_size}..{reflect_probe.max_size}, got {size}";
        }

        /// Why the session stopped without a cube; null while it is healthy.
        public string? failure { get; private set; }
        public bool complete => finished;
        public bool active => running;
        /// How many faces are already collected (0..6).
        public uint faces_collected => face;

        /// Arms the first face. False means nothing was captured and the caller should not step again.
        public bool begin() {
            if (failure != null) {
                return false;
            }
            cube = new float[6 * size * size * 4];
            face = 0;
            frame = 0;
            running = reflect_probe.try_install(position, 0, near_plane, far_plane);
            if (!running) {
                failure = "the host refused the capture camera";
            }
            return running;
        }

        /// Advances one frame. Returns false once the session has stopped (complete or failed).
        public bool step() {
            if (!running) {
                return false;
            }
            if (frame == reflect_probe.request_frame) {
                if (!reflect_probe.try_request()) {
                    return fail("the host refused the radiance request");
                }
                ++frame;
                return true;
            }
            if (frame == reflect_probe.read_frame) {
                var face_values = size * size * 4;
                var pixels = new float[face_values];
                if (!reflect_probe.try_read(size, pixels)) {
                    return fail("the radiance readback failed");
                }
                Array.Copy(pixels, 0, cube!, face * face_values, face_values);
                ++face;
                frame = 0;
                if (face == 6) {
                    reflect_probe.clear();
                    if (!reflect_probe.try_write(path, size, cube!)) {
                        return fail("the cube could not be written");
                    }
                    running = false;
                    finished = true;
                    log.write(
                        $"Game: reflect probe: wrote '{path}' ({size}px, 6 faces)");
                    return false;
                }
                reflect_probe.reset_history();
                if (!reflect_probe.try_install(position, face, near_plane, far_plane)) {
                    return fail($"the host refused the capture camera for face {face}");
                }
                return true;
            }
            ++frame;
            return true;
        }

        private bool fail(string message) {
            failure = message;
            running = false;
            reflect_probe.clear();
            return false;
        }
    }
}
