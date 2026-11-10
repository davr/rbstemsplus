// RB Stems Plus for Windows. One exe, four ways to run:
//   RBStemsPlus.exe                 the app (MainForm)
//   RBStemsPlus.exe --watch         the watcher, started at sign-in (HKCU Run): asks to reinstall
//                                   after a rekordbox update (Watcher.cs)
//   RBStemsPlus.exe --elevated ...  Stems Cache's changes as an administrator, started by the app
//                                   through UAC (Core/Elevated.cs); exits with an AdminExit code
//   RBStemsPlus.exe --probe         prints what it finds (rekordbox, ONNX Runtime, model, bridge)
namespace RBStemsPlus;

static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        var p = AppPaths.Current;
        if (args.Length > 0 && args[0] == "--elevated")
        {
            var log = new Log(p.AdminLog);
            log.Write($"elevated {string.Join(' ', args.Skip(1))} (RB Stems Plus {AppVersion.Current})");
            var r = Elevated.Run(args.Skip(1).ToArray(), p, log);
            log.Write($"elevated: exit {(int)r} {r}");
            return (int)r;
        }
        if (args.Length > 0 && args[0] == "--probe")
        {
            Console.Out.Write(Probe.Text(p));
            return 0;
        }
        Updater.CleanUpAfterUpdate();
        ApplicationConfiguration.Initialize();
        if (args.Length > 0 && args[0] == "--watch")
        {
            using var wm = new Mutex(true, Consts.WatcherMutex, out bool firstWatcher);
            if (!firstWatcher) return 0;
            LogErrors(new Log(p.WatcherLog), "watcher");
            Application.Run(new Watcher(p));
            return 0;
        }
        using var m = new Mutex(true, Consts.AppMutex, out bool first);
        if (!first)
        {
            // another copy is open: bring it forward (it watches for this file)
            Files.TryWriteText(Path.Combine(p.Support, "show"), "show\n");
            return 0;
        }
        var appLog = new Log(p.AppLog);
        LogErrors(appLog, "app");
        Application.Run(new MainForm(p));
        appLog.Write("app: closed");
        return 0;
    }

    /// Every error nothing else caught goes to the log with its stack, so a crash leaves a trace. An error in the
    /// window's own thread is logged and the app carries on (no .NET error dialog); one on another thread ends the
    /// process, as .NET has it, after its line is written. "closed" without a CRASH line before it is a normal quit;
    /// a log that just stops means the process was ended from outside.
    static void LogErrors(Log log, string who)
    {
        Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
        Application.ThreadException += (_, e) => log.Write($"{who}: ERROR in the window's thread (carried on): {e.Exception}");
        AppDomain.CurrentDomain.UnhandledException += (_, e) => log.Write($"{who}: CRASH (unhandled error{(e.IsTerminating ? ", the process ends" : "")}): {e.ExceptionObject}");
        TaskScheduler.UnobservedTaskException += (_, e) => { log.Write($"{who}: ERROR in a background task: {e.Exception}"); e.SetObserved(); };
        AppDomain.CurrentDomain.ProcessExit += (_, _) => log.Write($"{who}: process exit");
    }
}

/// What the app finds on this PC, for the log, the report and --probe.
static class Probe
{
    public static string Text(AppPaths p)
    {
        var sb = new System.Text.StringBuilder();
        sb.AppendLine($"RB Stems Plus {AppVersion.Current}{Consts.BuildNote}, Windows {Environment.OSVersion.Version}, {(Environment.Is64BitOperatingSystem ? "64-bit" : "32-bit")}");
        var all = Rekordbox.All();
        if (all.Count == 0) sb.AppendLine("rekordbox 7: not found");
        foreach (var rb in all)
        {
            var ort = Rekordbox.OrtVersion(rb.Ort);
            bool bridge = Rekordbox.StemsCachePresent(rb);
            sb.AppendLine($"rekordbox {rb.Version} in {rb.Dir}");
            sb.AppendLine($"  onnxruntime.dll: {(File.Exists(rb.Ort) ? (bridge ? "RB Stems Plus's bridge" : $"ONNX Runtime {ort ?? "?"}, {(Rekordbox.SignedByPioneer(rb.Ort) ? "signed by AlphaTheta" : "NOT signed by AlphaTheta")}") : "missing")}, sha256 {Files.Sha256(rb.Ort)?[..12] ?? "?"}");
        }
        var model = Path.Combine(p.ModelDir, "hdemucs.onnx");
        sb.AppendLine($"STEMS Engine: {(Rekordbox.EngineVersion(p.ModelDir) is { Length: > 0 } e ? e : "not downloaded")}; model {(File.Exists(model) ? $"{Files.Size(model)} bytes, sha256 {Files.Sha256(model)?[..12]}" : "missing")}");
        sb.AppendLine($"Pioneer's library copy: {(File.Exists(p.RootOrt) ? $"present ({Rekordbox.OrtVersion(p.RootOrt)})" : "none")}");
        var cfg = ConfigFile.Read(p.ConfigFile);
        sb.AppendLine($"config.ini: enabled={(cfg.Config.Enabled ? 1 : 0)} max_gb={ConfigFile.Number(cfg.Config.MaxGB)} max_days={ConfigFile.Number(cfg.Config.MaxDays)} rekordbox_model={(cfg.Config.RekordboxModel ? 1 : 0)} gpu={(cfg.Config.Gpu ? 1 : 0)} gpu_v4={(cfg.Config.GpuV4 ? 1 : 0)} location={cfg.Location switch { ConfigLocation.Chosen c => c.Dir, ConfigLocation.Malformed mm => "WRONG: " + mm.Why, _ => "this PC" }}{(cfg.Unreadable != null ? " UNREADABLE: " + cfg.Unreadable : "")}");
        var ch = State.ReadChosen(p);
        sb.AppendLine($"installed (chosen): model={(ch.Model ? 1 : 0)} cache={(ch.Cache ? 1 : 0)}; watcher {(WatcherRegistration.Registered() ? (WatcherRegistration.Disabled() ? "registered, turned OFF in Startup apps" : "registered") : "not registered")}");
        return sb.ToString();
    }
}
