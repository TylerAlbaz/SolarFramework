using System.Windows.Forms;

namespace Framework.Renderer
{

    /// <summary>HWND host for Vulkan; never lets GDI paint.</summary>
    public sealed class RendererHost : Control
    {
        public RendererHost()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint |
            ControlStyles.UserPaint |
            ControlStyles.Opaque, true);
            DoubleBuffered = false;
            TabStop = false;
        }

        protected override void OnPaintBackground(PaintEventArgs pevent) { /* no-op */ }
        protected override void OnPaint(PaintEventArgs e) { /* no-op */ }
        protected override void WndProc(ref Message m)
        {
            const int WM_ERASEBKGND = 0x0014; if (m.Msg == WM_ERASEBKGND) { m.Result = System.IntPtr.Zero; return; }
            base.WndProc(ref m);
        }

        public System.IntPtr HostHwnd => Handle;
    }
}