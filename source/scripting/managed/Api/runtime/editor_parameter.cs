using System;

namespace deren {
    /// <summary>Exposes a public writable behaviour field in the editor's Details panel.</summary>
    [AttributeUsage(AttributeTargets.Field, Inherited = true)]
    public sealed class editor_parameter : Attribute {
        public editor_parameter(string? label = null) {
            this.label = label;
        }

        public string? label { get; }
    }

    /// <summary>An asset-library ID assigned to a behaviour by the scene.</summary>
    public readonly record struct asset_reference(string id);
}
