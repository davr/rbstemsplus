// The Status tab's list (app/Sources/PrepareView.swift's StatusTab): the prepared playlists (kept sets), until when
// each is kept, and whether it is still ready: the helper's quick check, which reads nothing from a drive that
// may be away. Prepare Again chooses the playlist on the Prepare tab; Stop Keeping lets go of a set.
using System.Drawing;

namespace RBStemsPlus;

sealed class SetsTab : UserControl
{
    readonly AppPaths p;
    readonly Log log;
    readonly Func<Manifest?> saved;
    readonly Func<string, string> nameOf;
    readonly Func<string?> runningSet;
    readonly Action<string> prepareAgain;
    readonly ListView list = new() { Dock = DockStyle.Fill, View = View.Details, FullRowSelect = true, HideSelection = false, MultiSelect = false };
    readonly Button againBtn = new() { Text = "Prepare Again", Width = Ui.S(120), Height = Ui.S(28) }, stopBtn = new() { Text = "Stop Keeping", Width = Ui.S(120), Height = Ui.S(28) };
    readonly Label empty = new() { Dock = DockStyle.Top, AutoSize = false, Height = Ui.S(40), Padding = new Padding(Ui.S(8), Ui.S(8), Ui.S(8), Ui.S(0)), ForeColor = SystemColors.GrayText };
    readonly Dictionary<string, string> states = new();
    CancellationTokenSource? cts;

    public SetsTab(AppPaths p, Log log, Func<Manifest?> saved, Func<string, string> nameOf, Func<string?> runningSet, Action<string> prepareAgain)
    {
        this.p = p; this.log = log; this.saved = saved; this.nameOf = nameOf; this.runningSet = runningSet; this.prepareAgain = prepareAgain;
        _ = Handle;                                            // made now, on the window's thread: background results are posted to it
        list.Columns.Add("Playlist", Ui.S(260)); list.Columns.Add("Kept until", Ui.S(110)); list.Columns.Add("Tracks", Ui.S(60)); list.Columns.Add("State", Ui.S(260));
        var buttons = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = Ui.S(40), Padding = new Padding(Ui.S(8), Ui.S(4), Ui.S(0), Ui.S(0)) };
        buttons.Controls.AddRange(new Control[] { againBtn, stopBtn });
        Controls.Add(list);
        Controls.Add(buttons);
        Controls.Add(empty);
        againBtn.Click += (_, _) => { if (Current() is { } s) prepareAgain(s.Name); };
        stopBtn.Click += (_, _) => StopKeeping();
        list.SelectedIndexChanged += (_, _) => UpdateButtons();
    }

    Prepare.KeptSet? Current() => list.SelectedItems.Count == 1 ? list.SelectedItems[0].Tag as Prepare.KeptSet : null;

    void Post(Action a) => Ui.Post(this, log, "status", a);

    void UpdateButtons()
    {
        var s = Current();
        againBtn.Enabled = s != null && runningSet() == null;
        stopBtn.Enabled = s != null && runningSet() != s.Name;
    }

    /// The sets on disk now, each checked in the background.
    public void Reload()
    {
        var sets = Prepare.KeptSets(p.KeptDir, DateTimeOffset.UtcNow);
        var sel = Current()?.Name;
        list.BeginUpdate();
        list.Items.Clear();
        foreach (var s in sets)
        {
            var item = new ListViewItem(new[] { nameOf(s.Name), s.Until is { } u ? u.ToLocalTime().ToString("d") : "?", s.Files > 0 ? s.Files.ToString() : "", states.GetValueOrDefault(s.Name, "Checking…") }) { Tag = s };
            list.Items.Add(item);
            if (s.Name == sel) item.Selected = true;
        }
        list.EndUpdate();
        empty.Text = sets.Count == 0 ? "No prepared playlists. Prepare one on the Prepare tab to have its tracks load with their stems at once." : "";
        UpdateButtons();
        Check(sets);
    }

    void Check(List<Prepare.KeptSet> sets)
    {
        cts?.Cancel();
        var mine = cts = new CancellationTokenSource();
        var facts = PrepareEnv.Facts(p, saved());
        if (sets.Count == 0 || Prepare.Problems(facts).Count > 0) return;
        var baseArgs = PrepareEnv.BaseArgs(p, facts);
        Task.Run(() =>
        {
          try
          {
            foreach (var s in sets)
            {
                if (mine.IsCancellationRequested) return;
                if (runningSet() == s.Name) continue;                        // being prepared now: the run says how it goes
                var r = PrepareEnv.CheckSet(s.Name, baseArgs.Concat(new[] { "--estimate", "--quick", "--keep", s.Name }).ToList(),
                                            a => PrepareEnv.RunToEnd(a, TimeSpan.FromSeconds(120), cancel: mine.Token));
                var state = Prepare.SetState(Prepare.ParseEstimate(r.Out));
                Post(() =>
                {
                    if (mine.IsCancellationRequested) return;
                    states[s.Name] = state;
                    foreach (ListViewItem it in list.Items) if (it.Tag is Prepare.KeptSet k && k.Name == s.Name) it.SubItems[3].Text = state;
                });
            }
          }
          catch (Exception e) { log.Write("status: ERROR while checking the sets: " + e); }
        });
    }

    void StopKeeping()
    {
        if (Current() is not { } s) return;
        var page = new TaskDialogPage
        {
            Caption = "RB Stems Plus", Heading = $"Stop keeping “{nameOf(s.Name)}”?",
            Text = "Its stems stay in Stems Cache like any others, but they can be removed when the cache needs room.",
        };
        var yes = new TaskDialogButton("Stop Keeping");
        page.Buttons.Add(yes); page.Buttons.Add(TaskDialogButton.Cancel);
        if (TaskDialog.ShowDialog(FindForm()!, page) != yes) return;
        if (Prepare.StopKeeping(s.Name, p.KeptDir)) log.Write("prepare: stopped keeping " + s.Name);
        states.Remove(s.Name);
        Reload();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) cts?.Cancel();
        base.Dispose(disposing);
    }
}
