#nullable enable
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Framework.Renderer;

/// <summary>Thin loader/wrapper over the native GetRendererAPI (ABI v3).</summary>
public sealed class Renderer : IDisposable
{
    // ===== raw ABI headers (mirror your current c_api.h / renderer_api.h) =====
    [StructLayout(LayoutKind.Sequential)]
    private struct FwHeaderRaw
    {
        public uint AbiVersion;
        public IntPtr GetLastError;  // fn(): const char*
        public IntPtr Log;           // fm_log_fn (optional)
        public IntPtr LogUser;       // void*
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct FwRendererApiRaw
    {
        public FwHeaderRaw Hdr;

        public IntPtr SetLogger;     // void (fm_log_fn, void*)
        public IntPtr LinesUpload;   // int  (handle, float* xy, uint count, r,g,b,a)
        public IntPtr CreateDevice;  // int  (desc*, out handle)
        public IntPtr DestroyDevice; // void (handle)
        public IntPtr BeginFrame;    // void (handle)
        public IntPtr EndFrame;      // void (handle)
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct FwRendererDesc
    {
        public IntPtr Hwnd; // HWND (width/height not needed by current native)
    }

    // ===== delegates (cdecl) =====
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr FnGetLastError();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void FnLog(int level, [MarshalAs(UnmanagedType.LPStr)] string msg, IntPtr user);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private unsafe delegate void FnSetLogger(FnLog cb, IntPtr user);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private unsafe delegate int FnCreateDevice(ref FwRendererDesc desc, out ulong dev);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void FnDestroyDevice(ulong dev);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void FnBeginFrame(ulong dev);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void FnEndFrame(ulong dev);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private unsafe delegate int FnLinesUpload(ulong dev, float* xy, uint count, float r, float g, float b, float a);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr FnGetApi(uint abi);

    private const uint Abi = 3;

    // bound function pointers
    private IntPtr _lib;
    private FwRendererApiRaw _raw;

    private FnGetLastError _getLastErr = default!;
    private FnSetLogger _setLogger = default!;
    private FnCreateDevice _create = default!;
    private FnDestroyDevice _destroy = default!;
    private FnBeginFrame _begin = default!;
    private FnEndFrame _end = default!;
    private FnLinesUpload _linesUpload = default!;
    private FnLog? _logDelegate; // keep alive

    public ulong Device { get; private set; }

    private Renderer() { }

    private string Err() => Marshal.PtrToStringAnsi(_getLastErr()) ?? "(unknown)";

    /// <summary>Load the native DLL, bind the table, create a device for this HWND.</summary>
    public static Renderer Create(IntPtr hwnd, uint width, uint height, bool vsync = false, bool validation = true)
    {
        var r = new Renderer();

        r._lib = NativeLibrary.Load("RendererNative");

        // Try both names so we work with either export.
        if (!NativeLibrary.TryGetExport(r._lib, "fmGetRendererAPI", out var getApiPtr) &&
            !NativeLibrary.TryGetExport(r._lib, "fwGetRendererAPI", out getApiPtr))
            throw new EntryPointNotFoundException("Unable to find 'fmGetRendererAPI' or 'fwGetRendererAPI' in RendererNative.dll");

        var getApi = Marshal.GetDelegateForFunctionPointer<FnGetApi>(getApiPtr);
        var apiPtr = getApi(Abi);
        if (apiPtr == IntPtr.Zero)
            throw new InvalidOperationException("RendererNative: ABI v3 not available.");

        r._raw = Marshal.PtrToStructure<FwRendererApiRaw>(apiPtr);

        // Bind delegates
        r._getLastErr = Marshal.GetDelegateForFunctionPointer<FnGetLastError>(r._raw.Hdr.GetLastError);
        r._setLogger = Marshal.GetDelegateForFunctionPointer<FnSetLogger>(r._raw.SetLogger);
        r._create = Marshal.GetDelegateForFunctionPointer<FnCreateDevice>(r._raw.CreateDevice);
        r._destroy = Marshal.GetDelegateForFunctionPointer<FnDestroyDevice>(r._raw.DestroyDevice);
        r._begin = Marshal.GetDelegateForFunctionPointer<FnBeginFrame>(r._raw.BeginFrame);
        r._end = Marshal.GetDelegateForFunctionPointer<FnEndFrame>(r._raw.EndFrame);
        r._linesUpload = Marshal.GetDelegateForFunctionPointer<FnLinesUpload>(r._raw.LinesUpload);

        // Logger (keep delegate alive)
        r._logDelegate = (lvl, msg, _) => Debug.WriteLine($"[native {lvl}] {msg}");
        r._setLogger(r._logDelegate, IntPtr.Zero);

        var desc = new FwRendererDesc { Hwnd = hwnd };
        if (r._create(ref desc, out var dev) != 0 || dev == 0)
            throw new InvalidOperationException("create_device failed: " + r.Err());

        r.Device = dev;
        return r;
    }

    // ---- public API used by your forms/host ----

    public void Resize(uint w, uint h)
    {
        // No-op: native begin_frame detects size changes and recreates the swapchain.
    }

    public void Begin(float r, float g, float b, float a)
    {
        // Native begin_frame clears internally; we keep this signature for convenience.
        _begin(Device);
    }

    public void End()
    {
        // Native end_frame also submits & presents.
        _end(Device);
    }

    public void Present() { /* no-op (done in End) */ }

    // Convenience wrapper that matches your current host code (XYZ or XY input).
    public unsafe void DrawLines(float[] xyz, int count, float r, float g, float b, float a, float widthPx = 1f)
    {
        if (xyz == null || count <= 0) return;

        // Detect stride: host may pass XY or XYZ. Native expects XY.
        int stride = (xyz.Length >= count * 3) ? 3 : 2;

        if (stride == 2)
        {
            fixed (float* p = xyz)
            {
                var rc = _linesUpload(Device, p, (uint)count, r, g, b, a);
                if (rc != 0) Debug.WriteLine($"lines_upload rc={rc}: {Err()}");
            }
        }
        else
        {
            var tmp = new float[count * 2];
            for (int i = 0; i < count; i++)
            {
                tmp[i * 2 + 0] = xyz[i * 3 + 0];
                tmp[i * 2 + 1] = xyz[i * 3 + 1];
            }
            fixed (float* p = tmp)
            {
                var rc = _linesUpload(Device, p, (uint)count, r, g, b, a);
                if (rc != 0) Debug.WriteLine($"lines_upload rc={rc}: {Err()}");
            }
        }
    }

    // Keep the lower-level upload as well (useful for XY-only spans).
    public unsafe void UploadLines(ReadOnlySpan<float> xy, float r, float g, float b, float a)
    {
        if (xy.Length == 0) return;
        fixed (float* p = xy)
        {
            var rc = _linesUpload(Device, p, (uint)(xy.Length / 2), r, g, b, a);
            if (rc != 0) Debug.WriteLine($"lines_upload rc={rc}: {Err()}");
        }
    }

    // Compatibility stub: native doesn’t take matrices yet; keep API to avoid caller churn.
    public void SetMatrices(double[] view3x4, double[] proj4x4, double[] origin3) { /* no-op for now */ }

    public void Dispose()
    {
        if (Device != 0) { _destroy(Device); Device = 0; }
        if (_lib != IntPtr.Zero) { NativeLibrary.Free(_lib); _lib = IntPtr.Zero; }
        _logDelegate = null;
        GC.SuppressFinalize(this);
    }
}
