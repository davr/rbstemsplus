// The Prepare tab (app/Sources/PrepareView.swift): rekordbox's playlists on the left (read only, by the helper);
// on the right what preparing the chosen one would take, the tracks and their state, a keep-until date, and
// Prepare, Check Again, Pause and Stop. The helper (PrepareRunner.cs) does the work in the background.
using System.Drawing;

namespace RBStemsPlus;

sealed class PrepareTab : UserControl
{
    readonly AppPaths p;
    readonly Log log;
    readonly Func<Manifest?> saved;
    readonly Func<bool> otherActionRunning;
    readonly Label problems = new() { Dock = DockStyle.Top, AutoSize = false, Height = Ui.S(0), ForeColor = Color.Firebrick, Padding = new Padding(Ui.S(8), Ui.S(6), Ui.S(8), Ui.S(0)) };
    readonly TreeView tree = new() { Dock = DockStyle.Left, Width = Ui.S(250), HideSelection = false };
    readonly Label nameLabel = new() { Text = "Choose a playlist", AutoSize = false, Height = Ui.S(28), Dock = DockStyle.Top, Font = new Font(SystemFonts.MessageBoxFont ?? SystemFonts.DefaultFont, FontStyle.Bold) };
    readonly Label info = new() { AutoSize = false, Height = Ui.S(62), Dock = DockStyle.Top };
    readonly ListView rows = new() { Dock = DockStyle.Fill, View = View.Details, FullRowSelect = true, HideSelection = false, MultiSelect = false };
    readonly DateTimePicker until = new() { Format = DateTimePickerFormat.Short, Width = Ui.S(110) };
    readonly Button prepareBtn = new() { Text = "Prepare", Width = Ui.S(90), Height = Ui.S(28) }, checkBtn = new() { Text = "Check Again", Width = Ui.S(100), Height = Ui.S(28) },
                    pauseBtn = new() { Text = "Pause", Width = Ui.S(80), Height = Ui.S(28) }, stopBtn = new() { Text = "Stop", Width = Ui.S(80), Height = Ui.S(28) };
    readonly Label runLine = new() { AutoSize = false, Height = Ui.S(22), Dock = DockStyle.Bottom, ForeColor = SystemColors.GrayText };
    readonly ProgressBar bar = new() { Dock = DockStyle.Bottom, Height = Ui.S(14), Minimum = 0, Maximum = 1000, Visible = false };

    List<Prepare.Playlist> playlists = new();
    Prepare.Playlist? selected;
    List<Prepare.TrackLine> tracks = new();
    Prepare.Estimate? estimate;
    PrepareFacts facts = new();
    PrepareRun? run;
    List<Prepare.TrackLine>? runTracks;
    CancellationTokenSource? checkCts;
    bool checking, listLoaded, listLoading;
    string runSummary = "";
    string? lastRunLine;

    /// The set being prepared now, or null.
    public string? RunningSet => run is { Running: true } ? run.Playlist : null;
    public bool IsRunning => run is { Running: true };
    /// Raised when the kept sets may have changed (a run ended, a check finished): the Status tab reloads.
    public event Action? SetsChanged;

    public PrepareTab(AppPaths p, Log log, Func<Manifest?> saved, Func<bool> otherActionRunning)
    {
        this.p = p; this.log = log; this.saved = saved; this.otherActionRunning = otherActionRunning;
        _ = Handle;                                            // made now, on the window's thread: background results are posted to it
        rows.Columns.Add("Track", Ui.S(330)); rows.Columns.Add("Result", Ui.S(90)); rows.Columns.Add("Note", Ui.S(260));
        until.Value = DateTime.Today.AddDays(PrepareDefaults.KeepDaysDefault);
        until.MinDate = DateTime.Today.AddDays(1);
        until.MaxDate = DateTime.Today.AddDays(365);
        var buttons = new FlowLayoutPanel { Dock = DockStyle.Top, Height = Ui.S(40), Padding = new Padding(Ui.S(0), Ui.S(4), Ui.S(0), Ui.S(0)), WrapContents = false };
        buttons.Controls.AddRange(new Control[] { new Label { Text = "Keep until", AutoSize = true, Margin = new Padding(Ui.S(0), Ui.S(8), Ui.S(4), Ui.S(0)) }, until, prepareBtn, checkBtn, pauseBtn, stopBtn });
        var right = new Panel { Dock = DockStyle.Fill, Padding = new Padding(Ui.S(12), Ui.S(6), Ui.S(8), Ui.S(6)) };
        right.Controls.Add(rows);
        right.Controls.Add(runLine);
        right.Controls.Add(bar);
        right.Controls.Add(buttons);
        right.Controls.Add(info);
        right.Controls.Add(nameLabel);
        Controls.Add(right);
        Controls.Add(tree);
        Controls.Add(problems);
        tree.AfterSelect += (_, e) => Choose(e.Node?.Tag as Prepare.Playlist);
        prepareBtn.Click += (_, _) => PrepareClicked(null);
        checkBtn.Click += (_, _) => Check();
        pauseBtn.Click += (_, _) => PauseClicked();
        stopBtn.Click += (_, _) => { run?.Stop(); runSummary = "Stopping…"; log.Write("prepare: stop asked"); UpdateUi(); };
        UpdateUi();
    }

    /// Whether this tab is what the user is looking at (a check reads the playlists' tracks).
    public void Shown()
    {
        facts = PrepareEnv.Facts(p, saved());
        ShowProblems();
        if (facts.HelperThere && Prepare.Problems(facts).Count == 0 && !listLoaded && !listLoading) LoadPlaylists();
        else if (selected != null && !checking && !IsRunning) Check();
    }

    void Post(Action a) => Ui.Post(this, log, "prepare", a);

    /// Reads the playlists if they haven't been read (the Status tab names the prepared playlists by them).
    public void LoadNames()
    {
        if (listLoaded || listLoading) return;
        facts = PrepareEnv.Facts(p, saved());
        if (facts.HelperThere && Prepare.Problems(facts).Count == 0) LoadPlaylists();
    }

    /// Work off the window's thread. An error in it is logged with its stack, then failed runs on the window's thread.
    void Background(string what, Action failed, Action work) => Task.Run(() =>
    {
        try { work(); }
        catch (Exception e) { log.Write($"prepare: ERROR while {what}: {e}"); Post(failed); }
    });

    void ShowProblems()
    {
        var list = Prepare.Problems(facts);
        problems.Text = string.Join("\n", list);
        problems.Height = list.Count == 0 ? 0 : 20 + 16 * list.Sum(l => 1 + l.Length / 70);
        tree.Enabled = list.Count == 0;
        UpdateUi();
    }

    void LoadPlaylists()
    {
        listLoading = true;
        var args = new List<string> { "--playlists", "--library", p.RbLibraryFile, "--rekordbox", PrepareEnv.RbDir() };
        Background("reading the playlists", () => { listLoading = false; }, () =>
        {
            var r = PrepareEnv.RunToEnd(args, TimeSpan.FromSeconds(60));
            var list = Prepare.ParsePlaylists(r.Out);
            Post(() =>
            {
                listLoading = false;
                if (list.Count == 0 && r.Status != 0) { problems.Text = "rekordbox's playlists can't be read" + (r.Err.Length > 0 ? ": " + Prepare.NoPaths(r.Err) : "."); problems.Height = Ui.S(40); log.Write("prepare: playlists: " + Prepare.NoPaths(r.Err)); return; }
                listLoaded = true;
                playlists = list;
                DrawTree();
                SetsChanged?.Invoke();                         // the Status tab shows the sets by these names
            });
        });
    }

    void DrawTree()
    {
        tree.BeginUpdate();
        tree.Nodes.Clear();
        void Add(TreeNodeCollection into, IEnumerable<Prepare.PNode> nodes)
        {
            foreach (var n in nodes)
            {
                var t = new TreeNode(n.P.CanPrepare ? $"{n.P.Name} ({n.P.Tracks})" : n.P.Name) { Tag = n.P, ForeColor = n.P.CanPrepare || n.Children.Count > 0 ? SystemColors.WindowText : SystemColors.GrayText };
                into.Add(t);
                Add(t.Nodes, n.Children);
            }
        }
        Add(tree.Nodes, Prepare.PlaylistTree(playlists));
        tree.ExpandAll();
        tree.EndUpdate();
        UpdateUi();
    }

    /// A playlist's name from its id (the Status tab lists sets by id).
    public string NameOf(string id) => playlists.FirstOrDefault(x => x.Id == id)?.Name ?? id;

    /// Chooses a set from elsewhere (the Status tab's Prepare Again).
    public void Select(string playlistId)
    {
        TreeNode? Find(TreeNodeCollection c)
        {
            foreach (TreeNode n in c)
            {
                if (n.Tag is Prepare.Playlist pl && pl.Id == playlistId) return n;
                if (Find(n.Nodes) is { } f) return f;
            }
            return null;
        }
        if (Find(tree.Nodes) is { } hit) tree.SelectedNode = hit;
    }

    void Choose(Prepare.Playlist? pl)
    {
        CancelCheck();
        selected = pl;
        estimate = null;
        nameLabel.Text = pl?.Name ?? "Choose a playlist";
        if (IsRunning && pl != null && pl.Id == run!.Playlist && runTracks != null)
        {
            // back on the playlist being prepared: its rows as the run has them so far
            tracks = runTracks;
            FillRows();
            info.Text = "Being prepared now.";
            UpdateUi();
            return;
        }
        tracks = new();                                        // a list of its own: the run keeps runTracks
        rows.Items.Clear();
        info.Text = pl == null ? "" : !pl.CanPrepare ? "Only playlists can be prepared, not folders or intelligent playlists."
                  : IsRunning ? "Another playlist is being prepared. This one can be checked once that is done." : "Checking…";
        if (pl is { CanPrepare: true }) Check();
        UpdateUi();
    }

    void CancelCheck()
    {
        try { checkCts?.Cancel(); } catch (Exception e) { log.Write("prepare: ERROR while stopping a check: " + e); }
        checkCts = null;
        checking = false;
    }

    /// The tracks as the library lists them, then the estimate over them.
    void Check()
    {
        if (selected is not { CanPrepare: true } pl || IsRunning) return;
        CancelCheck();
        var cts = checkCts = new CancellationTokenSource();
        checking = true;
        estimate = null;
        info.Text = "Checking…";
        UpdateUi();
        var baseArgs = PrepareEnv.BaseArgs(p, facts);
        Background("checking a playlist", () => { if (cts.IsCancellationRequested) return; checking = false; info.Text = "The check failed (the log on the Manage tab says why)."; UpdateUi(); }, () =>
        {
            var list = Prepare.ParseTrackList(PrepareEnv.RunToEnd(new[] { "--tracks", pl.Id, "--library", p.RbLibraryFile, "--rekordbox", PrepareEnv.RbDir() }, TimeSpan.FromSeconds(60), cancel: cts.Token).Out);
            if (cts.IsCancellationRequested) return;
            Post(() => { if (cts.IsCancellationRequested) return; tracks = list; FillRows(); });
            var hint = 0;
            var r = PrepareEnv.RunToEnd(baseArgs.Concat(new[] { "--estimate", "--keep", pl.Id, "--playlist", pl.Id }), TimeSpan.FromMinutes(10),
                t => Post(() => { if (cts.IsCancellationRequested) return; hint = Prepare.Place(t, tracks, hint) + 1; UpdateRow(hint - 1); }), cts.Token);
            if (cts.IsCancellationRequested) return;
            var e = Prepare.ParseEstimate(r.Out);
            Post(() =>
            {
                if (cts.IsCancellationRequested) return;
                checking = false;
                estimate = e;
                if (e == null) { info.Text = "The check failed" + (r.Err.Length > 0 ? ": " + Prepare.NoPaths(r.Err) : "."); log.Write("prepare: check failed: " + Prepare.NoPaths(r.Err)); }
                else if (e.Refused != null) info.Text = e.Refused;
                else info.Text = Prepare.EstimateSummary(e) + (lastRunLine != null ? "\n" + lastRunLine : "");
                UpdateUi();
            });
        });
    }

    void FillRows()
    {
        rows.BeginUpdate();
        rows.Items.Clear();
        foreach (var t in tracks) rows.Items.Add(RowFor(t));
        rows.EndUpdate();
    }

    ListViewItem RowFor(Prepare.TrackLine t)
    {
        var note = t.Result == "skipped" ? t.Why : t.Result == "prepared" ? Prepare.PreparedNote(t) : t.Result == "prepare" && t.Missing > 0 ? "some stems were removed" : "";
        return new ListViewItem(new[] { t.Name, Prepare.ResultText(t, checking), note });
    }

    void UpdateRow(int i)
    {
        if (i < 0 || i >= tracks.Count) return;
        var item = RowFor(tracks[i]);
        if (i < rows.Items.Count) rows.Items[i] = item; else rows.Items.Add(item);
        if (IsRunning && i < rows.Items.Count) rows.EnsureVisible(i);
    }

    // ---------------------------------------------------------------- preparing

    void PrepareClicked(int? first)
    {
        if (selected is not { CanPrepare: true } pl || estimate is not { Refused: null } e || IsRunning) return;
        if (e.PrepareCount == 0 && first == null) { runSummary = "Everything in this playlist is ready."; UpdateUi(); return; }
        if (!e.Fits && first == null)
        {
            var d = Prepare.DoesntFitChoices(e, 1e9);
            var buttons = d.Buttons.Select(b => new TaskDialogButton(b)).ToList();
            var page = new TaskDialogPage { Caption = "RB Stems Plus", Heading = "This playlist doesn't fit", Text = d.Message };
            foreach (var b in buttons) page.Buttons.Add(b);
            var chosen = TaskDialog.ShowDialog(FindForm()!, page);
            var idx = buttons.IndexOf(chosen);
            if (idx < 0 || d.Buttons[idx] == "Cancel") return;
            if (d.Buttons[idx].StartsWith("Prepare the First")) { PrepareClicked(e.FitFiles); return; }
            if (d.RaiseTo is { } to && ConfigFile.WriteLimits(p.ConfigFile, to, ConfigFile.Read(p.ConfigFile).Config.MaxDays))
            {
                log.Write($"prepare: max_gb raised to {ConfigFile.Number(to)}");
                facts = PrepareEnv.Facts(p, saved());
                Check();
            }
            return;
        }
        var date = until.Value.Date.AddDays(1) > DateTime.Today ? until.Value.Date : DateTime.Today.AddDays(1);
        var untilSeconds = new DateTimeOffset(date.AddHours(23).AddMinutes(59), TimeZoneInfo.Local.GetUtcOffset(date)).ToUnixTimeSeconds();
        var args = PrepareEnv.BaseArgs(p, facts).Concat(Prepare.RunArgs(pl.Id, untilSeconds.ToString(System.Globalization.CultureInfo.InvariantCulture), first)).ToList();
        int finished = 0, skipped = 0;
        string? stopped = null, error = null;
        bool gotDone = false;
        double keptGB = 0;
        var r = new PrepareRun(pl.Id);
        run = r;
        var mine = runTracks = tracks;                         // the run's rows, whatever playlist the tab shows meanwhile
        runSummary = "Starting…";
        lastRunLine = null;
        bar.Visible = true; bar.Value = 0;
        var hint = 0;
        log.Write($"prepare: playlist {pl.Id}: started{(first != null ? $" (the first {first} tracks)" : "")}");
        bool started = r.Start(p, log, args, line => Post(() =>
        {
            switch (line)
            {
                case Prepare.RunLine.Progress g:
                    runSummary = $"Track {g.File + 1} of {g.Files}, part {g.Chunk} of {g.Chunks}";
                    if (g.Files > 0 && g.Chunks > 0) bar.Value = Math.Clamp((int)(1000.0 * (g.File + (double)g.Chunk / g.Chunks) / g.Files), 0, 1000);
                    break;
                case Prepare.RunLine.TrackDone t:
                    hint = Prepare.Place(t.Track, mine, hint) + 1;
                    if (ReferenceEquals(tracks, mine)) UpdateRow(hint - 1);
                    if (t.Track.Result == "skipped") skipped++; else if (t.Track.Result == "prepared") finished++;
                    break;
                case Prepare.RunLine.Room m:
                    log.Write($"prepare: made room: {m.Removed} entries removed (cache {m.CacheGB:0.0} GB)");
                    break;
                case Prepare.RunLine.Done d:
                    gotDone = true; stopped = d.Stopped; error = d.Error; keptGB = d.KeptGB;
                    break;
            }
            UpdateUi();
        }), (status, err) => Post(() =>
        {
            run = null;
            runTracks = null;
            bar.Visible = false;
            if (status != 0 && err.Length > 0) { runSummary = err; log.Write("prepare: the helper stopped: " + Prepare.NoPaths(err)); }
            else if (!gotDone) { runSummary = "The preparing stopped unexpectedly. Click Check Again, then Prepare."; log.Write($"prepare: the helper ended without its last line (status {status})"); }
            else
            {
                lastRunLine = Prepare.LastRunLine(finished, skipped, stopped, error);
                runSummary = $"{finished} {(finished == 1 ? "track" : "tracks")} done" + (skipped > 0 ? $", {skipped} skipped" : "") + $". Prepared playlists use {Prepare.SizeText(keptGB)}."
                             + (stopped != null ? (stopped == "stopped" ? " Stopped: the tracks not done yet aren't prepared." : $" Stopped: {stopped}.") : "") + (error != null ? " " + error : "");
                log.Write($"prepare: playlist {pl.Id}: {Prepare.NoPaths(runSummary)}");
            }
            Toast.Show(Prepare.RunEndTitle(pl.Name, status, gotDone, stopped, finished, skipped, error, first != null), runSummary);
            SetsChanged?.Invoke();
            UpdateUi();
            Check();
        }));
        if (!started) { run = null; runTracks = null; bar.Visible = false; runSummary = "The helper can't start."; log.Write("prepare: the helper can't start"); }
        UpdateUi();
    }

    void PauseClicked()
    {
        if (run == null) return;
        if (run.Paused) { run.Resume(); log.Write("prepare: resumed"); } else { run.Pause(); log.Write("prepare: paused"); }
        UpdateUi();
    }

    /// Stops a run and waits for it to write its set (Quit, actions that change rekordbox). True once no run is going.
    public bool StopAndWait(int seconds = 30) => run == null || run.StopAndWait(seconds);

    void UpdateUi()
    {
        bool ok = Prepare.Problems(facts).Count == 0 && !otherActionRunning();
        bool running = IsRunning;
        prepareBtn.Enabled = ok && !running && !checking && selected is { CanPrepare: true } && estimate is { Refused: null };
        checkBtn.Enabled = ok && !running && !checking && selected is { CanPrepare: true };
        pauseBtn.Enabled = running; pauseBtn.Text = run is { Paused: true } ? "Resume" : "Pause";
        stopBtn.Enabled = running;
        until.Enabled = !running;
        runLine.Text = running ? (run!.Paused ? "Paused. " : "") + runSummary : runSummary;
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) { checkCts?.Cancel(); }
        base.Dispose(disposing);
    }
}

/// A Windows notification (a tray balloon), for a run that ended and for the watcher's check: never a dialog over rekordbox.
static class Toast
{
    // One tray icon for the process, made at the first notification and shown only while a notification is up.
    // (An icon made and disposed for each notification was used after its disposal by its own timer.)
    static NotifyIcon? icon;
    static System.Windows.Forms.Timer? hide;

    public static void Show(string title, string body)
    {
        try
        {
            if (icon == null)
            {
                icon = new NotifyIcon { Icon = SystemIcons.Information, Text = "RB Stems Plus" };
                try { icon.Icon = Icon.ExtractAssociatedIcon(Environment.ProcessPath!) ?? SystemIcons.Information; } catch (Exception) { }
                icon.BalloonTipClosed += (_, _) => Hide();
                icon.BalloonTipClicked += (_, _) => Hide();
                hide = new System.Windows.Forms.Timer { Interval = 20000 };
                hide.Tick += (_, _) => Hide();
                Application.ApplicationExit += (_, _) => { try { hide?.Dispose(); icon?.Dispose(); } catch (Exception) { } hide = null; icon = null; };
            }
            icon.Visible = true;
            icon.BalloonTipTitle = title.Length > 60 ? title[..60] : title;
            icon.BalloonTipText = body.Length == 0 ? " " : body.Length > 240 ? body[..240] : body;
            icon.ShowBalloonTip(10000);
            hide!.Stop(); hide.Start();
        }
        catch (Exception) { }
    }

    static void Hide()
    {
        try { hide?.Stop(); if (icon != null) icon.Visible = false; } catch (Exception) { }
    }
}
