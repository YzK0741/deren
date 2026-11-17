namespace deren.scripting {
    /// <summary>
    /// The project-owned entry point. The managed host loads the assembly before the renderer
    /// exists, calls preinit to let the game parse startup arguments, and calls initialize once the
    /// scene API is available. The game creates its own scene tree, chooses its own assets,
    /// activates the tree, and owns its scripts.
    /// </summary>
    internal unsafe interface game_entry {
        script_status preinit(startup_options* options, int argc, byte** argv);
        script_status initialize(native_api* api, managed_api* scripts);
    }
}
