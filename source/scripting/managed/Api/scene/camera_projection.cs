using deren.scripting;

namespace deren {
    /// <summary>
    /// World from/to screen conversion against the camera the frame actually renders with.
    ///
    /// The answers are in RENDER pixels: the extent <see cref="try_get_viewport_size"/> reports and the
    /// passes shade at. That is the window's size only while the render scale is 1 - a game that wants
    /// window pixels scales by <see cref="frame_diagnostics.frame_counters"/>'s output/render ratio. The
    /// projection is the jitter-free one, so a world point lands on the same pixel in every TAA frame.
    /// </summary>
    public static unsafe class camera_projection {
        /// <summary>
        /// The resolution the frame shades at. Returns false when nothing has been rendered yet (no
        /// frame has picked an extent).
        /// </summary>
        public static bool try_get_viewport_size(out uint width, out uint height) {
            return try_get_viewport_size(out width, out height, out _);
        }

        public static bool try_get_viewport_size(out uint width, out uint height, out script_status status) {
            width = 0;
            height = 0;
            uint w;
            uint h;
            status = scene_api.query_viewport_size(&w, &h);
            if (status != script_status.success) {
                return false;
            }
            width = w;
            height = h;
            return true;
        }

        /// <summary>
        /// Projects a world-space point to render pixels. <paramref name="depth"/> is the view-space
        /// distance along the camera's forward axis: POSITIVE in front of the camera, negative behind
        /// it. The point is projected either way, so a caller that only wants on-screen points tests
        /// <c>depth &gt; 0</c> before trusting the pixel.
        /// </summary>
        public static bool try_project_point(float x, float y, float z, out float pixel_x, out float pixel_y,
                                             out float depth) {
            return try_project_point(x, y, z, out pixel_x, out pixel_y, out depth, out _);
        }

        public static bool try_project_point(float x, float y, float z, out float pixel_x, out float pixel_y,
                                             out float depth, out script_status status) {
            pixel_x = 0.0f;
            pixel_y = 0.0f;
            depth = 0.0f;
            float* world = stackalloc float[3];
            float* result = stackalloc float[3];
            world[0] = x;
            world[1] = y;
            world[2] = z;
            status = scene_api.project_world_point(world, result);
            if (status != script_status.success) {
                return false;
            }
            pixel_x = result[0];
            pixel_y = result[1];
            depth = result[2];
            return true;
        }

        /// <summary>
        /// The ray through a render pixel: its origin is the camera's position and its direction is a
        /// unit vector in world space. Pair it with a CPU-side test to pick the geometry under the
        /// cursor.
        /// </summary>
        public static bool try_screen_ray(float pixel_x, float pixel_y, out vector_3 origin, out vector_3 direction) {
            return try_screen_ray(pixel_x, pixel_y, out origin, out direction, out _);
        }

        public static bool try_screen_ray(float pixel_x, float pixel_y, out vector_3 origin, out vector_3 direction,
                                          out script_status status) {
            origin = default;
            direction = default;
            var eye = stackalloc float[3];
            var unit = stackalloc float[3];
            status = scene_api.query_screen_ray(pixel_x, pixel_y, eye, unit);
            if (status != script_status.success) {
                return false;
            }
            origin = new vector_3 { x = eye[0], y = eye[1], z = eye[2] };
            direction = new vector_3 { x = unit[0], y = unit[1], z = unit[2] };
            return true;
        }

        /// <summary>
        /// Tells the renderer the shape of the rectangle the game shows the scene in. The render still
        /// covers the whole window, but the projection is built for this aspect, so a game that puts
        /// the scene image into a document viewport stretches it without distorting the geometry. Pass
        /// 0 to go back to following the window's extent. The value applies from the next frame and
        /// <see cref="try_project_point"/> / <see cref="try_screen_ray"/> answer in the same projection.
        /// </summary>
        public static bool try_set_aspect(float aspect) {
            return try_set_aspect(aspect, out _);
        }

        public static bool try_set_aspect(float aspect, out script_status status) {
            status = scene_api.assign_scene_aspect(aspect);
            return status == script_status.success;
        }
    }
}
