using System.Runtime.InteropServices;

namespace deren.scripting {
    /// <summary>
    /// The project-owned entry point. The managed host only loads the project assembly, resolves
    /// this interface, and then forwards frames and shutdown to it. The game creates its own
    /// scene tree, chooses its own assets, activates the tree, and owns its scripts.
    /// </summary>
    internal unsafe interface game_entry {
        script_status initialize(native_api* api, managed_api* scripts);
    }
}
