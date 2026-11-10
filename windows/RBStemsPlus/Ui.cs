namespace RBStemsPlus;

/// Sizes in 96-dpi pixels, scaled to the screen's dpi (the tabs lay out by fixed sizes).
static class Ui
{
    static readonly Lazy<float> scale = new(() => { using var c = new Control(); return Math.Max(1f, c.DeviceDpi / 96f); });
    public static int S(int px) => (int)Math.Round(px * scale.Value);
}
