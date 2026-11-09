// Running the prepare helper (rbsp-prepare.exe, helper/win/): the facts preparing depends on, a check that
// runs to its end, and a preparing run that can be paused and stopped. Prepare.swift's runPrepareHelper and
// PrepareRun, on Windows: Pause and Stop are named events the helper checks between chunks
// (Local\rbsp-prepare-<its pid>-pause: set while paused; -stop: set to stop after the chunk, the set written).
using System.Diagnostics;
using System.Text;

namespace RBStemsPlus;

static class PrepareEnv
{
    public static string HelperPath => Path.Combine(Path.GetDirectoryName(Environment.ProcessPath) ?? AppContext.BaseDirectory, PrepareDefaults.HelperName);

    /// The facts now (Prepare.swift's prepareFacts).
    public static PrepareFacts Facts(AppPaths p, Manifest? saved)
    {
        var f = new PrepareFacts();
        var rb = Rekordbox.Current();
        var list = saved?.Compat ?? Payload.BuiltIn;
        f.HelperThere = File.Exists(HelperPath);
        f.RekordboxInstalled = rb != null;
        f.RekordboxVersion = rb?.ShortVersion ?? "";
        f.RekordboxOK = rb != null && Compat.StemsCacheBlocked(p, list, rb) == null;
        f.CacheInstalled = Rekordbox.StemsCachePresent(rb);
        try { f.BridgeKeeps = rb != null && File.Exists(rb.Ort) && Rekordbox.BridgeSaysKept(File.ReadAllBytes(rb.Ort)); } catch (IOException) { f.BridgeKeeps = false; }
        f.OrtThere = File.Exists(p.RootOrt);
        var cfg = ConfigFile.Read(p.ConfigFile);
        f.ConfigUnreadable = cfg.Unreadable;
        f.Enabled = cfg.Config.Enabled;
        f.MaxGB = cfg.Config.MaxGB;
        f.PlusOn = Rekordbox.StemsPlusPresent(p, saved);
        f.Quality = Prepare.StemsQuality(p.RbSettingsFile);
        if (saved?.Model.Sha256 is { } m) f.OurModels.Add(m);
        switch (cfg.Location)
        {
            case ConfigLocation.Chosen c: f.CacheFolder = c.Dir; f.ChosenDrive = true; break;
            case ConfigLocation.Malformed: f.CacheFolder = null; break;
            default: f.CacheFolder = p.DefaultCacheDir; break;
        }
        return f;
    }

    /// The helper's arguments every run shares: the model rekordbox runs, the cache, its limit.
    public static List<string> BaseArgs(AppPaths p, PrepareFacts f)
    {
        var rb = Rekordbox.Current();
        var a = new List<string>
        {
            "--model", Path.Combine(p.ModelDir, "hdemucs.onnx"), "--cache", f.CacheFolder ?? (f.ChosenDrive ? "" : p.DefaultCacheDir),
            "--max-gb", ConfigFile.Number(f.MaxGB), "--kept-dir", p.KeptDir, "--ort", p.RootOrt, "--rekordbox", rb?.Dir ?? "",
            "--library", p.RbLibraryFile,
        };
        if (f.ChosenDrive) a.Add("--chosen");
        return a;
    }

    static ProcessStartInfo Start(IEnumerable<string> args)
    {
        var psi = new ProcessStartInfo(HelperPath)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (var a in args) psi.ArgumentList.Add(a);
        return psi;
    }

    /// Runs the helper to its end (an estimate, the playlists): its output and exit status. track gets each track's
    /// line as it comes (on a background thread); cancel can stop it. Any thread but the UI's.
    public static (int Status, string Out, string Err) RunToEnd(IEnumerable<string> args, TimeSpan? timeout = null, Action<Prepare.TrackLine>? track = null, CancellationToken cancel = default)
    {
        Process? proc;
        try { proc = Process.Start(Start(args)); }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or IOException) { return (-1, "", "the helper can't start: " + e.Message); }
        if (proc == null) return (-1, "", "the helper can't start");
        using var _ = proc;
        var o = new StringBuilder();
        var err = new StringBuilder();
        proc.ErrorDataReceived += (_, e) => { if (e.Data != null) lock (err) err.AppendLine(e.Data); };
        proc.OutputDataReceived += (_, e) =>
        {
            if (e.Data == null) return;
            lock (o) o.AppendLine(e.Data);
            if (track != null && Prepare.ParseRunLine(e.Data) is Prepare.RunLine.TrackDone t) track(t.Track);
        };
        proc.BeginOutputReadLine();
        proc.BeginErrorReadLine();
        using var reg = cancel.Register(() => { try { if (!proc.HasExited) proc.Kill(true); } catch (InvalidOperationException) { } });
        if (!proc.WaitForExit((int)(timeout ?? TimeSpan.FromMinutes(5)).TotalMilliseconds)) { try { proc.Kill(true); } catch (InvalidOperationException) { } }
        proc.WaitForExit();
        lock (o) lock (err) return (proc.ExitCode, o.ToString(), err.ToString().Trim());
    }

    /// A prepared playlist's check (args: an estimate of set name): the playlist as it is now in rekordbox's library, so a
    /// track added since it was prepared counts; the set's own files when the playlist can't be read.
    public static (int Status, string Out, string Err) CheckSet(string name, List<string> args, Func<List<string>, (int Status, string Out, string Err)> run)
    {
        var r = run(args.Concat(new[] { "--playlist", name }).ToList());
        return Prepare.ParseEstimate(r.Out) != null ? r : run(args);
    }

    /// The marker of the run being prepared (the set's name and the helper's pid), so the watcher doesn't check a set that is
    /// being prepared (Watcher.swift's setRunPattern, without reading other processes' command lines).
    static string MarkerFile(AppPaths p) => Path.Combine(p.Support, "preparing");
    public static void MarkRun(AppPaths p, string name, int pid) => Files.TryWriteText(MarkerFile(p), $"{pid}\n{name}\n");
    public static void UnmarkRun(AppPaths p) => Files.Remove(MarkerFile(p));

    /// The set a helper of this account is preparing now, or null.
    public static string? RunningSet(AppPaths p)
    {
        var t = Files.ReadText(MarkerFile(p))?.Split('\n');
        if (t == null || t.Length < 2 || !int.TryParse(t[0], out var pid)) return null;
        try { using var h = Process.GetProcessById(pid); return h.HasExited ? null : t[1]; }
        catch (ArgumentException) { return null; }
    }
}

/// A preparing run: the helper started on a playlist, its lines handed on as they come, ended once it has exited.
sealed class PrepareRun
{
    public readonly string Playlist;
    Process? proc;
    EventWaitHandle? pauseEv, stopEv;
    public bool Paused { get; private set; }
    public bool Running => proc is { HasExited: false };

    public PrepareRun(string playlist) { Playlist = playlist; }

    public bool Start(AppPaths ap, IEnumerable<string> args, Action<Prepare.RunLine> line, Action<int, string> ended)
    {
        try { proc = Process.Start(PrepareEnvStart(args)); }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or IOException) { return false; }
        if (proc == null) return false;
        var pid = proc.Id;
        PrepareEnv.MarkRun(ap, Playlist, pid);
        pauseEv = new EventWaitHandle(false, EventResetMode.ManualReset, $@"Local\rbsp-prepare-{pid}-pause");
        stopEv = new EventWaitHandle(false, EventResetMode.ManualReset, $@"Local\rbsp-prepare-{pid}-stop");
        var err = new StringBuilder();
        proc.ErrorDataReceived += (_, e) => { if (e.Data != null) lock (err) err.AppendLine(e.Data); };
        proc.OutputDataReceived += (_, e) => { if (e.Data != null && Prepare.ParseRunLine(e.Data) is { } r) line(r); };
        proc.EnableRaisingEvents = true;
        var p = proc;
        proc.Exited += (_, _) =>
        {
            p.WaitForExit();                                   // the output handlers have run to the end
            string e; lock (err) e = err.ToString().Trim();
            PrepareEnv.UnmarkRun(ap);
            ended(p.ExitCode, e);
            pauseEv?.Dispose(); stopEv?.Dispose();
        };
        proc.BeginOutputReadLine();
        proc.BeginErrorReadLine();
        return true;
    }

    static ProcessStartInfo PrepareEnvStart(IEnumerable<string> args)
    {
        var psi = new ProcessStartInfo(PrepareEnv.HelperPath)
        {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
        };
        foreach (var a in args) psi.ArgumentList.Add(a);
        return psi;
    }

    public void Pause() { if (!Running || Paused) return; try { pauseEv?.Set(); Paused = true; } catch (ObjectDisposedException) { } }
    public void Resume() { if (!Running || !Paused) return; try { pauseEv?.Reset(); Paused = false; } catch (ObjectDisposedException) { } }
    public void Stop()
    {
        if (!Running) return;
        try { stopEv?.Set(); if (Paused) { pauseEv?.Reset(); Paused = false; } } catch (ObjectDisposedException) { }
    }

    /// Stops the run and waits for it to write its set. True once no run is going.
    public bool StopAndWait(int seconds = 30)
    {
        if (!Running) return true;
        Stop();
        return proc!.WaitForExit(seconds * 1000);
    }
}
