using System;
using System.Runtime.InteropServices;
using System.Text;

namespace deren.scripting {
    /// <summary>
    /// One UTF-8 copy of a managed string, valid until it is disposed. The ABI borrows every string
    /// as (data, length) for the length of a single call, so this owns an unmanaged copy instead of
    /// pinning a managed array: a call site reads <c>using var text = native_string.create(value)</c>
    /// and passes <see cref="view"/>, instead of repeating Encoding.GetBytes plus fixed.
    /// </summary>
    internal unsafe struct native_string : IDisposable {
        private byte* data;
        private uint length;

        /// <summary>Copies @p value to UTF-8. Null and the empty string both yield an empty view.</summary>
        internal static native_string create(string? value) {
            var text = value ?? string.Empty;
            var byte_count = Encoding.UTF8.GetByteCount(text);
            if (byte_count == 0) {
                return default;
            }
            var result = new native_string {
                data = (byte*)NativeMemory.Alloc((nuint)byte_count),
                length = (uint)byte_count,
            };
            fixed (char* chars = text) {
                Encoding.UTF8.GetBytes(chars, text.Length, result.data, byte_count);
            }
            return result;
        }

        /// <summary>The view the ABI takes. Empty input is a null pointer with length zero.</summary>
        internal readonly utf8_view view => new() {
            data = length == 0 ? null : data,
            length = length,
        };

        public void Dispose() {
            if (data == null) {
                return;
            }

            NativeMemory.Free(data);
            data = null;
            length = 0;
        }
    }
}
