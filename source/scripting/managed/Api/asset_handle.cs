using System.Runtime.InteropServices;

namespace deren {
    /// <summary>
    /// An opaque reference to one loaded asset. The native side owns the source data; release the
    /// handle when the script no longer needs to bind new instances of it.
    /// </summary>
    [StructLayout(LayoutKind.Sequential)]
    public readonly struct asset_handle {
        internal readonly ulong value;

        internal asset_handle(ulong value) {
            this.value = value;
        }

        public bool is_valid => value != 0;
    }
}
