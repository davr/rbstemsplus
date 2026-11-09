// The watcher (app/Sources/Watcher.swift): started at sign-in (HKCU Run, "--watch"), it notices a
// rekordbox update (a new "rekordbox 7.x.y" folder, a replaced onnxruntime.dll) or a new STEMS
// Engine, waits for the installer and for rekordbox to close, and if something the user chose went
// missing asks: Reinstall Now, Remind Me Later, Don't Ask Again for This Version. It never changes
// anything itself: Reinstall Now writes the request file and opens the app.
using System.Diagnostics;

namespace RBStemsPlus;

sealed class Watcher : ApplicationContext
{
    readonly AppPaths p;
    readonly Log log;
    readonly List<FileSystemWatcher> watchers = new();
    readonly System.Windows.Forms.Timer timer = new() { Interval = 60_000 };
    readonly EventWaitHandle quit = new(false, EventResetMode.ManualReset, WatcherRegistration.QuitEvent);
    DateTime dirtySince = DateTime.MinValue;
    bool asking;
    int busySeconds;                                      // how long the app has been busy, in a row
    string? lastWhy;                                      // what the log last said about waiting
    readonly HashSet<string> remindLater = new();

    public Watcher(AppPaths p)
    {
        this.p = p;
        log = new Log(p.WatcherLog);
        log.Write($"watcher started (RB Stems Plus {AppVersion.Current})");
        Watch(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "rekordbox"), true);
        Watch(Path.Combine(p.RbSettingsDir, "models"), true);
        timer.Tick += (_, _) => Tick();
        timer.Start();
        dirtySince = DateTime.UtcNow;                         // a check at sign-in too
        ThreadPool.RegisterWaitForSingleObject(quit, (_, _) => { log.Write("watcher: asked to quit"); ExitThreadCore(); }, null, Timeout.Infinite, true);
    }

    void Watch(string dir, bool sub)
    {
        try
        {
            if (!Directory.Exists(dir)) return;
            var w = new FileSystemWatcher(dir) { IncludeSubdirectories = sub, NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName | NotifyFilters.LastWrite | NotifyFilters.Size };
            w.Changed += Dirty; w.Created += Dirty; w.Deleted += Dirty; w.Renamed += (s, e) => Dirty(s, e);
            w.EnableRaisingEvents = true;
            watchers.Add(w);
        }
        catch (Exception e) { log.Write($"watcher: can't watch {dir}: {e.Message}"); }
    }

    void Dirty(object? s, FileSystemEventArgs e)
    {
        if (e.FullPath.Contains(".rbsp-") || e.FullPath.EndsWith(".part")) return;   // our own temporary files
        dirtySince = DateTime.UtcNow;
    }

    int lastSessionPid;
    readonly SynchronizationContext? ui = SynchronizationContext.Current;

    /// As rekordbox opens: its prepared playlists are checked, once per session (its pid).
    void CheckSession()
    {
        int pid;
        try { pid = Process.GetProcessesByName("rekordbox").Select(x => x.Id).FirstOrDefault(); } catch (InvalidOperationException) { return; }
        if (pid == 0 || pid == lastSessionPid) return;
        lastSessionPid = pid;
        Task.Run(() =>
        {
            try { PrepareWatch.Check(p, log, (title, body) => ui?.Post(_ => Toast.Show(title, body), null)); }
            catch (Exception e) { log.Write("prepared playlists: check failed: " + e.Message); }
        });
    }

    void Tick()
    {
        CheckSession();
        if (asking || dirtySince == DateTime.MinValue) return;
        if (DateTime.UtcNow - dirtySince < TimeSpan.FromSeconds(20)) return;          // let the change finish
        bool busy = State.AppBusy(p);
        if (busy) busySeconds += (int)(timer.Interval / 1000); else busySeconds = 0;
        var d = WatcherLogic.Decide(Rekordbox.PioneerUpdating(), busy, Rekordbox.Running(), busySeconds,
                                    () => State.Leftover(p) is { } a && a.Id.StartsWith("uninstall") ? a.Id : null, out var why);
        if (why != null && why != lastWhy) { log.Write("watcher: " + why); lastWhy = why; }
        if (d == Settle.Wait) return;
        lastWhy = null;
        dirtySince = DateTime.MinValue;
        if (d == Settle.Skip) return;
        var rb = Rekordbox.Current();
        var m = Payload.Saved(new PayloadFiles(p));
        var keys = State.MissingKeys(p, rb, m).Where(k => !remindLater.Contains(k)).ToList();
        if (keys.Count == 0) return;
        log.Write("watcher: missing " + string.Join(", ", keys));
        Ask(keys, rb, m);
    }

    void Ask(List<string> keys, RekordboxInstall? rb, Manifest? m)
    {
        asking = true;
        try
        {
            bool cache = keys.Any(k => k.StartsWith("cache:")), model = keys.Any(k => k.StartsWith("model:"));
            var list = m?.Compat ?? Payload.BuiltIn;
            bool supported = (!cache || Compat.StemsCacheBlocked(p, list, rb) == null) && (!model || Compat.StemsPlusBlocked(p, list, rb) == null);
            var what = cache && model ? "Stems Cache and Demucs v4 are off" : cache ? "Stems Cache is off (Demucs v4 still works)" : "Demucs v4 is off";
            var heading = cache ? "rekordbox was updated" : "rekordbox downloaded a new STEMS Engine";
            var text = supported ? $"{what}. Reinstall to turn it back on." + (cache ? " The stems it saved are kept." : "")
                                 : $"{what}. RB Stems Plus doesn't support this version yet: it checks again each time you open it.";
            var now = new TaskDialogButton("Reinstall Now");
            var later = new TaskDialogButton("Remind Me Later");
            var never = new TaskDialogButton("Don't Ask Again for This Version");
            var page = new TaskDialogPage { Caption = "RB Stems Plus", Heading = heading, Text = text, Buttons = supported ? new TaskDialogButtonCollection { now, later, never } : new TaskDialogButtonCollection { later, never } };
            var r = TaskDialog.ShowDialog(page, TaskDialogStartupLocation.CenterScreen);
            if (r == now)
            {
                log.Write("watcher: Reinstall Now");
                State.WriteReinstallRequest(p);
                Process.Start(new ProcessStartInfo(Environment.ProcessPath!) { UseShellExecute = false });
            }
            else if (r == never) { log.Write("watcher: don't ask again for " + string.Join(", ", keys)); State.Remember(p, keys); }
            else { log.Write("watcher: remind me later"); foreach (var k in keys) remindLater.Add(k); }
        }
        finally { asking = false; }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) { foreach (var w in watchers) w.Dispose(); timer.Dispose(); quit.Dispose(); }
        base.Dispose(disposing);
    }
}
