// The window (app/Sources/Controller.swift on macOS): two lights (Demucs v4, Stems Cache), the
// buttons on the left, the log on the right, and the line under the buttons saying what can't be
// done and why. Actions run on a background thread; the buttons are off meanwhile.
using System.Diagnostics;
using System.Drawing;

namespace RBStemsPlus;

public sealed class MainForm : Form, IAsk
{
    readonly AppPaths p;
    readonly Log log;
    readonly Engine engine;
    readonly Light modelLight = new() { Text = "Demucs v4" }, cacheLight = new() { Text = "Stems Cache" };
    readonly Button installModel = Btn("Install Demucs v4"), installCache = Btn("Install Stems Cache"), reinstall = Btn("Reinstall"),
                    uninstallModel = Btn("Uninstall Demucs v4"), uninstallCache = Btn("Uninstall Stems Cache"), uninstallAll = Btn("Uninstall RB Stems Plus Completely"),
                    updateCache = Btn("Update Stems Cache");
    readonly Label statusLine = new() { AutoSize = false, Height = 64, Dock = DockStyle.Bottom, ForeColor = SystemColors.GrayText };
    readonly Label stepLine = new() { AutoSize = false, Height = 36, Dock = DockStyle.Bottom, Font = new Font(SystemFonts.MessageBoxFont ?? SystemFonts.DefaultFont, FontStyle.Bold), Visible = false };
    readonly ProgressBar progressBar = new() { Dock = DockStyle.Bottom, Height = 18, Visible = false, Minimum = 0, Maximum = 100, MarqueeAnimationSpeed = 30 };
    readonly TextBox logBox = new() { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill, Font = new Font(FontFamily.GenericMonospace, 8.5f), BackColor = SystemColors.Window };
    readonly ToolTip tips = new();
    readonly System.Windows.Forms.Timer refreshTimer = new() { Interval = 5000 };
    ToolStripMenuItem updateItem = null!;
    bool busy;
    Manifest? updateAvailable;

    static Button Btn(string text) => new() { Text = text, Width = 250, Height = 32, Margin = new Padding(0, 0, 0, 8), TextAlign = ContentAlignment.MiddleLeft };

    public MainForm(AppPaths p)
    {
        this.p = p;
        log = new Log(p.AppLog);
        log.Echo = line => { if (IsHandleCreated) BeginInvoke(() => AppendLog(line)); };
        engine = new Engine(p, log, s => BeginInvoke(() => statusLine.Text = s), this,
                            pct => BeginInvoke(() => ShowProgress(pct)), s => BeginInvoke(() => { stepLine.Text = s; stepLine.Visible = s.Length > 0; }));
        Text = "RB Stems Plus" + Consts.BuildNote;
        ClientSize = new Size(900, 520);
        MinimumSize = new Size(760, 440);
        StartPosition = FormStartPosition.CenterScreen;
        try { Icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath!); } catch { }

        var menu = new MenuStrip();
        var app = new ToolStripMenuItem("RB Stems Plus");
        app.DropDownItems.Add("Settings…", null, (_, _) => OpenSettings());
        updateItem = new ToolStripMenuItem("Update RB Stems Plus", null, (_, _) => RunUpdate()) { Visible = false };
        app.DropDownItems.Add(updateItem);
        app.DropDownItems.Add(new ToolStripSeparator());
        app.DropDownItems.Add("Quit", null, (_, _) => Close());
        var help = new ToolStripMenuItem("Help");
        help.DropDownItems.Add("RB Stems Plus Troubleshooting", null, (_, _) => Open(Consts.TroubleshootingUrl));
        help.DropDownItems.Add("Create Report…", null, (_, _) => CreateReport());
        help.DropDownItems.Add("Report an Issue", null, (_, _) => Open(Consts.IssuesPageUrl));
        help.DropDownItems.Add(new ToolStripSeparator());
        help.DropDownItems.Add("About RB Stems Plus", null, (_, _) => Tell("RB Stems Plus", $"RB Stems Plus {AppVersion.Current}{Consts.BuildNote}\nFree and open source (MIT).\n{Consts.HomeUrl}\n\nrekordbox is a trademark of AlphaTheta Corporation. RB Stems Plus is not made, endorsed or supported by AlphaTheta."));
        menu.Items.AddRange(new ToolStripItem[] { app, help });
        MainMenuStrip = menu;

        var lights = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 44, Padding = new Padding(12, 10, 12, 0) };
        lights.Controls.AddRange(new Control[] { modelLight, cacheLight });
        var buttons = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, Dock = DockStyle.Fill, Padding = new Padding(12, 8, 8, 0), WrapContents = false };
        buttons.Controls.AddRange(new Control[] { installModel, installCache, updateCache, reinstall, uninstallModel, uninstallCache, uninstallAll });
        var left = new Panel { Dock = DockStyle.Left, Width = 280 };
        left.Controls.Add(buttons);
        left.Controls.Add(stepLine);
        left.Controls.Add(progressBar);
        left.Controls.Add(statusLine);
        statusLine.Padding = new Padding(12, 0, 8, 8);
        stepLine.Padding = new Padding(12, 6, 8, 0);
        progressBar.Margin = new Padding(12, 0, 12, 0);
        var right = new Panel { Dock = DockStyle.Fill, Padding = new Padding(4, 8, 12, 12) };
        right.Controls.Add(logBox);
        Controls.Add(right);
        Controls.Add(left);
        Controls.Add(lights);
        Controls.Add(menu);

        installModel.Click += (_, _) => Run("install-model", "Install Demucs v4", () =>
            Confirm("Install Demucs v4?", "Replaces rekordbox's stems model with Demucs v4. rekordbox's own model is kept, so it can be put back.", "Install") && engine.InstallModel());
        installCache.Click += (_, _) => Run("install-cache", "Install Stems Cache", () =>
            Confirm("Install Stems Cache?", "Saves the stems rekordbox separates, with either stems model, so they load much faster the next time.\n\nIt replaces one file in rekordbox's folder (Pioneer's is kept), so Windows asks for an administrator's permission.", "Install") && engine.InstallCache());
        reinstall.Click += (_, _) => Run("reinstall", "Reinstall", engine.ReinstallMissing);
        uninstallModel.Click += (_, _) => Run("uninstall-model", "Uninstall Demucs v4", () =>
            Confirm("Uninstall Demucs v4?", "Puts rekordbox's own stems model back.", "Uninstall") && engine.UninstallModel());
        uninstallCache.Click += (_, _) => Run("uninstall-cache", "Uninstall Stems Cache", () =>
            Confirm("Uninstall Stems Cache?", "Stops saving stems and puts rekordbox's original files back. The stems already saved are kept.", "Uninstall") && engine.UninstallCache());
        uninstallAll.Click += (_, _) => UninstallEverything();
        updateCache.Click += (_, _) => Run("update-cache", "Update Stems Cache", () =>
            Confirm("Update Stems Cache?", "A newer Stems Cache is available. Updating it replaces the one in rekordbox's folder (Windows asks for permission). The stems already saved are kept.", "Update")
            && engine.InstallCache());
        refreshTimer.Tick += (_, _) => { if (!busy) Refresh2(); CheckShowRequest(); };
        Shown += (_, _) => OnFirstShown();
        FormClosing += (s, e) => { if (busy) { e.Cancel = true; Tell("RB Stems Plus is busy", "Wait until the change is done."); } };
    }

    /// The progress bar: a percent, null for "working" (a moving bar), Engine.Hidden to hide it.
    void ShowProgress(int? pct)
    {
        if (pct == Engine.Hidden) { progressBar.Visible = false; return; }
        progressBar.Visible = true;
        if (pct is { } v)
        {
            if (progressBar.Style != ProgressBarStyle.Continuous) progressBar.Style = ProgressBarStyle.Continuous;
            progressBar.Value = Math.Clamp(v, 0, 100);
        }
        else if (progressBar.Style != ProgressBarStyle.Marquee) progressBar.Style = ProgressBarStyle.Marquee;
    }

    void AppendLog(string line)
    {
        logBox.AppendText((logBox.TextLength > 0 ? Environment.NewLine : "") + line);
    }

    void OnFirstShown()
    {
        foreach (var line in Probe.Text(p).TrimEnd().Split('\n')) log.Write(line.TrimEnd('\r'));
        if (!File.Exists(p.WelcomedFile))
        {
            Tell("Welcome to RB Stems Plus", "Demucs v4 swaps rekordbox's stems model for Meta's Demucs v4. Stems Cache saves the stems rekordbox separates, so the next load is much faster.\n\nInstall either one, or both. Quit rekordbox first.");
            Files.TryWriteText(p.WelcomedFile, "1\n");
        }
        Refresh2();
        refreshTimer.Start();
        if (State.Leftover(p) is { } left) log.Write($"an earlier run quit during \"{left.Label}\": check the lights, and run it again if needed");
        if (State.TakeReinstallRequest(p) is { } age && age < TimeSpan.FromHours(1)) reinstall.PerformClick();
        Task.Run(() =>
        {
            var m = Updater.Check(p, log);
            if (m != null) BeginInvoke(() => { updateAvailable = m; updateItem.Visible = true; Refresh2(); });
        });
    }

    void CheckShowRequest()
    {
        var f = Path.Combine(p.Support, "show");
        if (File.Exists(f)) { Files.Remove(f); if (WindowState == FormWindowState.Minimized) WindowState = FormWindowState.Normal; Activate(); }
        if (!busy && File.Exists(p.RequestFile) && State.TakeReinstallRequest(p) is not null) reinstall.PerformClick();
    }

    /// The lights, the buttons and the status line from what is on this PC now.
    void Refresh2()
    {
        var rb = Rekordbox.Current();
        var saved = engine.Saved;
        var list = saved?.Compat ?? Payload.BuiltIn;
        bool model = Rekordbox.StemsPlusPresent(p, saved), cache = Rekordbox.StemsCachePresent(rb);
        var chosen = State.ReadChosen(p);
        var modelWhy = Compat.StemsPlusBlocked(p, list, rb);
        var cacheWhy = cache ? null : Compat.StemsCacheBlocked(p, list, rb);
        modelLight.State = model ? LightState.On : LightState.Off;
        tips.SetToolTip(modelLight, model ? "Demucs v4 is on." : "Demucs v4 is off.");
        var cfg = ConfigFile.Read(p.ConfigFile);
        string? cacheTip = null;
        var lightState = cache ? LightState.On : LightState.Off;
        if (cache)
        {
            var folder = cfg.Location is ConfigLocation.Chosen c ? c.Dir : p.DefaultCacheDir;
            var (free, disk) = State.Space(folder);
            if (cfg.Unreadable != null || cfg.Location is ConfigLocation.Malformed) { lightState = LightState.Paused; cacheTip = "Stems Cache saves nothing: config.ini can't be read or sets the location wrongly (Settings fixes it)."; }
            else if (!cfg.Config.Enabled) { lightState = LightState.Paused; cacheTip = "Stems Cache is turned off in config.ini (enabled=0)."; }
            else if (State.PausedFloor(free, disk) is { } floor) { lightState = LightState.Paused; cacheTip = $"Stems Cache stopped saving stems: the disk has less than {State.FloorGB(floor)} GB free."; }
            else if (State.LiveBridgeState(p, folder) is { State: "unavailable" } bs) { lightState = LightState.Paused; cacheTip = $"Stems Cache isn't saving stems: {State.ReasonText(bs.Reason)}."; }
            else if (Rekordbox.EngineVersion(p.ModelDir).Length == 0) { lightState = LightState.Paused; cacheTip = "Stems Cache starts saving stems once rekordbox has downloaded its STEMS Engine."; }
        }
        cacheLight.State = lightState;
        tips.SetToolTip(cacheLight, cacheTip ?? (cache ? "Stems Cache is on." : "Stems Cache is off."));
        bool missing = (chosen.Model && !model) || (chosen.Cache && !cache);
        installModel.Visible = !model; uninstallModel.Visible = model;
        installCache.Visible = !cache; uninstallCache.Visible = cache || Directory.Exists(p.RootDir);
        bool cacheUpdate = cache && saved != null && rb != null && InstalledBridgeSha(rb) is { } have && have != saved.Bridge.Sha256;
        updateCache.Visible = cacheUpdate;
        updateCache.Enabled = !busy;
        reinstall.Visible = missing;
        installModel.Enabled = !busy && modelWhy == null;
        installCache.Enabled = !busy && cacheWhy == null;
        foreach (var b in new[] { reinstall, uninstallModel, uninstallCache, uninstallAll }) b.Enabled = !busy;
        var lines = new List<string>();
        if (!busy)
        {
            if (!model && modelWhy != null) lines.Add(modelWhy);
            if (!cache && cacheWhy != null && cacheWhy != modelWhy) lines.Add(cacheWhy);
            if (missing) lines.Add("rekordbox was updated: click Reinstall to turn back on what went missing.");
            if (chosen.Cache && cache && WatcherRegistration.Registered() && WatcherRegistration.Disabled())
                lines.Add("Reinstall reminders are off. Turn on RB Stems Plus Watcher in Settings > Apps > Startup.");
            if (cacheUpdate) lines.Add("A newer Stems Cache is available: click Update Stems Cache.");
            if (updateAvailable != null) lines.Add($"RB Stems Plus {updateAvailable.AppInfo.Version} is available (RB Stems Plus > Update RB Stems Plus).");
            statusLine.Text = string.Join("\n", lines);
        }
    }

    /// The SHA-256 of the bridge in rekordbox's folder, remembered per size and modification time.
    string? bridgeShaKey, bridgeSha;
    string? InstalledBridgeSha(RekordboxInstall rb)
    {
        try
        {
            var fi = new FileInfo(rb.Ort);
            var key = $"{fi.FullName}|{fi.Length}|{fi.LastWriteTimeUtc.Ticks}";
            if (key != bridgeShaKey) { bridgeSha = Files.Sha256(rb.Ort); bridgeShaKey = key; }
            return bridgeSha;
        }
        catch { return null; }
    }

    void Run(string id, string label, Func<bool> work)
    {
        if (busy) return;
        busy = true;
        Refresh2();
        State.SetBusy(p, new BusyAction(id, label));
        log.Write($"--- {label}");
        Task.Run(() =>
        {
            bool ok = false;
            try { ok = work(); }
            catch (Exception e) { log.Write($"{label}: FAILED {e}"); BeginInvoke(() => Tell(label, "Something went wrong. See the log in this window.")); }
            finally
            {
                State.SetBusy(p, null);
                engine.EndSteps();
                BeginInvoke(() => { busy = false; statusLine.Text = ""; Refresh2(); log.Write($"--- {label}: {(ok ? "done" : "not done")}"); });
            }
        });
    }

    void UninstallEverything()
    {
        var folder = ConfigFile.Read(p.ConfigFile).Location is ConfigLocation.Chosen c ? c.Dir : null;
        var stems = CacheLocation.EntriesSize(p.DefaultCacheDir) is { } n ? CacheLocation.AboutSize(n) : "";
        var text = "rekordbox goes back to how it was, and RB Stems Plus is deleted, including its settings, its logs and the stems it saved on this PC" + stems + "."
                   + (folder != null ? $"\n\nThe stems saved in {folder} stay on that drive." : "") + "\n\nYour music and your rekordbox library aren't touched.";
        if (!Confirm("Uninstall RB Stems Plus?", text, "Uninstall")) return;
        Run("uninstall-all", "Uninstall RB Stems Plus Completely", () =>
        {
            if (!engine.UninstallAll()) return false;
            BeginInvoke(() =>
            {
                Tell("RB Stems Plus is uninstalled", "rekordbox is back to how it was.");
                Updater.DeleteSelfAndQuit(p);
            });
            return true;
        });
    }

    void OpenSettings()
    {
        using var f = new SettingsForm(p, log);
        f.ShowDialog(this);
        Refresh2();
    }

    void RunUpdate()
    {
        if (updateAvailable is not { } m) return;
        Run("update", "Update RB Stems Plus", () =>
        {
            var exe = Updater.Download(p, m, log, s => BeginInvoke(() => statusLine.Text = s), pct => BeginInvoke(() => ShowProgress(pct)));
            if (exe == null) { BeginInvoke(() => Tell("Update failed", "The update couldn't be downloaded or didn't verify. RB Stems Plus stays as it is.")); return false; }
            BeginInvoke(() => Updater.SwapAndRestart(exe, log));
            return true;
        });
    }

    void CreateReport()
    {
        var zip = Report.Create(p, log);
        if (zip == null) { Tell("Report", "The report couldn't be made. See the log in this window."); return; }
        if (Confirm("Report saved", $"The report is on your Desktop:\n{zip}\n\nOpen a new GitHub issue now? Drag the zip into it.", "Open GitHub Issue", "Close"))
        {
            Open(Report.IssueUrl(p));
            try { Process.Start("explorer.exe", $"/select,\"{zip}\""); } catch { }
        }
    }

    static void Open(string url) { try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); } catch { } }

    // ---------------------------------------------------------------- IAsk (called from any thread)

    T OnUi<T>(Func<T> f) => InvokeRequired ? (T)Invoke(f) : f();

    public bool Confirm(string title, string text, string ok, string cancel = "Cancel") => OnUi(() =>
    {
        var okB = new TaskDialogButton(ok);
        var page = new TaskDialogPage { Caption = "RB Stems Plus", Heading = title, Text = text, Buttons = { okB, new TaskDialogButton(cancel) } };
        return TaskDialog.ShowDialog(this, page) == okB;
    });

    public void Tell(string title, string text) => OnUi(() =>
    {
        TaskDialog.ShowDialog(this, new TaskDialogPage { Caption = "RB Stems Plus", Heading = title, Text = text, Buttons = { TaskDialogButton.OK } });
        return true;
    });

    public bool RetryWhileRekordboxOpen(string what) => OnUi(() =>
    {
        var retry = new TaskDialogButton("Try Again");
        var page = new TaskDialogPage
        {
            Caption = "RB Stems Plus", Heading = "Quit rekordbox first",
            Text = Rekordbox.PioneerUpdating() ? "rekordbox's installer is running. Wait until it's done, then click Try Again."
                                                : "Quit rekordbox (also in other accounts on this PC), then click Try Again.",
            Buttons = { retry, TaskDialogButton.Cancel },
        };
        return TaskDialog.ShowDialog(this, page) == retry;
    });
}

public enum LightState { Off, On, Paused }

/// A status light: a green check, a red cross or a yellow dot, and its name.
sealed class Light : Control
{
    LightState state;
    public LightState State { get => state; set { state = value; Invalidate(); } }
    public Light() { Width = 170; Height = 26; DoubleBuffered = true; Margin = new Padding(0, 0, 16, 0); }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        var color = state switch { LightState.On => Color.FromArgb(52, 168, 83), LightState.Paused => Color.FromArgb(240, 180, 0), _ => Color.FromArgb(220, 60, 50) };
        int d = Height - 6;
        using (var b = new SolidBrush(color)) g.FillEllipse(b, 2, 3, d, d);
        using (var pen = new Pen(Color.White, 2.2f))
        {
            if (state == LightState.On) g.DrawLines(pen, new[] { new PointF(7, 3 + d / 2f), new PointF(2 + d / 2.4f, 3 + d * 0.72f), new PointF(d - 3, 7) });
            else if (state == LightState.Off) { g.DrawLine(pen, 8, 9, d - 4, d - 3); g.DrawLine(pen, d - 4, 9, 8, d - 3); }
        }
        TextRenderer.DrawText(g, Text, Font, new Point(d + 8, (Height - Font.Height) / 2), ForeColor);
    }
}
