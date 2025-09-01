using System;
using System.Windows.Forms;

namespace Framework.Renderer;

/// <summary>HWND host; drives the native renderer on the Idle loop.</summary>
public sealed class RendererHost : Control
{
    Renderer? _r;
    float[]? _circle;
    int _count;

    public RendererHost()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint |
                 ControlStyles.UserPaint |
                 ControlStyles.Opaque, true);
        DoubleBuffered = false;
        TabStop = false;
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);

        _r = Renderer.Create(Handle, (uint)ClientSize.Width, (uint)ClientSize.Height, vsync: false, validation: true);

        // simple circle in NDC for smoke test
        _circle = MakeCircleVertices(256, 0.8f, out _count);

        // identity matrices for now (camera-relative VS comes next)
        _r.SetMatrices(
            new double[] { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 },
            new double[] { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 },
            new double[] { 0, 0, 0 });

        Application.Idle += OnIdle;
    }

    protected override void OnHandleDestroyed(EventArgs e)
    {
        Application.Idle -= OnIdle;
        _r?.Dispose(); _r = null;
        base.OnHandleDestroyed(e);
    }

    void OnIdle(object? sender, EventArgs e)
    {
        if (_r == null || _circle == null) return;

        _r.Begin(0.02f, 0.03f, 0.05f, 1f);
        _r.DrawLines(_circle, _count, 0.85f, 0.85f, 0.85f, 1f, 1f);
        _r.End();
        _r.Present();
    }

    protected override void OnSizeChanged(EventArgs e)
    {
        base.OnSizeChanged(e);
        if (_r != null && ClientSize.Width > 0 && ClientSize.Height > 0)
            _r.Resize((uint)ClientSize.Width, (uint)ClientSize.Height);
    }

    protected override void OnPaintBackground(PaintEventArgs pevent) { /* no GDI */ }
    protected override void OnPaint(PaintEventArgs e) { /* native presents */ }

    protected override void WndProc(ref Message m)
    {
        const int WM_ERASEBKGND = 0x0014;
        if (m.Msg == WM_ERASEBKGND) { m.Result = IntPtr.Zero; return; }
        base.WndProc(ref m);
    }

    static float[] MakeCircleVertices(int segments, float r, out int count)
    {
        count = segments + 1;
        var arr = new float[count * 3];
        for (int i = 0; i <= segments; i++)
        {
            double a = 2.0 * Math.PI * i / segments;
            arr[i * 3 + 0] = (float)(r * Math.Cos(a));
            arr[i * 3 + 1] = (float)(r * Math.Sin(a));
            arr[i * 3 + 2] = 0f;
        }
        return arr;
    }
}
