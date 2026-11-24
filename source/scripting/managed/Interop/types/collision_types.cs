using System.Runtime.InteropServices;

namespace deren.scripting {
    /// <summary>
    /// ABI 45: one box of the collision world, in WORLD space.
    /// </summary>
    /// <remarks>
    /// The kernel works in world space on purpose: a rotated or non-uniformly scaled document node
    /// would turn a capsule into an ellipsoid in the node's own frame. The three axes are the box's
    /// local directions as unit vectors and <c>half</c> is the extent along each of them. The
    /// <c>id</c> is the caller's own number and comes back unchanged from every query;
    /// <c>enabled</c> 0 keeps the box in the set but out of every query.
    /// </remarks>
    [StructLayout(LayoutKind.Sequential)]
    internal struct collision_box_desc {
        public float center_x;
        public float center_y;
        public float center_z;
        public float half_x;
        public float half_y;
        public float half_z;
        // Row-major: the first three are the x axis, then y, then z.
        public float axis_0;
        public float axis_1;
        public float axis_2;
        public float axis_3;
        public float axis_4;
        public float axis_5;
        public float axis_6;
        public float axis_7;
        public float axis_8;
        public uint id;
        public byte enabled;
        public byte padding_0;
        public byte padding_1;
        public byte padding_2;
    }

    /// <summary>The swept shape of ABI 45: the segment lower..upper dilated by <c>radius</c>.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct collision_capsule_desc {
        public float lower_x;
        public float lower_y;
        public float lower_z;
        public float upper_x;
        public float upper_y;
        public float upper_z;
        public float radius;
        public float padding;
    }

    /// <summary>
    /// One sweep's answer. <c>time</c> is the fraction of the requested displacement travelled before
    /// the first contact (1 when nothing was hit, 0 when the start already overlaps), <c>point</c> is
    /// the closest point on the box at that time, and <c>depth</c> is how far the start was inside.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct collision_hit_desc {
        public byte hit;
        public byte padding_0;
        public byte padding_1;
        public byte padding_2;
        public float time;
        public float point_x;
        public float point_y;
        public float point_z;
        public float normal_x;
        public float normal_y;
        public float normal_z;
        public float depth;
        public uint id;
    }

    /// <summary>One slide's answer: what was actually travelled, the last contact and its surface.</summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct collision_move_desc {
        public float displacement_x;
        public float displacement_y;
        public float displacement_z;
        public float normal_x;
        public float normal_y;
        public float normal_z;
        public uint id;
        public int contacts;
        public byte blocked;
        public byte grounded;
        public byte padding_0;
        public byte padding_1;
    }

    /// <summary>
    /// One floor probe: <c>distance</c> is measured from the capsule's lowest point along the probe
    /// (it already excludes the radius) and <c>walkable</c> applies the requested normal threshold.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    internal struct collision_floor_desc {
        public byte hit;
        public byte walkable;
        public byte padding_0;
        public byte padding_1;
        public float distance;
        public float point_x;
        public float point_y;
        public float point_z;
        public float normal_x;
        public float normal_y;
        public float normal_z;
        public uint id;
    }
}
