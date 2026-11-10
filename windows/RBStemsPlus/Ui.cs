namespace RBStemsPlus;

/// Sizes in 96-dpi pixels, scaled to the screen's dpi (the tabs lay out by fixed sizes), and a safe way onto the
/// window's thread.
static class Ui
{
    static readonly Lazy<float> scale = new(() => { using var c = new Control(); return Math.Max(1f, c.DeviceDpi / 96f); });
    public static int S(int px) => (int)Math.Round(px * scale.Value);

    /// Runs a on c's thread, later. From any thread: nothing is thrown at the caller (a helper's output thread would
    /// take the app down), and an error in a is logged with its stack instead of reaching .NET's error dialog.
    public static void Post(Control c, Log log, string what, Action a)
    {
        try
        {
            if (c.IsDisposed || !c.IsHandleCreated) return;
            c.BeginInvoke(() =>
            {
                try { if (!c.IsDisposed) a(); }
                catch (Exception e) { log.Write($"{what}: ERROR (carried on): {e}"); }
            });
        }
        catch (Exception e) when (e is InvalidOperationException or ObjectDisposedException) { }   // the window is closing
    }
}
