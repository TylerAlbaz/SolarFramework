// Framework.Renderer/Renderer.cs
#nullable enable
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Framework.Renderer;

public sealed class Renderer : IDisposable
{
    // ---- ABI structs (must match native exactly) ---------------------------
    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct FwHeaderRaw
    {
        public uint abi_version;
        public IntPtr get_last_error;  // char* () cdecl
        public IntPtr log_cb;          // optional
        public IntPtr log_user;        // optional
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct FwRendererApiRaw
    {
        public FwHeaderRaw Hdr;

        public IntPtr set_logger;      // void (*)(void* cb /*fw_log_fn*/, void* user)
        public IntPtr create_device;   // int  (*)(const fw_renderer_desc*, fw_handle*)
        public IntPtr destroy_device;  // void (*)(fw_handle)
        public IntPtr begin_frame;     // void (*)(fw_handle)
        public IntPtr end_frame;       // void (*)(fw_handle)
        public IntPtr lines_upload;    // int  (*)(fw_handle, float* xy, uint count, float r,g,b,a)
    }

    [StructLayout(LayoutKind.Sequential, Pack = 8)]
    private struct FwRendererDesc
    {
        public IntPtr hwnd; // HWND
    }

    // ---- delegates (cdecl) -------------------------------------------------
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr FnGetApi(uint abi);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate IntPtr FnGetLastError();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void FnSetLogger(IntPtr cb, IntPtr user);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private unsafe delegate int FnLinesUpload(ulong dev, float* xy, uint count, float r, float g, float b, float a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int FnCreateDevice(ref FwRendererDesc desc, out ulong dev);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void FnDestroyDevice(ulong dev);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void FnBeginFrame(ulong dev);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void FnEndFrame(ulong dev);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void FnLog(int level, IntPtr msg, IntPtr user);

    // ---- instance state ----------------------------------------------------
    private IntPtr _lib;
    private FwRendererApiRaw _raw;

    private FnGetLastError _getLastErr = default!;
    private FnSetLogger _setLogger = default!;
    private FnCreateDevice _create = default!;
    private FnDestroyDevice _destroy = default!;
    private FnBeginFrame _begin = default!;
    private FnEndFrame _end = default!;
    private FnLinesUpload _linesUpload = default!;
    private FnLog _logDelegate = default!; // keep rooted

    private ulong Device { get; set; }

    private Renderer() { }

    // ---- factory -----------------------------------------------------------
    public static Renderer Create(IntPtr hwnd)
    {
        var r = new Renderer();

        // 1) Load native DLL
        if (!NativeLibrary.TryLoad("RendererNative", out r._lib))
            throw new DllNotFoundException("Could not load RendererNative.dll.");

        // 2) Resolve fmGetRendererAPI / fwGetRendererAPI symbol
        if (!TryGetExport(r._lib, "fmGetRendererAPI", out var sym) &&
            !TryGetExport(r._lib, "fwGetRendererAPI", out sym))
            throw new EntryPointNotFoundException("Could not find fmGetRendererAPI or fwGetRendererAPI.");

        var getApi = Marshal.GetDelegateForFunctionPointer<FnGetApi>(sym);
        IntPtr apiPtr = getApi(3); // ABI v3
        if (apiPtr == IntPtr.Zero)
            throw new InvalidOperationException("ABI v3 not available.");

        // 3) Marshal table & bind delegates (ORDER MUST MATCH THE NATIVE HEADER)
        r._raw = Marshal.PtrToStructure<FwRendererApiRaw>(apiPtr)!;

        Debug.WriteLine($"[interop] API table @ 0x{apiPtr.ToInt64():X}");
        Debug.WriteLine($"[interop]   get_last_error @ 0x{r._raw.Hdr.get_last_error.ToInt64():X}");
        Debug.WriteLine($"[interop]   set_logger    @ 0x{r._raw.set_logger.ToInt64():X}");
        Debug.WriteLine($"[interop]   create_device @ 0x{r._raw.create_device.ToInt64():X}");
        Debug.WriteLine($"[interop]   destroy_device@ 0x{r._raw.destroy_device.ToInt64():X}");
        Debug.WriteLine($"[interop]   begin_frame   @ 0x{r._raw.begin_frame.ToInt64():X}");
        Debug.WriteLine($"[interop]   end_frame     @ 0x{r._raw.end_frame.ToInt64():X}");
        Debug.WriteLine($"[interop]   lines_upload  @ 0x{r._raw.lines_upload.ToInt64():X}");

        r._getLastErr = GetDel<FnGetLastError>(r._raw.Hdr.get_last_error, nameof(FnGetLastError));
        r._setLogger = GetDel<FnSetLogger>(r._raw.set_logger, nameof(FnSetLogger));
        r._create = GetDel<FnCreateDevice>(r._raw.create_device, nameof(FnCreateDevice));
        r._destroy = GetDel<FnDestroyDevice>(r._raw.destroy_device, nameof(FnDestroyDevice));
        r._begin = GetDel<FnBeginFrame>(r._raw.begin_frame, nameof(FnBeginFrame));
        r._end = GetDel<FnEndFrame>(r._raw.end_frame, nameof(FnEndFrame));
        r._linesUpload = GetDel<FnLinesUpload>(r._raw.lines_upload, nameof(FnLinesUpload));

        static T GetDel<T>(IntPtr p, string name) where T : Delegate
        {
            if (p == IntPtr.Zero)
                throw new EntryPointNotFoundException($"Function pointer for {name} is null (ABI mismatch).");
            return Marshal.GetDelegateForFunctionPointer<T>(p);
        }

        // 4) Install logger (keep delegate alive)
        r._logDelegate = (lvl, msg, _) =>
        {
            string text = msg != IntPtr.Zero ? Marshal.PtrToStringAnsi(msg)! : "<null>";
            Debug.WriteLine($"[native {lvl}] {text}");
        };
        IntPtr logFnPtr = Marshal.GetFunctionPointerForDelegate(r._logDelegate);
        r._setLogger(logFnPtr, IntPtr.Zero);
        Debug.WriteLine("[interop] Logger installed (ABI v3).");

        // 5) Create device
        var desc = new FwRendererDesc { hwnd = hwnd };
        int rc = r._create(ref desc, out var dev);
        if (rc != 0 || dev == 0)
        {
            string err = r.Err();
            r.Dispose();
            throw new InvalidOperationException($"create_device failed (rc={rc}): {err}");
        }

        r.Device = dev;
        return r;
    }

    // ---- public facade -----------------------------------------------------
    public void Begin(float r, float g, float b, float a) => _begin(Device);
    public void End() => _end(Device);
    public void Present() => End();          // native presents in End()
    public void Resize(uint w, uint h) { }   // native recreates swapchain by HWND size

    public unsafe void DrawLines(float[] xyz, int count, float r, float g, float b, float a, float widthPx = 1f)
    {
        if (xyz is null || count <= 0) return;
        if (xyz.Length < count * 2)
            throw new ArgumentException("xyz must contain 2*count floats (x,y per vertex).", nameof(xyz));

        fixed (float* p = xyz)
            _linesUpload(Device, p, (uint)count, r, g, b, a);
    }

    // ---- camera matrices (optional; stored for future UBO path) ---------------
    private static readonly double[] s_identity = new double[]
    {
    1,0,0,0,
    0,1,0,0,
    0,0,1,0,
    0,0,0,1
    };

    private double[] _mView = (double[])s_identity.Clone();
    private double[] _mProj = (double[])s_identity.Clone();
    private double[] _mWorld = (double[])s_identity.Clone();

    /// <summary>
    /// Provide 4x4 row-major matrices (length 16 each). Stored only for now;
    /// native path ignores them until we wire the UBO pipeline.
    /// </summary>
    public void SetMatrices(double[] view, double[] proj, double[] world)
    {
        if (view is null || view.Length != 16) throw new ArgumentException("view must be 16 elements", nameof(view));
        if (proj is null || proj.Length != 16) throw new ArgumentException("proj must be 16 elements", nameof(proj));
        if (world is null || world.Length != 16) throw new ArgumentException("world must be 16 elements", nameof(world));

        // keep copies to avoid external mutation
        _mView = (double[])view.Clone();
        _mProj = (double[])proj.Clone();
        _mWorld = (double[])world.Clone();
    }

    // (kept: your stored matrices & SetMatrices(...) if you’re using them later)

    // ---- helpers -----------------------------------------------------------
    public string Err()
    {
        try
        {
            var p = _getLastErr != null ? _getLastErr() : IntPtr.Zero;
            return p != IntPtr.Zero ? Marshal.PtrToStringAnsi(p)! : "<no error>";
        }
        catch { return "<error reading native error string>"; }
    }

    public void Dispose()
    {
        try { if (Device != 0) _destroy(Device); } catch { } finally { Device = 0; }
        if (_lib != IntPtr.Zero) { try { NativeLibrary.Free(_lib); } catch { } _lib = IntPtr.Zero; }
    }

    private static bool TryGetExport(IntPtr lib, string name, out IntPtr symbol)
    {
        try { symbol = NativeLibrary.GetExport(lib, name); return symbol != IntPtr.Zero; }
        catch { symbol = IntPtr.Zero; return false; }
    }
}
