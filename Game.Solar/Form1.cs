using System;
using System.Windows.Forms;
using Framework.Renderer;

namespace Game.Solar
{
    public partial class Form1 : Form
    {
        private Renderer? _renderer;
        private bool _running;

        public Form1()
        {
            InitializeComponent();
            Text = "Hello-Interop";
            DoubleBuffered = true;
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);

            System.Diagnostics.Debug.WriteLine($"Proc x64: {Environment.Is64BitProcess}");
            System.Diagnostics.Debug.WriteLine($"BaseDir: {AppContext.BaseDirectory}");
            System.Diagnostics.Debug.WriteLine(
                $"DLL exists: {System.IO.File.Exists(System.IO.Path.Combine(AppContext.BaseDirectory, "RendererNative.dll"))}");

            _renderer = Renderer.Create(
                hwnd: this.Handle,
                width: (uint)ClientSize.Width,
                height: (uint)ClientSize.Height,
                vsync: false,
                validation: true);

            _running = true;
            Application.Idle += Tick;
        }

        protected override void OnSizeChanged(EventArgs e)
        {
            base.OnSizeChanged(e);
            if (_renderer != null && ClientSize.Width > 0 && ClientSize.Height > 0)
                _renderer.Resize((uint)ClientSize.Width, (uint)ClientSize.Height);
        }

        private void Tick(object? sender, EventArgs e)
        {
            if (!_running || _renderer is null) return;

            // Clear only (no geometry yet)
            _renderer.Begin(0.02f, 0.03f, 0.05f, 1f);
            _renderer.End();
            _renderer.Present();

            Invalidate(); // keep WM_PAINT flowing
        }

        protected override void OnFormClosed(FormClosedEventArgs e)
        {
            _running = false;
            Application.Idle -= Tick;
            _renderer?.Dispose();
            base.OnFormClosed(e);
        }
    }
}
