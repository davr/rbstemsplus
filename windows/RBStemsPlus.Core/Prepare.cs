// Preparing playlists (app/Sources/Prepare.swift of upstream 1.3.0): the checks before preparing, what the
// helper (rbsp-prepare.exe) prints, the kept sets, and the words the Prepare tab uses. No window here:
// that is PrepareTab.cs. The decisions are upstream's: at most 5 playlists, 75% of the limit, kept 10 days.
using System.Globalization;
using System.Text;
using System.Text.Json;

namespace RBStemsPlus;

public static class PrepareDefaults
{
    public const string HelperName = "rbsp-prepare.exe";
    public const int KeptSetsMax = 5;
    public const int KeepDaysDefault = 10;
}

/// What preparing depends on (Prepare.swift's PrepareFacts); gathered by the app, or by the tests.
public sealed class PrepareFacts
{
    public bool HelperThere = true;
    public bool RekordboxInstalled = true;
    public bool RekordboxOK = true;             // its version is one Stems Cache supports
    public string RekordboxVersion = "";
    public bool CacheInstalled = true;
    public bool BridgeKeeps = true;             // the bridge says kept=1
    public bool OrtThere = true;                // Pioneer's ONNX Runtime, the copy Stems Cache keeps
    public string? ConfigUnreadable;
    public bool Enabled = true;
    public bool CacheLit = true;                // Stems Cache's light is on (not red or yellow)
    public string CacheTip = "";
    public int? Quality;                        // PartAnalysisQuality: 0 sound quality, 1 speed, null unknown
    public bool PlusOn;                         // our model is installed (else rekordbox's own runs)
    public string? ModelSha;                    // the installed model's checksum (null: not known yet)
    public List<string> OurModels = new();
    public string? CacheFolder;                 // where the stems go now (null: none usable)
    public bool ChosenDrive;
    public double MaxGB = 20;
}

public static class Prepare
{
    /// What a run of playlist id adds to the helper's arguments. first: "Prepare the First N Tracks".
    public static List<string> RunArgs(string id, string until, int? first)
    {
        var a = new List<string> { "--until", until, "--keep", id, "--playlist", id };
        if (first is { } n) { a.Add("--first"); a.Add(n.ToString(CultureInfo.InvariantCulture)); }
        return a;
    }

    /// Why playlists can't be prepared now, one line each (none: they can). The same lines for the
    /// Prepare tab and the watcher's check.
    public static List<string> Problems(PrepareFacts f)
    {
        if (!f.HelperThere) return new() { "This copy of RB Stems Plus can't prepare playlists: its helper is missing. Download RB Stems Plus again." };
        if (!f.RekordboxInstalled) return new() { "rekordbox 7 isn't installed." };
        if (!f.CacheInstalled) return new() { "Install Stems Cache first: prepared stems are saved where Stems Cache saves them." };
        var o = new List<string>();
        if (!f.BridgeKeeps || !f.OrtThere) o.Add("Reinstall Stems Cache: this installation can't keep prepared playlists.");
        if (!f.RekordboxOK) o.Add($"Preparing doesn't support rekordbox {f.RekordboxVersion} yet. RB Stems Plus checks again each time you open it.");
        if (f.ConfigUnreadable is { } why) o.Add($"Stems Cache can't read its settings (config.ini: {why}).");
        else if (!f.Enabled) o.Add("Stems Cache is turned off in config.ini (enabled=0).");
        if (!f.CacheLit && f.CacheTip.Length > 0 && !o.Contains(f.CacheTip)) o.Add(f.CacheTip);
        if (f.Quality == 1)
            o.Add("rekordbox is set to Prioritize speed for STEMS: prepared stems are used only with Prioritize sound quality (rekordbox > Preferences > Extensions > STEMS).");
        if (f.ModelSha is { } sha && f.PlusOn && !f.OurModels.Contains(sha))
            o.Add("The Demucs v4 model in rekordbox isn't the one RB Stems Plus installed: reinstall Demucs v4.");
        if (f.CacheFolder == null && o.Count == 0) o.Add("Where Stems Cache saves stems can't be used now.");
        return o;
    }

    /// PartAnalysisQuality in rekordbox's settings file: <VALUE name="PartAnalysisQuality" val="0"/>.
    public static int? StemsQuality(string file)
    {
        try
        {
            var fi = new FileInfo(file);
            if (!fi.Exists || fi.Length >= 4 << 20) return null;
            var s = File.ReadAllText(file, Encoding.UTF8);
            const string key = "name=\"PartAnalysisQuality\"";
            int at = s.IndexOf(key, StringComparison.Ordinal);
            if (at < 0) return null;
            var rest = s.Substring(at + key.Length, Math.Min(64, s.Length - at - key.Length));
            int v = rest.IndexOf("val=\"", StringComparison.Ordinal);
            if (v < 0) return null;
            var digits = new string(rest[(v + 5)..].TakeWhile(char.IsAsciiDigit).ToArray());
            return digits.Length > 0 ? int.Parse(digits, CultureInfo.InvariantCulture) : null;
        }
        catch (IOException) { return null; }
        catch (UnauthorizedAccessException) { return null; }
    }

    // ---------------------------------------------------------------- what the helper prints (one JSON object per line)

    public sealed record Playlist(string Id, string Parent, string Name, string Kind, int Tracks)
    {
        public bool CanPrepare => Kind == "playlist";
    }

    /// One track's line, from an estimate or a run.
    public sealed record TrackLine
    {
        public string File { get; init; } = "";
        public string Result { get; init; } = "";
        public string Why { get; init; } = "";
        public string Access { get; init; } = "";
        public string Folder { get; init; } = "";
        public int Entries { get; init; }
        public int Written { get; init; }
        public int Already { get; init; }
        public int Missing { get; init; }
        public int Refused { get; init; }
        public double Seconds { get; init; }
        public double GB { get; init; }
        public string Name => System.IO.Path.GetFileName(File.Replace('/', '\\'));
    }

    static JsonElement? Line(string line)
    {
        line = line.Trim();
        if (line.Length == 0 || line[0] != '{') return null;
        try { using var d = JsonDocument.Parse(line); return d.RootElement.Clone(); }
        catch (JsonException) { return null; }
    }

    static string Str(JsonElement o, string k) =>
        o.TryGetProperty(k, out var v) ? v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : v.ValueKind == JsonValueKind.Number ? v.GetRawText() : "" : "";
    static double Num(JsonElement o, string k) => o.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : 0;
    static int Int(JsonElement o, string k) => (int)Num(o, k);
    static bool Bool(JsonElement o, string k) => o.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.True;

    static IEnumerable<JsonElement> Lines(string output) =>
        output.Split('\n').Select(l => Line(l)).Where(l => l.HasValue).Select(l => l!.Value);

    public static List<Playlist> ParsePlaylists(string output) =>
        Lines(output).Select(o => (id: Str(o, "playlist"), o)).Where(x => x.id.Length > 0)
            .Select(x => new Playlist(x.id, Str(x.o, "parent"), Str(x.o, "name"), Str(x.o, "kind"), Int(x.o, "tracks"))).ToList();

    static TrackLine Track(JsonElement o) => new()
    {
        File = Str(o, "file"), Result = Str(o, "result"), Why = Str(o, "why"), Access = Str(o, "access"), Folder = Str(o, "folder"),
        Entries = Int(o, "entries"), Written = Int(o, "written"), Already = Int(o, "already"), Missing = Int(o, "missing"),
        Refused = Int(o, "refused"), Seconds = Num(o, "seconds"), GB = Num(o, "gb"),
    };

    /// --tracks' lines: a playlist's tracks as the library lists them, with nothing said about them yet.
    public static List<TrackLine> ParseTrackList(string output) =>
        Lines(output).Select(o => Str(o, "file")).Where(f => f.Length > 0).Select(f => new TrackLine { File = f }).ToList();

    /// The same tracks, with nothing said about them: what the last check said isn't what is true now.
    public static List<TrackLine> NotChecked(IEnumerable<TrackLine> rows) => rows.Select(r => new TrackLine { File = r.File }).ToList();

    /// t in its track's row, or in a new row at the end for a track the rows don't have. Its row. Lines come in the
    /// rows' order: hint, the row after the last one, is tried first, or big playlists stall the window.
    public static int Place(TrackLine t, List<TrackLine> rows, int hint = 0)
    {
        if (hint < rows.Count && string.Equals(rows[hint].File, t.File, StringComparison.Ordinal)) { rows[hint] = t; return hint; }
        int i = rows.FindIndex(r => string.Equals(r.File, t.File, StringComparison.Ordinal));
        if (i >= 0) { rows[i] = t; return i; }
        rows.Add(t);
        return rows.Count - 1;
    }

    /// A prepared track's note: whether it was separated now or saved already, and parts that couldn't be saved.
    public static string PreparedNote(TrackLine t)
    {
        if (t.Written == 0 && t.Already == 0) return t.Refused > 0 ? "couldn't be saved: rekordbox separates it when loaded" : "silent: nothing to save";
        var s = t.Written == 0 ? "already saved" : t.Already > 0 ? "partly saved already, the rest separated now" : "separated now";
        return t.Refused > 0 ? s + ", some parts couldn't be saved" : s;
    }

    /// A row's Result column. Nothing said about the track yet: checking tells a check under way from no check at all.
    public static string ResultText(TrackLine t, bool checking)
    {
        if (t.Result.Length == 0) return checking ? "Checking…" : "Not checked";
        return t.Result switch
        {
            "ready" => "Ready", "prepare" => "To prepare", "skipped" => "Skipped", "prepared" => "Prepared",
            "stopped" => "Stopped", "unchecked" => "Not checked", _ => t.Result,
        };
    }

    /// An estimate's last line (the helper's header comment) and its tracks.
    public sealed class Estimate
    {
        public List<TrackLine> Tracks = new();
        public string? Refused;
        public int Ready, PrepareCount, Skipped, Unchecked, Asking, FitFiles;
        public double TimeS, NeedGB, KeptGB, ShareGB, MaxGB, MaxGBNeeded;
        public bool Fits = true, DiskOK = true;
        public List<string> Folders = new();
        public IEnumerable<TrackLine> ToPrepare => Tracks.Where(t => t.Result == "prepare");
    }

    public static Estimate? ParseEstimate(string output)
    {
        var e = new Estimate();
        bool ended = false;
        foreach (var o in Lines(output))
        {
            if (o.TryGetProperty("refused", out var r) && r.ValueKind == JsonValueKind.String) { e.Refused = r.GetString(); return e; }
            if (o.TryGetProperty("file", out _)) { e.Tracks.Add(Track(o)); continue; }
            if (!o.TryGetProperty("estimate", out _)) continue;
            ended = true;
            e.Ready = Int(o, "ready"); e.PrepareCount = Int(o, "prepare"); e.Skipped = Int(o, "skipped"); e.Unchecked = Int(o, "unchecked");
            e.Asking = Int(o, "asking"); e.FitFiles = Int(o, "fit_files");
            e.TimeS = Num(o, "time_s"); e.NeedGB = Num(o, "need_gb"); e.KeptGB = Num(o, "kept_gb"); e.ShareGB = Num(o, "share_gb");
            e.MaxGB = Num(o, "max_gb"); e.MaxGBNeeded = Num(o, "max_gb_needed");
            e.Fits = Bool(o, "fits");
            e.DiskOK = Bool(o, "disk_ok");
            if (o.TryGetProperty("folders", out var fl) && fl.ValueKind == JsonValueKind.Array)
                e.Folders = fl.EnumerateArray().Where(x => x.ValueKind == JsonValueKind.String).Select(x => x.GetString()!).ToList();
        }
        return ended ? e : null;
    }

    /// A run's lines as they come.
    public abstract record RunLine
    {
        public sealed record Progress(int File, int Files, int Chunk, int Chunks) : RunLine;
        public sealed record TrackDone(TrackLine Track) : RunLine;
        public sealed record Room(int Removed, double CacheGB) : RunLine;
        public sealed record Done(int Files, string? Stopped, double KeptGB, string? Error) : RunLine;
    }

    public static RunLine? ParseRunLine(string line)
    {
        if (Line(line) is not { } o) return null;
        if (o.TryGetProperty("chunk", out _)) return new RunLine.Progress(Int(o, "at"), Int(o, "files"), Int(o, "chunk"), Int(o, "chunks"));
        if (o.TryGetProperty("file", out _)) return new RunLine.TrackDone(Track(o));
        if (o.TryGetProperty("room", out _)) return new RunLine.Room(Int(o, "room"), Num(o, "cache_gb"));
        if (o.TryGetProperty("done", out _))
        {
            var err = new[] { Str(o, "kept_error"), Str(o, "manifest_error") }.FirstOrDefault(s => s.Length > 0);
            return new RunLine.Done(Int(o, "done"), o.TryGetProperty("stopped", out var st) && st.ValueKind == JsonValueKind.String ? st.GetString() : null, Num(o, "kept_gb"), err);
        }
        return null;
    }

    // ---------------------------------------------------------------- the kept sets

    public sealed record KeptSet(string Name, DateTimeOffset? Until = null, int Entries = 0, int Files = 0);

    /// A set's name, as the helper allows it (kept.c, name_ok).
    public static bool KeptNameOK(string s) =>
        s.Length is > 0 and <= 64 && s.All(c => c is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or >= '0' and <= '9' or '-' or '_');

    /// The kept sets in dir whose date hasn't passed (now), by name.
    public static List<KeptSet> KeptSets(string dir, DateTimeOffset now)
    {
        var o = new List<KeptSet>();
        if (!Directory.Exists(dir)) return o;
        foreach (var path in Directory.EnumerateFiles(dir, "*.kept"))
        {
            var n = System.IO.Path.GetFileName(path);
            if (n.StartsWith('.') || !n.EndsWith(".kept", StringComparison.Ordinal)) continue;
            var name = n[..^5];
            var fi = new FileInfo(path);
            if (!KeptNameOK(name) || (fi.Attributes & FileAttributes.ReparsePoint) != 0) continue;
            var set = new KeptSet(name);
            if (fi.Length <= 8 << 20)
            {
                var lines = File.ReadAllText(path, Encoding.UTF8).Split('\n');
                if (lines.Length >= 2 && lines[0] == "rbstemsplus-kept 1" && lines[1].StartsWith("until ", StringComparison.Ordinal)
                    && double.TryParse(lines[1][6..], NumberStyles.Float, CultureInfo.InvariantCulture, out var u))
                    set = set with { Until = DateTimeOffset.FromUnixTimeSeconds((long)u), Entries = lines.Skip(2).Count(l => l.Length == 129) };
            }
            if (set.Until is { } until && until <= now) continue;
            var manifest = System.IO.Path.Combine(dir, name + ".manifest");
            if (File.Exists(manifest)) set = set with { Files = File.ReadLines(manifest).Count(l => l.StartsWith("file ", StringComparison.Ordinal)) };
            o.Add(set);
        }
        return o.OrderBy(s => s.Name, StringComparer.Ordinal).ToList();
    }

    /// Stops keeping a set: its file and its manifest go (its stems stay, like any others). True if the set is gone.
    public static bool StopKeeping(string name, string dir)
    {
        if (!KeptNameOK(name)) return false;
        foreach (var f in new[] { System.IO.Path.Combine(dir, name + ".kept"), System.IO.Path.Combine(dir, name + ".manifest") })
        {
            try { if (File.Exists(f) && (File.GetAttributes(f) & FileAttributes.ReparsePoint) == 0) File.Delete(f); }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
        return !File.Exists(System.IO.Path.Combine(dir, name + ".kept"));
    }

    // ---------------------------------------------------------------- the views' words (PrepareView.swift, Watcher.swift)

    /// The notification's title when a run ends.
    public static string RunEndTitle(string name, int status, bool gotDone, string? stopped, int finished, int skipped, string? error, bool partial = false)
    {
        if (status != 0 || !gotDone) return $"“{name}” couldn't be prepared";
        if (stopped == null && skipped == 0 && error == null && !partial) return $"“{name}” is prepared";
        return finished == 0 ? $"“{name}” couldn't be prepared" : $"“{name}” isn't fully prepared";
    }

    /// The last run in one line, under the next estimate.
    public static string LastRunLine(int files, int skipped, string? stopped, string? error = null)
    {
        var s = $"Last time: {files} {(files == 1 ? "track" : "tracks")} done";
        if (skipped > 0) s += $", {skipped} skipped";
        if (stopped != null) s += stopped == "stopped" ? ", stopped before the end" : stopped.Contains("75%") ? ", stopped at the 75% share" : ", stopped early (see the log)";
        if (error != null) s += ", the set couldn't be saved (see the log)";
        return s + ".";
    }

    /// A set's state from its check.
    public static string SetState(Estimate? e)
    {
        if (e == null) return "Couldn't be checked";
        if (e.Refused is { } r) return r;
        if (e.Ready + e.PrepareCount + e.Skipped + e.Unchecked == 0) return "No tracks in the playlist";
        if (e.PrepareCount == 0 && e.Skipped == 0) return e.Unchecked > 0 ? $"Ready ({e.Unchecked} not checked)" : "Ready";
        var parts = new List<string>();
        if (e.PrepareCount > 0) parts.Add($"{e.PrepareCount} {(e.PrepareCount == 1 ? "track" : "tracks")} to prepare");
        if (e.Skipped > 0) parts.Add($"{e.Skipped} can't be read");
        return string.Join(", ", parts);
    }

    public sealed class PNode
    {
        public Playlist P;
        public List<PNode> Children = new();
        public PNode(Playlist p) { P = p; }
    }

    /// The tree of playlists from --playlists' lines (parents come before their children).
    public static List<PNode> PlaylistTree(IEnumerable<Playlist> list)
    {
        var byId = new Dictionary<string, PNode>();
        var roots = new List<PNode>();
        foreach (var p in list)
        {
            var n = new PNode(p);
            byId[p.Id] = n;
            if (byId.TryGetValue(p.Parent, out var parent)) parent.Children.Add(n); else roots.Add(n);
        }
        return roots;
    }

    /// A regular expression for the helper preparing set name, matched against its command line (RunArgs: a check has no --until).
    public static string SetRunPattern(string name) => @"rbsp-prepare(\.exe)?""? .*--until \S+ --keep " + System.Text.RegularExpressions.Regex.Escape(name) + @"( |$)";

    /// A check's tracks that aren't ready ("unchecked" ones weren't looked at).
    public static List<string> NotReadyTracks(Estimate e) => e.Tracks.Where(t => t.Result is "prepare" or "skipped").Select(t => t.File).ToList();

    /// Whether a notification tells something new: a track not ready that the last one didn't cover.
    public static bool NewlyNotReady(IEnumerable<string> now, IEnumerable<string> noticed) => !now.ToHashSet(StringComparer.Ordinal).IsSubsetOf(noticed);

    /// What the notification says about a prepared playlist that isn't ready, or null when it is.
    public static string? KeptSetNotice(Estimate e)
    {
        int n = e.PrepareCount + e.Skipped;
        if (n <= 0) return null;
        return $"{n} {(n == 1 ? "track isn't" : "tracks aren't")} fully prepared: rekordbox may separate {(n == 1 ? "it" : "them")} as you play. Open RB Stems Plus to see why and prepare it again.";
    }

    // ---------------------------------------------------------------- words

    /// A line for the logs, which never hold track names or paths: every path in it becomes <path>.
    public static string NoPaths(string s) =>
        System.Text.RegularExpressions.Regex.Replace(s, @"([A-Za-z]:|\\\\)[\\/][^\r\n:;,)""]*", "<path>");

    /// "1 h 05 min", "12 min", "under a minute"
    public static string DurationText(double s)
    {
        if (s < 60) return "under a minute";
        int m = (int)Math.Round(s / 60, MidpointRounding.AwayFromZero);
        return m < 60 ? $"{m} min" : $"{m / 60} h {m % 60:00} min";
    }

    /// "850 MB", "2.4 GB"
    public static string SizeText(double gb) =>
        gb < 1 ? $"{Math.Max(1, (int)Math.Round(gb * 1024, MidpointRounding.AwayFromZero))} MB" : gb.ToString("0.0", CultureInfo.InvariantCulture) + " GB";

    /// What an estimate says, for the Prepare tab: the tracks, then the space and time.
    public static string EstimateSummary(Estimate e)
    {
        var parts = new List<string>();
        if (e.Ready > 0) parts.Add($"{e.Ready} {(e.Ready == 1 ? "track" : "tracks")} ready");
        if (e.PrepareCount > 0) parts.Add($"{e.PrepareCount} to prepare ({DurationText(e.ToPrepare.Sum(t => t.Seconds))} of music)");
        if (e.Skipped > 0) parts.Add($"{e.Skipped} skipped");
        var s = parts.Count == 0 ? "No tracks." : string.Join(", ", parts) + ".";
        if (e.PrepareCount > 0) s += $" About {SizeText(e.NeedGB)} and {DurationText(e.TimeS)}.";
        s += $" Prepared playlists will use {SizeText(e.KeptGB + e.NeedGB)} of the {SizeText(e.ShareGB)} they may use (75% of Stems Cache's {ConfigFile.Number(e.MaxGB)} GB limit).";
        return s;
    }

    /// The "doesn't fit" choices: prepare the tracks that fit, raise the limit (only when the disk has room for
    /// it), or cancel. freeGB: the disk's free space above its floor.
    public static (string Message, List<string> Buttons, double? RaiseTo) DoesntFitChoices(Estimate e, double freeGB)
    {
        double need = Math.Ceiling(e.MaxGBNeeded * 10) / 10;
        var buttons = new List<string>();
        if (e.FitFiles > 0) buttons.Add($"Prepare the First {e.FitFiles} {(e.FitFiles == 1 ? "Track" : "Tracks")}");
        double? raise = need > e.MaxGB && need - e.MaxGB <= freeGB ? need : null;
        if (raise is { } r) buttons.Add($"Raise the Limit to {ConfigFile.Number(r)} GB");
        buttons.Add("Cancel");
        var message = $"Prepared playlists may use 75% of Stems Cache's {ConfigFile.Number(e.MaxGB)} GB limit ({SizeText(e.ShareGB)}), the rest stays for the tracks rekordbox separates during a set. This playlist would take them to {SizeText(e.KeptGB + e.NeedGB)}."
                      + (raise == null ? " The disk doesn't have room for a higher limit." : "");
        return (message, buttons, raise);
    }
}
