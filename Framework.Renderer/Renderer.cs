// Framework.Renderer/Renderer.cs
#nullable enable
#pragma warning disable IDE0002, IDE0051, IDE0060 // style warnings

using System;
using System.Runtime.InteropServices;
using System.Diagnostics;

namespace Framework.Renderer;

public sealed class Renderer : IDisposable
{
    // ---------------------------
    // Native ABI (fmGetRendererAPI, ABI v3)
    // ---------------------------

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate IntPtr FnGetApi(uint abi);                // void* fmGetRendererAPI(uint32_t)

    [StructLayout(LayoutKind.Sequential)]
    private struct FwHeaderRaw
    {
        public uint abi_version;
        public IntPtr GetLastError; // char* (*)()
        public IntPtr LogCb;        // fw_log_fn (optional)
        public IntPtr LogUser;      // void* user (optional)
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct FwRendererApiRaw
    {
        public FwHeaderRaw Hdr;

        public IntPtr SetLogger;     // void (*)(FnLog cb, void* user)
        public IntPtr LinesUpload;   // int  (*)(ulong dev, float* xy, uint count, float r,g,b,a)
        public IntPtr CreateDevice;  // int  (*)(ref FwRendererDesc, out ulong)
        public IntPtr DestroyDevice; // void (*)(ulong dev)
        public IntPtr BeginFrame;    // void (*)(ulong dev)
        public IntPtr EndFrame;      // void (*)(ulong dev)
    }

    /// <summary>
    /// Cache projection/view/world matrices (row-major 4x4). Currently a no-op
    /// on the native side; kept for future UBO upload. Accepts double for
    /// simulation precision; converted to float for the renderer.
    /// </summary>
    public void SetMatrices(double[] proj, double[] view, double[] world)
    {
        SetMatrices((ReadOnlySpan<double>)proj, (ReadOnlySpan<double>)view, (ReadOnlySpan<double>)world);
    }

    public void SetMatrices(ReadOnlySpan<double> proj, ReadOnlySpan<double> view, ReadOnlySpan<double> world)
    {
        if (proj.Length < 16 || view.Length < 16 || world.Length < 16)
            throw new ArgumentException("SetMatrices expects 16 elements for each matrix.");

        CopyDoublesToFloats(proj, _proj);
        CopyDoublesToFloats(view, _view);
        CopyDoublesToFloats(world, _world);
        _matricesDirty = true; // future: trigger UBO upload in Begin()
    }

    private static void CopyDoublesToFloats(ReadOnlySpan<double> src, float[] dst)
    {
        for (int i = 0; i < 16; ++i) dst[i] = (float)src[i];
    }

    // Mirror of native fm_renderer_desc (current ABI: HWND only)
    [StructLayout(LayoutKind.Sequential)]
    public struct FwRendererDesc
    {
        public IntPtr hwnd; // HWND on Windows
    }

    // Function pointer shapes
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate IntPtr FnGetLastError();

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void FnLog(int level, [MarshalAs(UnmanagedType.LPStr)] string msg, IntPtr user);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void FnSetLogger(FnLog cb, IntPtr user);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int FnCreateDevice(ref FwRendererDesc desc, out ulong dev);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void FnDestroyDevice(ulong dev);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void FnBeginFrame(ulong dev);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void FnEndFrame(ulong dev);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private unsafe delegate int FnLinesUpload(ulong dev, float* xy, uint count, float r, float g, float b, float a);

    // ---------------------------
    // Managed state
    // ---------------------------

    private IntPtr _lib;
    private FwRendererApiRaw _raw;

    private FnGetLastError? _getLastErr;
    private FnSetLogger? _setLogger;
    private FnCreateDevice? _create;
    private FnDestroyDevice? _destroy;
    private FnBeginFrame? _begin;
    private FnEndFrame? _end;
    private unsafe FnLinesUpload? _linesUpload;

    // --- camera/world matrices (cached; not used by native yet) ---
    private readonly float[] _proj = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    private readonly float[] _view = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    private readonly float[] _world = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    private bool _matricesDirty;

    // Keep the logger delegate rooted
    private FnLog? _logDelegate;

    public ulong Device { get; private set; }

    // ---------------------------
    // Construction / teardown
    // ---------------------------

    private Renderer() { }

    /// <summary>
    /// Load the native DLL, bind the API table, and create the renderer device for this HWND.
    /// Extra parameters are accepted for future ABI expansion but currently unused by native code.
    /// </summary>
    public static Renderer Create(IntPtr hwnd, uint width = 0, uint height = 0, bool vsync = false, bool validation = true)
    {
        var r = new Renderer();

        // Load native library
        r._lib = NativeLibrary.Load("RendererNative");

        // Look up the unified export. Fall back to fwGetRendererAPI if present.
        IntPtr sym = IntPtr.Zero;
        try { sym = NativeLibrary.GetExport(r._lib, "fmGetRendererAPI"); } catch { /* ignore */ }
        if (sym == IntPtr.Zero)
        {
            try { sym = NativeLibrary.GetExport(r._lib, "fwGetRendererAPI"); } catch { /* ignore */ }
        }
        if (sym == IntPtr.Zero)
        {
            r.Dispose();
            throw new EntryPointNotFoundException("Neither 'fmGetRendererAPI' nor 'fwGetRendererAPI' found in RendererNative.dll");
        }

        var getApi = Marshal.GetDelegateForFunctionPointer<FnGetApi>(sym);
        IntPtr apiPtr = getApi(3); // ABI v3
        if (apiPtr == IntPtr.Zero)
        {
            r.Dispose();
            throw new InvalidOperationException("RendererNative: ABI v3 not available.");
        }

        // Marshal the raw function table and bind delegates
        r._raw = Marshal.PtrToStructure<FwRendererApiRaw>(apiPtr);
        r._getLastErr = Marshal.GetDelegateForFunctionPointer<FnGetLastError>(r._raw.Hdr.GetLastError);
        r._setLogger = Marshal.GetDelegateForFunctionPointer<FnSetLogger>(r._raw.SetLogger);
        r._create = Marshal.GetDelegateForFunctionPointer<FnCreateDevice>(r._raw.CreateDevice);
        r._destroy = Marshal.GetDelegateForFunctionPointer<FnDestroyDevice>(r._raw.DestroyDevice);
        r._begin = Marshal.GetDelegateForFunctionPointer<FnBeginFrame>(r._raw.BeginFrame);
        r._end = Marshal.GetDelegateForFunctionPointer<FnEndFrame>(r._raw.EndFrame);
        r._linesUpload = Marshal.GetDelegateForFunctionPointer<FnLinesUpload>(r._raw.LinesUpload);

        // Install a simple logger (kept rooted via _logDelegate)
        r._logDelegate = (lvl, msg, _) => Debug.WriteLine($"[native {lvl}] {msg}");
        r._setLogger!(r._logDelegate, IntPtr.Zero);

        // Create native device (current desc carries only hwnd)
        var desc = new FwRendererDesc { hwnd = hwnd };

        ulong dev;
        int rc = r._create!(ref desc, out dev);
        if (rc != 0 || dev == 0)
        {
            string err = r.Err();
            r.Dispose();
            throw new InvalidOperationException($"create_device failed (rc={rc}): {err}");
        }

        r.Device = dev;
        return r;
    }

    // ---------------------------
    // Public API
    // ---------------------------

    public string Err()
    {
        try
        {
            IntPtr p = _getLastErr?.Invoke() ?? IntPtr.Zero;
            return p != IntPtr.Zero ? (Marshal.PtrToStringAnsi(p) ?? string.Empty) : string.Empty;
        }
        catch { return string.Empty; }
    }

    /// <summary>Resize hook — currently handled natively from the HWND; kept for future expansion.</summary>
    public void Resize(uint w, uint h) { /* no-op for now */ }

    /// <summary>Begin a frame (color parameters are currently unused by native; kept for call-site stability).</summary>
    public void Begin(float r, float g, float b, float a) => _begin?.Invoke(Device);

    /// <summary>Finish the frame; present happens inside native EndFrame.</summary>
    public void End() => _end?.Invoke(Device);

    /// <summary>No-op: present is driven by native swapchain submit in End().</summary>
    public void Present() { /* no-op */ }

    /// <summary>Upload a line list in NDC: xy = [x0,y0, x1,y1, ...], count = pairs.</summary>
    public unsafe void DrawLines(ReadOnlySpan<float> xy, int count, float r, float g, float b, float a, float widthPx = 1f)
    {
        if (Device == 0 || count <= 0 || xy.Length < count * 2) return;
        fixed (float* p = xy)
        {
            _ = _linesUpload?.Invoke(Device, p, (uint)count, r, g, b, a);
        }
    }

    public void Dispose()
    {
        try
        {
            if (Device != 0) { _destroy?.Invoke(Device); Device = 0; }
        }
        catch { /* swallow on dispose */ }
        _logDelegate = null;
        if (_lib != IntPtr.Zero)
        {
            try { NativeLibrary.Free(_lib); } catch { }
            _lib = IntPtr.Zero;
        }
    }
}
