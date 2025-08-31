// Framework.Renderer/Renderer.cs
#nullable enable
#pragma warning disable CS0649 // struct fields assigned from native memory

using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Framework.Renderer;

public unsafe sealed class Renderer : IDisposable
{
    // -------- ABI header mirrors native fw_header --------
    [StructLayout(LayoutKind.Sequential)]
    public struct Header
    {
        public uint abi_version;
        public delegate* unmanaged[Cdecl]<sbyte*> get_last_error; // returns char*
        public nint log_cb;   // fw_log_fn*, optional
        public nint log_user; // void* user
    }

    // -------- API table mirrors native fw_renderer_api --------
    [StructLayout(LayoutKind.Sequential)]
    public struct RendererApi
    {
        public Header hdr;

        // void set_logger(void* cb /*fw_log_fn*/, void* user)
        public delegate* unmanaged[Cdecl]<nint, nint, void> set_logger;

        // int lines_upload(fw_handle dev, const float* xy, uint count, float r,g,b,a)
        public delegate* unmanaged[Cdecl]<ulong, float*, uint, float, float, float, float, int> lines_upload;

        // int create_device(const fw_renderer_desc*, fw_handle*)
        public delegate* unmanaged[Cdecl]<RendererDesc*, ulong*, int> create_device;

        // void destroy_device(fw_handle)
        public delegate* unmanaged[Cdecl]<ulong, void> destroy_device;

        // void begin_frame(fw_handle)
        public delegate* unmanaged[Cdecl]<ulong, void> begin_frame;

        // void end_frame(fw_handle)
        public delegate* unmanaged[Cdecl]<ulong, void> end_frame;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct RendererDesc
    {
        public nint hwnd; // HWND on Windows
    }

    private nint _lib;
    private RendererApi _api;
    private ulong _dev;
    private GCHandle _gch; // to recover 'this' in the log thunk

    private Renderer() { }

    // ------- native logger thunk (no delegate marshaling) -------
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void LogThunk(int level, sbyte* msg, void* user)
    {
        try
        {
            var r = (Renderer?)GCHandle.FromIntPtr((nint)user).Target;
            string text = msg != null ? new string(msg) : "<null>";
            Debug.WriteLine($"[native {level}] {text}");
            // You can route this into a managed ILogger if you like:
            // r?._managedLogger?.Log(...);
        }
        catch { /* never throw across native boundary */ }
    }

    public static unsafe Renderer Create(nint hwnd)
    {
        var r = new Renderer();

        // Load native DLL and fetch fwGetRendererAPI (returns void* to table)
        r._lib = NativeLibrary.Load("RendererNative.dll");
        nint sym = NativeLibrary.GetExport(r._lib, "fwGetRendererAPI");
        var getApi = (delegate* unmanaged[Cdecl]<uint, void*>)sym;

        var apiPtr = (RendererApi*)getApi(3); // ABI v3
        if (apiPtr == null)
            throw new InvalidOperationException("RendererNative: ABI v3 not available.");
        r._api = *apiPtr;

        // Install logger (pass raw function pointer + GCHandle as user)
        r._gch = GCHandle.Alloc(r);
        delegate* unmanaged[Cdecl]<int, sbyte*, void*, void> cb = &LogThunk;
        r._api.set_logger((nint)cb, GCHandle.ToIntPtr(r._gch));

        // Create device
        var desc = new RendererDesc { hwnd = hwnd };
        ulong dev = 0;
        int rc = r._api.create_device(&desc, &dev);
        if (rc != 0 || dev == 0)
        {
            string err = r._api.hdr.get_last_error != null
                ? new string(r._api.hdr.get_last_error())
                : "unknown";
            throw new InvalidOperationException($"create_device failed: {err}");
        }

        r._dev = dev;
        return r;
    }

    public void BeginFrame() => _api.begin_frame(_dev);
    public void EndFrame() => _api.end_frame(_dev);

    // xy = [x0,y0, x1,y1, ...] in NDC, count = xy.Length/2
    public unsafe void UploadLines(ReadOnlySpan<float> xy, float r, float g, float b, float a)
    {
        if (xy.Length == 0) return;
        fixed (float* p = xy)
        {
            _api.lines_upload(_dev, p, (uint)(xy.Length / 2), r, g, b, a);
        }
    }

    public void Dispose()
    {
        if (_dev != 0) { _api.destroy_device(_dev); _dev = 0; }
        if (_gch.IsAllocated) _gch.Free();
        if (_lib != 0) { NativeLibrary.Free(_lib); _lib = 0; }
    }
}
