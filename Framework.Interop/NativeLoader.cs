using System;
using System.IO;
using System.Runtime.InteropServices;

namespace Framework.Interop
{
    public static class NativeLoader
    {
        public static nint Load(string dllName)
        {
            var full = Path.Combine(AppContext.BaseDirectory, dllName);
            if (!File.Exists(full))
                throw new FileNotFoundException($"Native DLL not found at: {full}");
            return NativeLibrary.Load(full);   // absolute path
        }

        public static T Get<T>(nint lib, string name) where T : Delegate =>
          Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(lib, name));

        public static void Free(nint lib) => NativeLibrary.Free(lib);
    }
}