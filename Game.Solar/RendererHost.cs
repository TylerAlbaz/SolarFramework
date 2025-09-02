using System;
using System.Windows.Forms;
using Framework.Renderer;
using NativeRenderer = Framework.Renderer.Renderer;

namespace Game.Solar
{
    /// <summary>WinForms control that drives the native renderer on the idle loop.</summary>
    public sealed class RendererHost : Control
    {
        private NativeRenderer? _r;
        private float[]? _circle;
        private int _count;

        // 4x4 identity (row-major) for future UBO path
        private static readonly double[] s_identity = new double[]
        {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1
        };

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

            // NEW: single-argument factory
            _r = NativeRenderer.Create(this.Handle);

            // simple circle in NDC for smoke test
            _circle = MakeCircleVertices(256, 0.8f, out _count);

            // stash identity matrices (native will use once UBO is wired)
            _r.SetMatrices(s_identity, s_identity, s_identity);

            Application.Idle += OnIdle;
        }

        protected override void OnHandleDestroyed(EventArgs e)
        {
            Application.Idle -= OnIdle;
            _r?.Dispose();
            _r = null;
            base.OnHandleDestroyed(e);
        }

        private void OnIdle(object? sender, EventArgs e)
        {
            if (_r == null || _circle == null) return;

            // Clear only (no geometry yet)
            _r.Begin(0.02f, 0.03f, 0.05f, 1f);

            // draw the circle in a light gray; widthPx is accepted (currently ignored by native)
            _r.DrawLines(_circle, _count, 0.85f, 0.85f, 0.85f, 1f, 1f);

            _r.End();
            _r.Present();

            Invalidate(); // keep WM_PAINT flowing
        }

        protected override void OnSizeChanged(EventArgs e)
        {
            base.OnSizeChanged(e);
            if (_r != null && ClientSize.Width > 0 && ClientSize.Height > 0)
                _r.Resize((uint)ClientSize.Width, (uint)ClientSize.Height);
        }

        private static float[] MakeCircleVertices(int segs, float radius, out int count)
        {
            // xy pairs
            var verts = new float[(segs + 1) * 2];
            for (int i = 0; i <= segs; i++)
            {
                double t = (i / (double)segs) * Math.PI * 2.0;
                verts[i * 2 + 0] = (float)(Math.Cos(t) * radius);
                verts[i * 2 + 1] = (float)(Math.Sin(t) * radius);
            }
            count = segs + 1;
            return verts;
        }
    }
}
