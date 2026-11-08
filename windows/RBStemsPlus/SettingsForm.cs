// Settings (app/Sources/Settings.swift and LocationActions.swift): the size limit, the days limit,
// Clear Saved Stems, and where the stems are saved: this PC, or a folder on an external drive.
// The bridge reads them the next time rekordbox starts.
using System.Drawing;

namespace RBStemsPlus;

sealed class SettingsForm : Form
{
    readonly AppPaths p;
    readonly Log log;
    readonly NumericUpDown maxGB = new() { Minimum = 0.1m, Maximum = 100000, DecimalPlaces = 1, Increment = 1, Width = 90 };
    readonly NumericUpDown maxDays = new() { Minimum = 1, Maximum = 36500, Width = 90 };
    readonly Label where = new() { AutoSize = true, MaximumSize = new Size(420, 0) };
    string? chosenDir, chosenId;

    public SettingsForm(AppPaths p, Log log)
    {
        this.p = p; this.log = log;
        Text = "Settings";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = MinimizeBox = false;
        StartPosition = FormStartPosition.CenterParent;
        AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink;
        Padding = new Padding(16);
        var st = ConfigFile.Read(p.ConfigFile);
        if (st.Unreadable != null) log.Write("Settings: config.ini can't be read (" + st.Unreadable + "): Save won't write over it");
        maxGB.Value = (decimal)st.Config.MaxGB;
        maxDays.Value = (decimal)st.Config.MaxDays;
        if (st.Location is ConfigLocation.Chosen c) { chosenDir = c.Dir; chosenId = c.Id; }
        var change = new Button { Text = "Change…", AutoSize = true };
        var thisPc = new Button { Text = "Use This PC", AutoSize = true };
        var clear = new Button { Text = "Clear Saved Stems", AutoSize = true };
        var save = new Button { Text = "Save", AutoSize = true, DialogResult = DialogResult.OK };
        var cancel = new Button { Text = "Cancel", AutoSize = true, DialogResult = DialogResult.Cancel };
        AcceptButton = save; CancelButton = cancel;
        var grid = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, Dock = DockStyle.Top };
        grid.Controls.Add(new Label { Text = "Stems Cache location:", AutoSize = true, Margin = new Padding(0, 6, 8, 0) });
        var loc = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown };
        loc.Controls.Add(where);
        var lb = new FlowLayoutPanel { AutoSize = true };
        lb.Controls.AddRange(new Control[] { change, thisPc });
        loc.Controls.Add(lb);
        grid.Controls.Add(loc);
        grid.Controls.Add(new Label { Text = "Stems Cache size limit (GB):", AutoSize = true, Margin = new Padding(0, 6, 8, 0) });
        grid.Controls.Add(maxGB);
        grid.Controls.Add(new Label { Text = "Remove stems unused for (days):", AutoSize = true, Margin = new Padding(0, 6, 8, 0) });
        grid.Controls.Add(maxDays);
        var note = new Label { AutoSize = true, MaximumSize = new Size(560, 0), ForeColor = SystemColors.GrayText, Margin = new Padding(0, 12, 0, 12),
                               Text = "The settings apply the next time rekordbox opens. Saved stems take about 13 MB per minute of music." };
        var buttons = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Bottom, FlowDirection = FlowDirection.RightToLeft };
        buttons.Controls.AddRange(new Control[] { cancel, save, clear });
        var all = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true };
        all.Controls.AddRange(new Control[] { grid, note, buttons });
        Controls.Add(all);
        ShowWhere();
        change.Click += (_, _) => ChooseFolder();
        thisPc.Click += (_, _) => { chosenDir = chosenId = null; ShowWhere(); };
        clear.Click += (_, _) => ClearStems();
        save.Click += (_, _) => Save();
    }

    void ShowWhere()
    {
        var size = CacheLocation.EntriesSize(chosenDir ?? p.DefaultCacheDir) is { } n ? CacheLocation.AboutSize(n) : "";
        where.Text = (chosenDir ?? "This PC") + size;
    }

    void ChooseFolder()
    {
        using var d = new FolderBrowserDialog { Description = "Choose the external drive (or a folder on it) for saved stems. A folder named \"RB Stems Plus Cache\" is made there.", UseDescriptionForTitle = true };
        if (d.ShowDialog(this) != DialogResult.OK) return;
        var target = d.SelectedPath;
        var refusal = CacheLocation.Check(target);
        if (refusal != CacheLocation.Refusal.None)
        {
            MessageBox.Show(this, CacheLocation.RefusalText(refusal, Path.GetPathRoot(target) ?? target), "RB Stems Plus", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }
        var (dir, id, err) = CacheLocation.Create(target);
        if (dir == null) { MessageBox.Show(this, "The folder couldn't be made: " + err, "RB Stems Plus", MessageBoxButtons.OK, MessageBoxIcon.Warning); return; }
        chosenDir = dir; chosenId = id;
        log.Write($"Settings: chose {dir} ({(CacheLocation.ReadMarker(dir) == id ? "its marker" : "new marker")} {id})");
        ShowWhere();
    }

    void ClearStems()
    {
        var folder = chosenDir ?? p.DefaultCacheDir;
        if (Rekordbox.Running()) { MessageBox.Show(this, "Quit rekordbox first.", "RB Stems Plus"); return; }
        if (MessageBox.Show(this, $"Delete the stems saved in {folder}? rekordbox separates those tracks again the next time you load them.", "Clear Saved Stems",
                            MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
        CacheLocation.ClearEntries(folder, log.Write);
        ShowWhere();
    }

    void Save()
    {
        var before = ConfigFile.Read(p.ConfigFile);
        bool ok = ConfigFile.WriteLimits(p.ConfigFile, (double)maxGB.Value, (double)maxDays.Value);
        var beforeDir = before.Location is ConfigLocation.Chosen c ? c.Dir : null;
        if (ok && beforeDir != chosenDir) ok = ConfigFile.WriteLocation(p.ConfigFile, chosenDir, chosenId);
        if (!ok)
        {
            MessageBox.Show(this, "The settings couldn't be saved (config.ini can't be read or written).", "RB Stems Plus", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            DialogResult = DialogResult.None;
            return;
        }
        log.Write($"Settings: max_gb={ConfigFile.Number((double)maxGB.Value)} max_days={ConfigFile.Number((double)maxDays.Value)} location={chosenDir ?? "this PC"}");
        if (beforeDir != chosenDir && beforeDir == null && !Rekordbox.Running())
        {
            // switching to a drive starts empty: this PC's stems go (as on macOS)
            CacheLocation.ClearEntries(p.DefaultCacheDir, log.Write);
        }
    }
}
