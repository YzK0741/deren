namespace deren.scripting {
    /// <summary>
    /// The host's answer for one call. Public because the wrapper overloads hand it back: a game that
    /// wants to tell "no such asset" from "this host has no such entry" branches on it.
    /// </summary>
    public enum script_status {
        success = 0,
        invalid_argument = 1,
        invalid_node = 2,
        scene_exists = 3,
        scene_not_found = 4,
        script_failure = 5,
        spawn_unavailable = 6,
        module_unavailable = 7,
        module_invalid = 8,
        asset_not_found = 9,
        asset_invalid = 10
    }
}
