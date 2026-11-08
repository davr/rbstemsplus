// Whether each feature can be installed on this PC (app/Sources/Compat.swift). The lists come from
// payload-windows.json's "compatible" block (Payload.BuiltIn until one is downloaded).
// - Demucs v4: rekordbox 7 is installed and its STEMS Engine version is listed.
// - Stems Cache: the rekordbox version is listed, and Pioneer's ONNX Runtime is 1.18.x and validly
//   signed by AlphaTheta. Windows asks for an administrator's consent (UAC) when it is installed.
namespace RBStemsPlus;

public static class Compat
{
    /// The end of every "not supported yet" reason.
    public const string ChecksAgain = " RB Stems Plus checks again each time you open it.";

    public static string? StemsPlusBlocked(AppPaths p, Manifest.Compatible list, RekordboxInstall? rb)
    {
        if (rb == null) return "rekordbox 7 isn't installed.";
        var engine = Rekordbox.EngineVersion(p.ModelDir);
        if (engine.Length == 0) return "Open rekordbox and turn on STEMS once, so it downloads its STEMS Engine.";
        if (!list.StemsEngine.Contains(engine)) return $"Demucs v4 isn't available for STEMS Engine {engine} yet." + ChecksAgain;
        return null;
    }

    public static string? StemsCacheBlocked(AppPaths p, Manifest.Compatible list, RekordboxInstall? rb)
    {
        if (rb == null) return "rekordbox 7 isn't installed.";
        if (!Rekordbox.Listed(rb.Version, list.Rekordbox)) return $"Stems Cache doesn't support rekordbox {rb.ShortVersion} yet." + ChecksAgain;
        // Pioneer's library: in rekordbox, or (bridge already in) the admin-only copy the bridge loads
        var lib = Rekordbox.StemsCachePresent(rb) ? p.RootOrt : rb.Ort;
        if (!File.Exists(lib)) return "Stems Cache can't find rekordbox's ONNX Runtime: reinstall rekordbox.";
        if (!Rekordbox.SignedByPioneer(lib)) return "rekordbox's ONNX Runtime isn't the original: reinstall rekordbox.";
        var ort = Rekordbox.OrtVersion(lib) ?? "unknown";
        if (!ort.StartsWith(list.OrtVersionPrefix)) return $"Stems Cache doesn't support ONNX Runtime {ort} yet." + ChecksAgain;
        return null;
    }

    public static bool BlockedByVersionList(string? a, string? b) => (a?.EndsWith(ChecksAgain) ?? false) || (b?.EndsWith(ChecksAgain) ?? false);
}
