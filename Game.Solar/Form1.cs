using System;
using System.Windows.Forms;
using Framework.Renderer;

namespace Game.Solar
{
    public partial class Form1 : Form
    {
        private Renderer? _renderer;
        private bool _running;

        public Form1() { InitializeComponent(); Text = "Hello-Interop"; DoubleBuffered = true; }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            System.Diagnostics.Debug.WriteLine($"Proc x64: {Environment.Is64BitProcess}");
            System.Diagnostics.Debug.WriteLine($"BaseDir: {AppContext.BaseDirectory}");
            System.Diagnostics.Debug.WriteLine($"DLL exists: {System.IO.File.Exists(System.IO.Path.Combine(AppContext.BaseDirectory, "RendererNative.dll"))}");
            _renderer = Renderer.Create(this.Handle);   // create native device
            _running = true;
            Application.Idle += Tick;
        }

        private void Tick(object? s, EventArgs e)
        {
            if (!_running || _renderer is null) return;
            _renderer.BeginFrame();   // no-op for now, just exercising the call path
            _renderer.EndFrame();
            Invalidate();
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
