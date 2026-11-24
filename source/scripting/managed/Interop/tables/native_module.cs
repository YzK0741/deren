namespace deren.scripting {
    internal enum native_module : uint {
        scene = 1,
        ui = 2,
        audio = 3,
        rml_ui = 4,
        node_bounds = 5,
        rml_ui_extended = 6,
        asset_authoring = 7,
        asset_authoring_extended = 8,
        /// <summary>ABI 45: the capsule collision world (see <c>collision_api</c>).</summary>
        collision = 9
    }
}
