// What is on this PC right now: rekordbox 7, its versions, and whether our two features are in
// place (app/Sources/Rekordbox.swift and Compat.swift). rekordbox 7 for Windows installs each
// version side by side in C:\Program Files\rekordbox\rekordbox 7.x.y\ (the uninstall key
// "Pioneer rekordbox 7.x.y" has an empty InstallLocation: the folder comes from its
// UninstallString), with onnxruntime.dll (ONNX Runtime 1.18.0, DirectML build, no version
// resource) next to rekordbox.exe. The STEMS model is per account (Paths.ModelDir).
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace RBStemsPlus;

public sealed record RekordboxInstall(string Dir, string Version)
{
    public string Exe => Path.Combine(Dir, "rekordbox.exe");
    public string Ort => Path.Combine(Dir, "onnxruntime.dll");
    public string ShortVersion => Rekordbox.ShortVersion(Version);
}

public static class Rekordbox
{
    /// Overrides discovery (the tests, and a user who installed rekordbox elsewhere: none yet).
    public static Func<IEnumerable<RekordboxInstall>>? DiscoverOverride;

    /// Every rekordbox 7 install, newest version first: from the uninstall keys, else the usual folders.
    public static List<RekordboxInstall> All()
    {
        if (DiscoverOverride != null) return DiscoverOverride().ToList();
        var found = new Dictionary<string, RekordboxInstall>(StringComparer.OrdinalIgnoreCase);
        if (OperatingSystem.IsWindows())
        {
            foreach (var hive in new[] { RegistryHive.LocalMachine, RegistryHive.CurrentUser })
                foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
                {
                    try
                    {
                        using var root = RegistryKey.OpenBaseKey(hive, view).OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall");
                        if (root == null) continue;
                        foreach (var name in root.GetSubKeyNames())
                        {
                            using var k = root.OpenSubKey(name);
                            if (k?.GetValue("DisplayName") is not string dn || !dn.StartsWith("rekordbox 7", StringComparison.OrdinalIgnoreCase)) continue;
                            var dir = (k.GetValue("InstallLocation") as string)?.Trim().Trim('"');
                            if (string.IsNullOrEmpty(dir) && k.GetValue("UninstallString") is string us)
                                dir = Path.GetDirectoryName(us.Trim().Trim('"'));
                            if (string.IsNullOrEmpty(dir)) continue;
                            dir = dir.TrimEnd('\\');
                            var inst = Describe(dir);
                            if (inst != null) found[dir] = inst;
                        }
                    }
                    catch { }
                }
        }
        foreach (var parent in new[] { Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "rekordbox"),
                                       Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Pioneer") })
        {
            try
            {
                foreach (var d in Directory.EnumerateDirectories(parent, "rekordbox 7*"))
                    if (!found.ContainsKey(d) && Describe(d) is { } inst) found[d] = inst;
            }
            catch { }
        }
        return found.Values.OrderByDescending(i => i.Version, Comparer<string>.Create(Payload.CompareVersions)).ToList();
    }

    /// The install in `dir`, if rekordbox.exe is there: its ProductVersion ("7.2.14.0323").
    public static RekordboxInstall? Describe(string dir)
    {
        var exe = Path.Combine(dir, "rekordbox.exe");
        if (!File.Exists(exe)) return null;
        string v = "";
        try { v = FileVersionInfo.GetVersionInfo(exe).ProductVersion?.Trim() ?? ""; } catch { }
        if (v.Length == 0)
        {
            var m = Regex.Match(Path.GetFileName(dir), @"(\d+\.\d+\.\d+)");
            v = m.Success ? m.Groups[1].Value : "";
        }
        return new RekordboxInstall(dir, v);
    }

    /// The rekordbox the user runs: the newest rekordbox 7 installed.
    public static RekordboxInstall? Current() => All().FirstOrDefault();

    /// "7.2.14.0323" → "7.2.14".
    public static string ShortVersion(string v) => string.Join(".", v.Split('.').Take(3));

    /// Whether a version is listed: "7.2.14.0323" matches "7.2.14".
    public static bool Listed(string v, IEnumerable<string> list) => v.Length > 0 && list.Any(x => v == x || v.StartsWith(x + "."));

    /// The STEMS Engine version rekordbox downloaded ("0002"; "" if none yet).
    public static string EngineVersion(string modelDir) => (Files.ReadText(Path.Combine(modelDir, "demucs3_ver.txt")) ?? "").Trim();

    public static bool Running() => Process.GetProcessesByName("rekordbox").Length > 0;

    /// Pioneer's installer or rekordbox's updater is running: rekordbox is about to change.
    public static bool PioneerUpdating() =>
        Process.GetProcesses().Any(p =>
        {
            try { return p.ProcessName.StartsWith("Install_rekordbox", StringComparison.OrdinalIgnoreCase) || p.ProcessName.Equals("Upmgr rekordbox", StringComparison.OrdinalIgnoreCase); }
            catch { return false; }
        });

    // ---------------------------------------------------------------- the bridge and our model

    static byte[]? Read(string path) { try { return File.ReadAllBytes(path); } catch { return null; } }

    public static bool IsBridge(byte[] d) => IndexOf(d, Encoding.ASCII.GetBytes(Consts.BridgeMarker)) >= 0;

    /// The bridge is recognised by its own message text.
    public static bool StemsCachePresent(RekordboxInstall? rb) => rb != null && Read(rb.Ort) is { } d && IsBridge(d);

    /// Our model is recognised by its size (for the lights only; whatever replaces a model decides
    /// by checksum: Models.cs).
    public static bool StemsPlusPresent(AppPaths p, Manifest? m)
    {
        var s = Files.Size(Path.Combine(p.ModelDir, "hdemucs.onnx"));
        return s != null && (s == Consts.OurModelSize || s == m?.Model.Size);
    }

    /// The capability line the bridge carries ("rbstems-cache: rebuild=1 location=1 pioneer=<sha>;").
    static string? CapsLine(byte[] d)
    {
        if (!IsBridge(d)) return null;
        int at = IndexOf(d, Encoding.ASCII.GetBytes(Consts.BridgeCaps));
        if (at < 0) return null;
        at += Consts.BridgeCaps.Length;
        int end = Array.IndexOf(d, (byte)';', at, Math.Min(4096, d.Length - at));
        return end < 0 ? null : Encoding.ASCII.GetString(d, at, end - at);
    }

    /// Which of rekordbox's own models the bridge saves stems for (by checksum); null without a bridge.
    public static List<string>? BridgeCacheModels(byte[] d)
    {
        var line = CapsLine(d);
        if (line == null) return null;
        int p = line.IndexOf("pioneer=", StringComparison.Ordinal);
        return p < 0 ? new() : line[(p + 8)..].Split(',').Where(Files.IsSha256).ToList();
    }

    public static bool BridgeSaysLocation(byte[] d) =>
        CapsLine(d) is { } line && line.Split("pioneer=")[0].Split(' ').Contains("location=1");

    public static int IndexOf(byte[] hay, byte[] needle, int from = 0)
    {
        if (needle.Length == 0) return 0;
        var span = hay.AsSpan(from);
        int i = span.IndexOf(needle);
        return i < 0 ? -1 : i + from;
    }

    /// ONNX Runtime's version, from the standalone "1.x.y" string it keeps (Pioneer's build has no
    /// version resource: "Internal Build"); the version resource when it has one.
    public static string? OrtVersion(string path)
    {
        var d = Read(path);
        if (d == null) return null;
        var found = new HashSet<string>();
        var text = Encoding.Latin1.GetString(d);
        foreach (Match m in Regex.Matches(text, "\0(1\\.[0-9]{1,2}\\.[0-9]{1,3})\0")) found.Add(m.Groups[1].Value);
        if (found.Count == 1) return found.First();
        try
        {
            var v = FileVersionInfo.GetVersionInfo(path).ProductVersion;
            if (v != null && Regex.IsMatch(v, @"^1\.\d+\.\d+")) return Regex.Match(v, @"^1\.\d+\.\d+").Value;
        }
        catch { }
        return found.Count > 0 ? found.OrderBy(x => x, Comparer<string>.Create(Payload.CompareVersions)).Last() : null;
    }

    // ---------------------------------------------------------------- Authenticode

    static readonly Dictionary<string, bool> signedMemo = new();

    /// Whether a file carries a valid Authenticode signature (WinVerifyTrust, the whole chain) by
    /// AlphaTheta (the signer certificate's organisation). Remembered per file version.
    public static bool SignedByPioneer(string path)
    {
        if (!OperatingSystem.IsWindows()) return false;
        FileInfo fi;
        try { fi = new FileInfo(path); if (!fi.Exists) return false; } catch { return false; }
        var key = $"{path}|{fi.Length}|{fi.LastWriteTimeUtc.Ticks}";
        lock (signedMemo) if (signedMemo.TryGetValue(key, out var k)) return k;
        bool ok = Authenticode.Verify(path) && SignerOrganisation(path) == Consts.PioneerSigner;
        lock (signedMemo) signedMemo[key] = ok;
        return ok;
    }

    public static string? SignerOrganisation(string path)
    {
        try
        {
#pragma warning disable SYSLIB0057
            using var c = new X509Certificate2(X509Certificate.CreateFromSignedFile(path));
#pragma warning restore SYSLIB0057
            var o = c.SubjectName.Name.Split(',').Select(x => x.Trim()).FirstOrDefault(x => x.StartsWith("O="));
            return o?[2..].Trim('"');
        }
        catch { return null; }
    }
}

/// WinVerifyTrust with the Authenticode policy: a valid, trusted, unrevoked-as-far-as-cached signature.
static class Authenticode
{
    static readonly Guid Action = new("00AAC56B-CD44-11d0-8CC2-00C04FC295EE");   // WINTRUST_ACTION_GENERIC_VERIFY_V2

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct FileInfo { public uint cbStruct; public string pcwszFilePath; public IntPtr hFile; public IntPtr pgKnownSubject; }

    [StructLayout(LayoutKind.Sequential)]
    struct Data
    {
        public uint cbStruct; public IntPtr pPolicyCallbackData, pSIPClientData; public uint dwUIChoice, fdwRevocationChecks, dwUnionChoice;
        public IntPtr pFile; public uint dwStateAction; public IntPtr hWVTStateData, pwszURLReference; public uint dwProvFlags, dwUIContext;
        public IntPtr pSignatureSettings;
    }

    [DllImport("wintrust.dll", CharSet = CharSet.Unicode)]
    static extern int WinVerifyTrust(IntPtr hwnd, ref Guid action, ref Data data);

    public static bool Verify(string path)
    {
        var fi = new FileInfo { cbStruct = (uint)Marshal.SizeOf<FileInfo>(), pcwszFilePath = path };
        var pFile = Marshal.AllocHGlobal(Marshal.SizeOf<FileInfo>());
        try
        {
            Marshal.StructureToPtr(fi, pFile, false);
            var d = new Data
            {
                cbStruct = (uint)Marshal.SizeOf<Data>(), dwUIChoice = 2 /* WTD_UI_NONE */, fdwRevocationChecks = 0, dwUnionChoice = 1 /* FILE */,
                pFile = pFile, dwStateAction = 1 /* VERIFY */, dwProvFlags = 0x1000 /* WTD_CACHE_ONLY_URL_RETRIEVAL */,
            };
            var g = Action;
            int r = WinVerifyTrust(IntPtr.Zero, ref g, ref d);
            d.dwStateAction = 2;                                 // CLOSE
            WinVerifyTrust(IntPtr.Zero, ref g, ref d);
            return r == 0;
        }
        catch { return false; }
        finally { Marshal.FreeHGlobal(pFile); }
    }
}
