// What the app does (app/Sources/Actions.swift on macOS): install, reinstall and uninstall the two
// features, off the UI thread. Every change downloads and verifies what it needs first, checks
// that rekordbox is closed, then changes rekordbox. Demucs v4 runs as the user; Stems Cache's
// changes run in a second copy of the app started through UAC (Elevated.cs).
using System.ComponentModel;
using System.Diagnostics;
using Microsoft.Win32;

namespace RBStemsPlus;

/// What the engine asks the window for (always called on the UI thread by the window's adapter).
public interface IAsk
{
    bool Confirm(string title, string text, string ok, string cancel = "Cancel");
    void Tell(string title, string text);
    /// rekordbox is open: "Try Again" (true) or Cancel.
    bool RetryWhileRekordboxOpen(string what);
}

public sealed class Engine
{
    readonly AppPaths p;
    readonly Log log;
    readonly Action<string> status;
    readonly IAsk ask;
    /// The progress bar: a percent, null for "working" (no percentage), Hidden to hide it.
    readonly Action<int?> progress;
    public const int Hidden = -1;
    /// The step label ("Step 2 of 4: …"); "" hides it.
    readonly Action<string> stepLabel;
    int stepNo, stepCount;

    public Engine(AppPaths p, Log log, Action<string> status, IAsk ask, Action<int?>? progress = null, Action<string>? stepLabel = null)
    {
        this.p = p; this.log = log; this.status = status; this.ask = ask;
        this.progress = progress ?? (_ => { });
        this.stepLabel = stepLabel ?? (_ => { });
    }

    /// Starts a numbered list of steps for the action under way (`count` of them).
    void Steps(int count) { stepNo = 0; stepCount = count; }
    void Step(string what)
    {
        stepNo++;
        var line = stepCount > 1 ? $"Step {Math.Min(stepNo, stepCount)} of {stepCount}: {what}" : what;
        stepLabel(line);
        log.Write("  " + line);
    }
    public void EndSteps() { stepLabel(""); progress(Hidden); stepCount = 0; }

    Models ModelsFor(Manifest? m)
    {
        var models = Models.ForAccount(p, m, log.Write);
        models.Progress = progress;
        models.Status = status;
        return models;
    }

    PayloadFiles PF => new(p);
    public Manifest? Saved => Payload.Saved(PF, log.Write);
    public Manifest.Compatible CompatList => Saved?.Compat ?? Payload.BuiltIn;

    /// The payload for an install: downloaded now, or the saved one (offline) if its files are here.
    Manifest? PayloadForInstall(bool needBridge, bool needModel)
    {
        Step("Checking for the latest files");
        status("Checking for the latest files…");
        progress(null);
        var (r, m) = Payload.FetchChecked(p, PF, log.Write);
        if (r == Payload.Fetch.Unverified) { ask.Tell("Download failed", "The download couldn't be verified. Nothing was changed. Try again later."); return null; }
        if (m != null) return m;
        var saved = Saved;
        if (saved != null && (!needModel || Files.Verified(saved.ModelPath(p), saved.Model.Sha256, saved.Model.Size))
                          && (!needBridge || Files.Verified(saved.BridgePath(p), saved.Bridge.Sha256, null)))
        {
            log.Write("offline: using the files downloaded earlier");
            return saved;
        }
        ask.Tell("No connection", "Nothing was changed. Check your internet connection and try again.");
        return null;
    }

    bool Fetch(Manifest m, bool bridge, bool model)
    {
        if (model)
        {
            Step("Downloading the Demucs v4 model (about 300 MB)");
            var r = Payload.Asset(p, m.Model.File, m.ModelPath(p), m.Model.Sha256, m.Model.Size, "the Demucs v4 model", status, log.Write, progress: progress);
            if (r != Payload.Fetched.Ok) { TellFetch(r); return false; }
        }
        if (bridge)
        {
            Step("Downloading Stems Cache");
            var r = Payload.Asset(p, m.Bridge.File, m.BridgePath(p), m.Bridge.Sha256, null, "Stems Cache", status, log.Write, progress: progress);
            if (r != Payload.Fetched.Ok) { TellFetch(r); return false; }
        }
        return true;
    }

    void TellFetch(Payload.Fetched r) => ask.Tell("Download failed", r switch
    {
        Payload.Fetched.NoSpace => "The disk is full. Nothing was changed. Free up some space and try again.",
        Payload.Fetched.Damaged => "A file didn't download correctly. Nothing was changed. Try again later.",
        _ => "Nothing was changed. Check your internet connection and try again.",
    });

    bool WaitForRekordboxClosed(string what)
    {
        progress(null);
        while (Rekordbox.Running() || Rekordbox.PioneerUpdating())
        {
            if (!ask.RetryWhileRekordboxOpen(what)) { log.Write($"{what}: cancelled (rekordbox open)"); return false; }
        }
        return true;
    }

    // ---------------------------------------------------------------- Demucs v4

    public bool InstallModel(Manifest? given = null)
    {
        var rb = Rekordbox.Current();
        if (Compat.StemsPlusBlocked(p, CompatList, rb) is { } why) { ask.Tell("Demucs v4 can't be installed", why); return false; }
        if (given == null) Steps(3);
        var m = given ?? PayloadForInstall(needBridge: false, needModel: true);
        if (m == null || !Fetch(m, bridge: false, model: true)) return false;
        if (!WaitForRekordboxClosed("Install Demucs v4")) return false;
        Step("Installing Demucs v4 in rekordbox");
        status("Installing Demucs v4…");
        var r = ModelsFor(m).InstallOurs(m.ModelPath(p), m.Model.Sha256);
        if (!r.Ok) { ask.Tell("Demucs v4 wasn't installed", $"Demucs v4 couldn't be installed. {r.Message} Nothing was changed."); return false; }
        var c = State.ReadChosen(p);
        State.SetChosen(p, true, c.Cache);
        log.Write("Demucs v4: on");
        return true;
    }

    public bool UninstallModel()
    {
        if (!WaitForRekordboxClosed("Uninstall Demucs v4")) return false;
        Steps(1);
        Step("Putting rekordbox's own stems model back");
        var r = ModelsFor(Saved).Restore();
        if (!r.Ok)
        {
            ask.Tell("Demucs v4 wasn't removed", r.NoSpace ? $"Demucs v4 couldn't be removed. {r.Message} Free up some space and try again."
                                                          : $"Demucs v4 couldn't be removed. {r.Message} See Help > Troubleshooting.");
            return false;
        }
        State.SetChosen(p, false, State.ReadChosen(p).Cache);
        log.Write("Demucs v4: off");
        return true;
    }

    // ---------------------------------------------------------------- Stems Cache

    public bool InstallCache(Manifest? given = null)
    {
        var rb = Rekordbox.Current();
        if (Compat.StemsCacheBlocked(p, CompatList, rb) is { } why) { ask.Tell("Stems Cache can't be installed", why); return false; }
        if (given == null) Steps(3);
        var m = given ?? PayloadForInstall(needBridge: true, needModel: false);
        if (m == null || !Fetch(m, bridge: true, model: false)) return false;
        if (!WaitForRekordboxClosed("Install Stems Cache")) return false;
        Step("Installing Stems Cache in rekordbox (Windows asks for permission)");
        progress(null);
        status("Installing Stems Cache… (Windows asks for permission)");
        var r = RunElevated("install-cache", rb!.Dir, rb.Version, m.BridgePath(p), m.Bridge.Sha256);
        if (r != AdminExit.Ok) { ask.Tell("Stems Cache wasn't installed", Elevated.Describe(r)); return false; }
        State.SetChosen(p, State.ReadChosen(p).Model, true);
        if (!ConfigFile.Sets(p.ConfigFile, "rekordbox_model"))
            log.Write($"Stems Cache: saving rekordbox's own model's stems too (rekordbox_model=1): {(ConfigFile.WriteRekordboxModel(p.ConfigFile, true) ? "written" : "COULDN'T BE WRITTEN")}");
        WatcherRegistration.Register(log.Write);
        var (free, disk) = State.Space(p.DefaultCacheDir);
        if (State.PausedFloor(free, disk) is { } floor)
            ask.Tell("Stems Cache is installed", $"Stems Cache will stop saving stems while your disk has less than {State.FloorGB(floor)} GB free.");
        log.Write("Stems Cache: on");
        return true;
    }

    public bool UninstallCache()
    {
        if (!WaitForRekordboxClosed("Uninstall Stems Cache")) return false;
        Steps(1);
        Step("Putting rekordbox's original files back (Windows asks for permission)");
        status("Uninstalling Stems Cache… (Windows asks for permission)");
        var r = RunElevated("uninstall-cache");
        if (r != AdminExit.Ok) { ask.Tell("Stems Cache wasn't removed", Elevated.Describe(r)); return false; }
        State.SetChosen(p, State.ReadChosen(p).Model, false);
        log.Write("Stems Cache: off (the stems already saved are kept)");
        return true;
    }

    /// Reinstalls what the user chose that went missing (after a rekordbox update or a new STEMS Engine).
    public bool ReinstallMissing()
    {
        var c = State.ReadChosen(p);
        var rb = Rekordbox.Current();
        bool needModel = c.Model && !Rekordbox.StemsPlusPresent(p, Saved), needCache = c.Cache && !Rekordbox.StemsCachePresent(rb);
        if (!needModel && !needCache) { log.Write("reinstall: nothing missing"); return true; }
        Steps(1 + (needModel ? 2 : 0) + (needCache ? 2 : 0));
        var m = PayloadForInstall(needCache, needModel);
        if (m == null) return false;
        bool ok = true;
        if (needModel) ok &= InstallModel(m);
        if (needCache) ok &= InstallCache(m);
        return ok;
    }

    /// Everything off and RB Stems Plus's files deleted (the app itself is deleted by the window, last).
    public bool UninstallAll()
    {
        if (!WaitForRekordboxClosed("Uninstall RB Stems Plus")) return false;
        var rb = Rekordbox.Current();
        Steps(3);
        Step("Removing Stems Cache (Windows asks for permission)");
        if (Rekordbox.StemsCachePresent(rb) || Directory.Exists(p.RootDir))
        {
            progress(null);
            status("Uninstalling Stems Cache… (Windows asks for permission)");
            var r = RunElevated("uninstall-cache");
            if (r != AdminExit.Ok) { ask.Tell("RB Stems Plus wasn't removed", "Stems Cache couldn't be removed, so nothing else was deleted. " + Elevated.Describe(r)); return false; }
        }
        Step("Putting rekordbox's own stems model back");
        var models = ModelsFor(Saved);
        var restore = models.Restore();
        if (!restore.Ok)
        {
            if (!ask.Confirm("Demucs v4 couldn't be removed", restore.Message + " Remove RB Stems Plus anyway? rekordbox's saved model is kept in "
                             + p.OriginalsDir + ".", "Remove RB Stems Plus Anyway")) return false;
        }
        WatcherRegistration.Unregister(log.Write);
        Step("Deleting saved stems and settings");
        progress(null);
        status("Deleting saved stems and settings…");
        CacheLocation.ClearEntries(p.DefaultCacheDir, log.Write);
        try { if (Directory.Exists(p.DefaultCacheDir)) Directory.Delete(p.DefaultCacheDir, true); } catch { }
        foreach (var f in new[] { p.ChosenFile, p.DismissedFile, p.RequestFile, p.ConfigFile, p.ManifestFile, p.ManifestSigFile, p.PayloadFloorFile,
                                  p.WelcomedFile, p.LocationHintFile, p.LastUpdateCheckFile })
            Files.Remove(f);
        try { if (Directory.Exists(p.PayloadRoot)) Directory.Delete(p.PayloadRoot, true); } catch { }
        try { if (Directory.Exists(p.UpdateDir)) Directory.Delete(p.UpdateDir, true); } catch { }
        if (restore.Ok) try { if (Directory.Exists(p.OriginalsDir)) Directory.Delete(p.OriginalsDir, true); } catch { }
        log.Write("RB Stems Plus: uninstalled");
        return true;
    }

    /// Starts this exe again as an administrator (UAC) for one operation and waits for it.
    AdminExit RunElevated(params string[] args)
    {
        var exe = Environment.ProcessPath!;
        var psi = new ProcessStartInfo(exe) { UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden };
        psi.ArgumentList.Add("--elevated");
        foreach (var a in args) psi.ArgumentList.Add(a);
        try
        {
            adminLogSeen = Files.Size(p.AdminLog) ?? 0;     // only what this step writes is shown
            using var proc = Process.Start(psi)!;
            proc.WaitForExit();
            var r = Enum.IsDefined(typeof(AdminExit), proc.ExitCode) ? (AdminExit)proc.ExitCode : AdminExit.Failed;
            log.Write($"elevated {args[0]}: {r} (details in admin.log)");
            foreach (var line in AdminLogTail()) log.Write("  " + line);
            return r;
        }
        catch (Win32Exception e) when (e.NativeErrorCode == 1223)
        {
            log.Write($"elevated {args[0]}: the administrator permission was declined");
            return AdminExit.NotAdmin;
        }
    }

    long adminLogSeen;
    IEnumerable<string> AdminLogTail()
    {
        try
        {
            using var fs = new FileStream(p.AdminLog, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            if (adminLogSeen > fs.Length) adminLogSeen = 0;
            fs.Position = adminLogSeen;
            var text = new StreamReader(fs).ReadToEnd();
            adminLogSeen = fs.Length;
            return text.Split('\n', StringSplitOptions.RemoveEmptyEntries).Where(l => l.Contains("Stems Cache") || l.Contains("FAILED")).Select(l => l.Length > 21 ? l[21..] : l);
        }
        catch { return Array.Empty<string>(); }
    }
}

/// The watcher's start at sign-in: HKCU\...\Run, which the user can turn off in Settings > Apps >
/// Startup (StartupApproved\Run then holds a value starting with 3).
public static class WatcherRegistration
{
    const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    const string ApprovedKey = @"Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run";

    public static string Command => $"\"{Environment.ProcessPath}\" --watch";
    public const string QuitEvent = @"Local\io.github.rbstemsplus.watcher.quit";

    public static bool Registered()
    {
        using var k = Registry.CurrentUser.OpenSubKey(RunKey);
        return k?.GetValue(Consts.WatcherRunName) is string;
    }

    public static bool Disabled()
    {
        using var k = Registry.CurrentUser.OpenSubKey(ApprovedKey);
        return k?.GetValue(Consts.WatcherRunName) is byte[] b && b.Length > 0 && (b[0] & 1) == 1;
    }

    public static void Register(Action<string> log)
    {
        try
        {
            using var k = Registry.CurrentUser.CreateSubKey(RunKey);
            if (k.GetValue(Consts.WatcherRunName) as string != Command) { k.SetValue(Consts.WatcherRunName, Command); log("watcher: registered to start at sign-in"); }
            Process.Start(new ProcessStartInfo(Environment.ProcessPath!, "--watch") { UseShellExecute = false });
        }
        catch (Exception e) { log("watcher: couldn't be registered: " + e.Message); }
    }

    public static void Unregister(Action<string> log)
    {
        try
        {
            using var k = Registry.CurrentUser.OpenSubKey(RunKey, true);
            k?.DeleteValue(Consts.WatcherRunName, false);
            using var a = Registry.CurrentUser.OpenSubKey(ApprovedKey, true);
            a?.DeleteValue(Consts.WatcherRunName, false);
            // the running watcher quits when this event is set (Watcher.cs)
            try { if (EventWaitHandle.TryOpenExisting(QuitEvent, out var ev)) using (ev) ev.Set(); } catch { }
            log("watcher: removed");
        }
        catch (Exception e) { log("watcher: couldn't be removed: " + e.Message); }
    }
}
