using System.Runtime.InteropServices;

namespace deren.scripting {
    // The native node identity is a scene_tree::scene_node ECS entity (index + generation); the scene
    // field scopes it to one live script_scene, so the existing ABI layout already carries a node id.
    [StructLayout(LayoutKind.Sequential)]
    internal struct node_handle {
        public ulong scene;
        public uint index;
        public uint generation;
    }
}
