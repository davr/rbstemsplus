// The small files the app and the watcher share (app/Sources/State.swift): installed (the user's
// choice), dismissed (prompts not to show again), request (the watcher asks the app to reinstall)
// and busy (the app is changing things); the bridge's bridge-state; the free-space floor.
using System.Diagnostics;

namespace RBStemsPlus;

public sealed record Chosen(bool Model, bool Cache);
public sealed record BusyAction(string Id, string Label);
public sealed record BusyMarker(int Pid, long? Start, BusyAction? Action, DateTime? Written);
public sealed record BridgeState(int Pid, long Start, string Location, string State, string Reason);

public static class State
{
    public static Chosen ReadChosen(AppPaths p)
    {
        var s = Files.ReadText(p.ChosenFile) ?? "";
        return new(s.Contains("model=1"), s.Contains("cache=1"));
    }

    public static void SetChosen(AppPaths p, bool model, bool cache) =>
        Files.TryWriteText(p.ChosenFile, $"model={(model ? 1 : 0)}\ncache={(cache ? 1 : 0)}\n");

    public static HashSet<string> Dismissed(AppPaths p) =>
        (Files.ReadText(p.DismissedFile) ?? "").Split('\n', StringSplitOptions.RemoveEmptyEntries).ToHashSet();

    public static void Remember(AppPaths p, IEnumerable<string> keys) =>
        Files.TryWriteText(p.DismissedFile, string.Join("\n", Dismissed(p).Union(keys).OrderBy(x => x, StringComparer.Ordinal)) + "\n");

    /// What went missing from what the user chose, as version-specific prompt keys.
    public static List<string> MissingKeys(AppPaths p, RekordboxInstall? rb, Manifest? m)
    {
        var c = ReadChosen(p);
        var keys = new List<string>();
        if (c.Cache && !Rekordbox.StemsCachePresent(rb)) keys.Add("cache:" + (rb?.Version ?? ""));
        if (c.Model && !Rekordbox.StemsPlusPresent(p, m)) keys.Add("model:" + Rekordbox.EngineVersion(p.ModelDir));
        var d = Dismissed(p);
        return keys.Where(k => !d.Contains(k)).ToList();
    }

    public static void WriteReinstallRequest(AppPaths p) => Files.TryWriteText(p.RequestFile, "reinstall\n");

    public static TimeSpan? TakeReinstallRequest(AppPaths p)
    {
        try
        {
            var fi = new FileInfo(p.RequestFile);
            if (!fi.Exists) return null;
            var age = DateTime.UtcNow - fi.LastWriteTimeUtc;
            fi.Delete();
            return age < TimeSpan.Zero ? TimeSpan.Zero : age;
        }
        catch { return null; }
    }

    // ---------------------------------------------------------------- processes

    /// When process `pid` started, in whole seconds since 1970; null if there is none (or it can't be read).
    public static long? ProcessStart(int pid)
    {
        try
        {
            using var proc = Process.GetProcessById(pid);
            return new DateTimeOffset(proc.StartTime.ToUniversalTime()).ToUnixTimeSeconds();
        }
        catch { return null; }
    }

    // ---------------------------------------------------------------- busy

    public static void SetBusy(AppPaths p, BusyAction? a)
    {
        if (a == null) { Files.Remove(p.BusyFile); return; }
        int pid = Environment.ProcessId;
        Files.TryWriteText(p.BusyFile, $"{pid}\n{ProcessStart(pid) ?? 0}\n{a.Id}\n{a.Label}\n");
    }

    public static BusyMarker? ReadBusy(AppPaths p)
    {
        var s = Files.ReadText(p.BusyFile);
        if (s == null) return null;
        var lines = s.Split('\n').Select(l => l.Trim()).ToArray();
        if (!int.TryParse(lines[0], out var pid) || pid <= 0) return null;
        long? start = lines.Length >= 2 && lines[1].Length > 0 && lines[1].All(char.IsAsciiDigit) ? long.Parse(lines[1]) : null;
        var rest = lines.Skip(start == null ? 1 : 2).ToArray();
        var action = rest.Length >= 2 && rest[0].Length > 0 ? new BusyAction(rest[0], rest[1]) : null;
        DateTime? written = null;
        try { written = File.GetLastWriteTimeUtc(p.BusyFile); } catch { }
        return new(pid, start, action, written);
    }

    public static bool MarkerLive(BusyMarker m, Func<int, long?>? startOf = null)
    {
        startOf ??= ProcessStart;
        if (startOf(m.Pid) is not { } started) return false;
        if (m.Start is { } s) return started == s;
        if (m.Written is not { } w) return false;
        return started <= new DateTimeOffset(w).ToUnixTimeSeconds();
    }

    public static bool AppBusy(AppPaths p) => ReadBusy(p) is { } m && MarkerLive(m);

    /// The action an earlier run left half-done (its process is gone), or null.
    public static BusyAction? Leftover(AppPaths p)
    {
        if (ReadBusy(p) is not { } m) return null;
        for (int waited = 0; MarkerLive(m); waited++)
        {
            if (m.Pid == Environment.ProcessId || waited >= 20) return null;
            Thread.Sleep(100);
        }
        return m.Action;
    }

    // ---------------------------------------------------------------- bridge-state

    public static BridgeState? ParseBridgeState(string text)
    {
        var v = new Dictionary<string, string>();
        foreach (var line in text.Split('\n'))
        {
            int eq = line.IndexOf('=');
            if (eq > 0) v[line[..eq]] = line[(eq + 1)..].TrimEnd('\r');
        }
        if (!v.TryGetValue("pid", out var ps) || !int.TryParse(ps, out var pid) || pid <= 0) return null;
        if (!v.TryGetValue("start", out var ss) || !long.TryParse(ss, out var start)) return null;
        if (!v.TryGetValue("state", out var state) || state is not ("pending" or "available" or "unavailable")) return null;
        return new(pid, start, v.GetValueOrDefault("location", ""), state, v.GetValueOrDefault("reason", ""));
    }

    /// bridge-state while the rekordbox that wrote it still runs (the same pid and start time,
    /// within a second: the bridge and .NET round differently), for the folder in use (`dir`).
    public static BridgeState? LiveBridgeState(AppPaths p, string dir, Func<int, long?>? startOf = null)
    {
        var t = Files.ReadText(p.BridgeStateFile);
        if (t == null || ParseBridgeState(t) is not { } s) return null;
        startOf ??= ProcessStart;
        if (startOf(s.Pid) is not { } st || Math.Abs(st - s.Start) > 1) return null;
        return string.Equals(s.Location, dir, StringComparison.OrdinalIgnoreCase) ? s : null;
    }

    public static string ReasonText(string r) => r switch
    {
        "config" => "config.ini can't be read, or sets the location wrongly",
        "disabled" => "Stems Cache is off, or isn't hooked into this rekordbox",
        "not-connected" => "its drive isn't connected",
        "folder" => "its folder is missing",
        "marker" => "its folder's marker is missing or another folder's",
        "read-only" => "the drive is read-only",
        "not-local" => "it's a network drive",
        "no-permission" => "no permission",
        "write-failed" => "writing failed",
        _ => "the drive doesn't answer",
    };

    // ---------------------------------------------------------------- free space

    public const long FreeFloorMax = 50_000_000_000;

    /// Stems Cache saves nothing while its disk has less free than 10% of its size, at most 50 GB
    /// (the bridge's free_floor).
    public static long FreeFloor(long? disk) => disk is > 0 ? Math.Min(FreeFloorMax, disk.Value / 10) : FreeFloorMax;

    public static long FloorGB(long floor) => (floor + 500_000_000) / 1_000_000_000;

    public static (long? Free, long? Disk) Space(string path)
    {
        try
        {
            var p = path;
            while (!Directory.Exists(p) && Path.GetDirectoryName(p) is { } up && up != p) p = up;
            var d = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(p))!);
            return (d.AvailableFreeSpace, d.TotalSize);
        }
        catch { return (null, null); }
    }

    public static long? PausedFloor(long? free, long? disk)
    {
        var f = FreeFloor(disk);
        return free is { } x && x < f ? f : null;
    }
}
