using System;
using System.Diagnostics;
using System.Windows.Forms;
using Framework.Renderer; // Renderer

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
            DoubleBuffered = false;
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);

            Debug.WriteLine($"Proc x64: {Environment.Is64BitProcess}");
            Debug.WriteLine($"BaseDir: {AppContext.BaseDirectory}");

            // NEW: single-argument factory (old width/height/vsync/validation removed)
            _renderer = Renderer.Create(this.Handle);

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
            if (!_running || _renderer == null) return;

            // simple clear + present (native uses a fixed clear color internally for now)
            _renderer.Begin(0.02f, 0.03f, 0.05f, 1f);
            _renderer.End();
            _renderer.Present();

            Invalidate(); // keep WM_PAINT flowing
        }

        protected override void OnHandleDestroyed(EventArgs e)
        {
            Application.Idle -= Tick;
            _renderer?.Dispose();
            _renderer = null;
            base.OnHandleDestroyed(e);
        }
    }
}
