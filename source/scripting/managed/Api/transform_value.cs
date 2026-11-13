using System.Runtime.InteropServices;

namespace deren {
    [StructLayout(LayoutKind.Sequential)]
    public struct vector_3 {
        public float x;
        public float y;
        public float z;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct transform_value {
        public vector_3 position;
        public vector_3 rotation_degrees;
        public vector_3 scale;
    }
}
