// Prepare.cs, with fake data: the checks before preparing, the bridge's kept=1, rekordbox's quality setting, the
// helper's lines (playlists, an estimate, a run), the kept sets on disk, the tree of playlists and the words the
// window shows. Ported from upstream 1.3.0's app/Tests/PrepareTests.swift.
using System.Text;
using Xunit;
using static RBStemsPlus.Prepare;

namespace RBStemsPlus.Tests;

public class PrepareTests
{
    static PrepareFacts Ok() => new() { CacheFolder = @"C:\x" };

    [Fact] public void EverythingInPlace_NoProblem() => Assert.Empty(Problems(Ok()));
    [Fact] public void NoHelper_SaysSo() => Assert.StartsWith("This copy of RB Stems Plus can't prepare", Problems(new PrepareFacts { HelperThere = false, CacheFolder = "x" })[0]);
    [Fact] public void NoStemsCache_InstallItFirst() => Assert.StartsWith("Install Stems Cache first", Problems(new PrepareFacts { CacheInstalled = false, CacheFolder = "x" })[0]);
    [Fact] public void BridgeWithoutKept_Reinstall() =>
        Assert.Equal(new[] { "Reinstall Stems Cache: this installation can't keep prepared playlists." }, Problems(new PrepareFacts { BridgeKeeps = false, CacheFolder = "x" }));
    [Fact] public void NoOrt_Reinstall() => Assert.StartsWith("Reinstall Stems Cache", Problems(new PrepareFacts { OrtThere = false, CacheFolder = "x" })[0]);
    [Fact] public void PrioritizeSpeed_SaidWithWhereToChangeIt() => Assert.Contains("Prioritize speed", Problems(new PrepareFacts { Quality = 1, CacheFolder = "x" })[0]);
    [Fact] public void PrioritizeSoundQuality_Fine() => Assert.Empty(Problems(new PrepareFacts { Quality = 0, CacheFolder = "x" }));
    [Fact] public void StemsCacheOffInConfig() =>
        Assert.Equal(new[] { "Stems Cache is turned off in config.ini (enabled=0)." }, Problems(new PrepareFacts { Enabled = false, CacheFolder = "x" }));
    [Fact] public void ConfigUnreadable_ThatNotEnabled0() =>
        Assert.Equal(new[] { "Stems Cache can't read its settings (config.ini: a NUL byte)." }, Problems(new PrepareFacts { ConfigUnreadable = "a NUL byte", Enabled = false, CacheFolder = "x" }));
    [Fact] public void UnlistedRekordbox_NotSupportedYet() =>
        Assert.StartsWith("Preparing doesn't support rekordbox 7.2.20 yet.", Problems(new PrepareFacts { RekordboxOK = false, RekordboxVersion = "7.2.20", CacheFolder = "x" })[0]);
    [Fact] public void CacheNotSavingNow_ItsLightsReason()
    {
        var tip = "Stems Cache starts saving stems once rekordbox has downloaded its STEMS Engine.";
        Assert.Equal(new[] { tip }, Problems(new PrepareFacts { CacheLit = false, CacheTip = tip, CacheFolder = "x" }));
    }
    [Fact] public void UnknownModelWithDemucsOn_ReinstallIt()
    {
        var f = new PrepareFacts { PlusOn = true, ModelSha = "ab" + new string('0', 62), OurModels = { new string('c', 64) }, CacheFolder = "x" };
        Assert.Contains("reinstall Demucs v4", Problems(f)[0]);
        f.ModelSha = f.OurModels[0];
        Assert.Empty(Problems(f));
    }
    [Fact] public void NoFolderInUse_Said() =>
        Assert.Equal(new[] { "Where Stems Cache saves stems can't be used now." }, Problems(new PrepareFacts { CacheFolder = null }));

    [Fact] public void QualityRead_NotTheSettingNamedLikeIt()
    {
        var f = Path.Combine(Path.GetTempPath(), "rbsp-test-" + Guid.NewGuid().ToString("N") + ".settings");
        File.WriteAllText(f, "<PROPERTIES><VALUE name=\"PartAnalysis\" val=\"1\"/><VALUE name=\"PartAnalysisQuality\" val=\"1\"/></PROPERTIES>");
        try { Assert.Equal(1, StemsQuality(f)); }
        finally { File.Delete(f); }
        Assert.Null(StemsQuality(f));
    }

    [Fact] public void BridgeCaps_KeptToken()
    {
        byte[] Line(string s) => Encoding.ASCII.GetBytes("xx rbstems bridge: yy rbstems-cache: " + s + "; zz");
        Assert.True(Rekordbox.BridgeSaysKept(Line("rebuild=1 location=1 kept=1 pioneer=abc")));
        Assert.False(Rekordbox.BridgeSaysKept(Line("rebuild=1 location=1 pioneer=abc")));
        Assert.False(Rekordbox.BridgeSaysKept(Line("rebuild=1 pioneer=kept=1")));
        Assert.False(Rekordbox.BridgeSaysKept(Encoding.ASCII.GetBytes("rbstems-cache: kept=1;")));
    }

    [Fact] public void Playlists_Parsed_QuotesUnescaped_OnlyPlainOnesPreparable()
    {
        var pl = ParsePlaylists("{\"playlist\":\"1\",\"parent\":\"root\",\"name\":\"Folder\",\"kind\":\"folder\",\"tracks\":0}\n" +
                                "{\"playlist\":\"2\",\"parent\":\"1\",\"name\":\"Friday \\\"Late\\\"\",\"kind\":\"playlist\",\"tracks\":12}\n" +
                                "{\"playlist\":\"3\",\"parent\":\"root\",\"name\":\"Smart\",\"kind\":\"intelligent\",\"tracks\":0}\nnot json\n");
        Assert.Equal(3, pl.Count);
        Assert.Equal("Friday \"Late\"", pl[1].Name);
        Assert.Equal(12, pl[1].Tracks);
        Assert.True(pl[1].CanPrepare);
        Assert.False(pl[0].CanPrepare);
        Assert.False(pl[2].CanPrepare);
        var tree = PlaylistTree(pl);
        Assert.Equal(2, tree.Count);
        Assert.Equal(new[] { "2" }, tree[0].Children.Select(c => c.P.Id));
    }

    const string EstimateText =
        "{\"file\":\"C:\\\\m\\\\a.flac\",\"result\":\"ready\",\"entries\":6,\"gb\":0.015}\n" +
        "{\"file\":\"C:\\\\m\\\\b.m4a\",\"result\":\"prepare\",\"seconds\":300.0,\"entries\":56,\"gb\":0.156,\"missing\":2}\n" +
        "{\"file\":\"\\\\\\\\nas\\\\share\\\\c.mp3\",\"result\":\"skipped\",\"why\":\"the file isn't there\",\"access\":\"denied\",\"folder\":\"share\"}\n" +
        "{\"estimate\":3,\"ready\":1,\"prepare\":1,\"skipped\":1,\"entries\":56,\"runs\":28,\"time_s\":70,\"need_gb\":0.156,\"kept_gb\":2.000,\"share_gb\":15.000,\"max_gb\":20,\"cache_gb\":4.000,\"free_gb\":100.0,\"fits\":true,\"fit_files\":3,\"max_gb_needed\":2.875,\"disk_ok\":true,\"asking\":1,\"unchecked\":0,\"folders\":[\"share\"]}\n";

    [Fact] public void Estimate_Parsed()
    {
        var e = ParseEstimate(EstimateText)!;
        Assert.Equal(3, e.Tracks.Count);
        Assert.Equal(1, e.Ready); Assert.Equal(1, e.PrepareCount); Assert.Equal(1, e.Asking);
        Assert.Equal(new[] { "share" }, e.Folders);
        Assert.True(e.Fits);
        Assert.Equal(2, e.Tracks[1].Missing);
        Assert.Equal("denied", e.Tracks[2].Access);
        Assert.Null(ParseEstimate("{\"file\":\"C:\\\\a\",\"result\":\"ready\"}"));
        Assert.StartsWith("5 playlists", ParseEstimate("{\"refused\":\"5 playlists are kept already: stop keeping one first\"}")!.Refused);
    }

    [Fact] public void EstimateSummary_Words()
    {
        var s = Prepare.EstimateSummary(ParseEstimate(EstimateText)!);
        Assert.StartsWith("1 track ready, 1 to prepare (5 min of music), 1 skipped. About 160 MB and 1 min.", s);
        Assert.Contains("2.2 GB of the 15.0 GB", s);
        Assert.Contains("20 GB limit", s);
    }

    [Fact] public void TrackRows()
    {
        var rows = ParseTrackList("{\"file\":\"C:\\\\m\\\\a \\\"x\\\".flac\"}\nnot json\n{\"file\":\"C:\\\\m\\\\b.m4a\"}\n");
        Assert.Equal(new[] { "a \"x\".flac", "b.m4a" }, rows.Select(r => r.Name));
        Assert.All(rows, r => Assert.Equal("", r.Result));
        Assert.Empty(ParseTrackList(""));
        Assert.Equal("Checking…", ResultText(rows[0], true));
        Assert.Equal("Not checked", ResultText(rows[0], false));
        Assert.Equal(1, Place(new TrackLine { File = rows[1].File, Result = "ready" }, rows));
        Assert.Equal("ready", rows[1].Result);
        Assert.Equal(2, Place(new TrackLine { File = @"C:\m\c.mp3", Result = "prepare" }, rows));
        Assert.Equal(3, rows.Count);
        Assert.Equal("Ready", ResultText(rows[1], true));
        Assert.Equal(2, Place(new TrackLine { File = @"C:\m\c.mp3", Result = "ready" }, rows, hint: 2));
        Assert.Equal(1, Place(new TrackLine { File = rows[1].File, Result = "skipped" }, rows, hint: 0));
        Assert.Equal(3, Place(new TrackLine { File = @"C:\m\x.mp3", Result = "ready" }, rows, hint: 99));
        var forms = new List<TrackLine> { new() { File = "/m/caf\u00e9.mp3" }, new() { File = "/m/cafe\u0301.mp3" } };
        Assert.Equal(1, Place(new TrackLine { File = "/m/cafe\u0301.mp3", Result = "ready" }, forms));
        Assert.Equal("", forms[0].Result);
        var nc = NotChecked(rows);
        Assert.Equal(rows.Select(r => r.File), nc.Select(r => r.File));
        Assert.All(nc, r => Assert.Equal("", r.Result));
    }

    [Fact] public void PreparedNotes()
    {
        Assert.Equal("separated now", PreparedNote(new TrackLine { Written = 6 }));
        Assert.Equal("already saved", PreparedNote(new TrackLine { Already = 6 }));
        Assert.Equal("partly saved already, the rest separated now", PreparedNote(new TrackLine { Written = 2, Already = 4 }));
        Assert.Equal("separated now, some parts couldn't be saved", PreparedNote(new TrackLine { Written = 5, Refused = 1 }));
        Assert.Equal("couldn't be saved: rekordbox separates it when loaded", PreparedNote(new TrackLine { Refused = 6 }));
        Assert.Equal("silent: nothing to save", PreparedNote(new TrackLine()));
    }

    [Fact] public void RunLines()
    {
        Assert.Equal(new RunLine.Progress(2, 10, 3, 30), ParseRunLine("{\"at\":2,\"files\":10,\"chunk\":3,\"chunks\":30}"));
        Assert.Equal(new RunLine.Room(4, 17.5), ParseRunLine("{\"room\":4,\"cache_gb\":17.500}"));
        Assert.Equal(new RunLine.Done(5, "stopped", 0.5, "x"),
                     ParseRunLine("{\"done\":5,\"stopped\":\"stopped\",\"runs\":3,\"run_s\":5.0,\"kept\":\"7\",\"until\":1,\"kept_gb\":0.5,\"manifest_error\":\"x\"}"));
        var t = Assert.IsType<RunLine.TrackDone>(ParseRunLine("{\"file\":\"C:\\\\m\\\\a.flac\",\"result\":\"prepared\",\"written\":3,\"already\":1,\"silent\":0,\"refused\":0,\"how\":\"pcm\"}"));
        Assert.Equal(3, t.Track.Written); Assert.Equal(1, t.Track.Already); Assert.Equal("a.flac", t.Track.Name);
        Assert.Null(ParseRunLine("not json"));
    }

    [Fact] public void DoesntFitChoices_Sheet()
    {
        var big = new Estimate { Fits = false, FitFiles = 4, MaxGB = 20, MaxGBNeeded = 24.31, ShareGB = 15, KeptGB = 12, NeedGB = 6.2 };
        var d = DoesntFitChoices(big, 50);
        Assert.Equal(new[] { "Prepare the First 4 Tracks", "Raise the Limit to 24.4 GB", "Cancel" }, d.Buttons);
        Assert.Equal(24.4, d.RaiseTo);
        d = DoesntFitChoices(big, 3);
        Assert.Equal(new[] { "Prepare the First 4 Tracks", "Cancel" }, d.Buttons);
        Assert.Contains("doesn't have room for a higher limit", d.Message);
        big.FitFiles = 0;
        Assert.Equal(new[] { "Cancel" }, DoesntFitChoices(big, 3).Buttons);
    }

    [Fact] public void KeptSets_OnDisk()
    {
        var kd = Path.Combine(Path.GetTempPath(), "rbsp-kept-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(kd);
        try
        {
            var entry = new string('e', 64) + " " + new string('a', 64);
            var now = DateTimeOffset.UtcNow;
            long Unix(int add) => now.ToUnixTimeSeconds() + add;
            File.WriteAllText(Path.Combine(kd, "123.kept"), $"rbstemsplus-kept 1\nuntil {Unix(3600)}\n{entry}\n{entry}\n");
            File.WriteAllText(Path.Combine(kd, "123.manifest"), "rbstemsplus-manifest 1\nhelper 1\nplaylist 123\nfile 1 2 1 0 a.flac\nfile 1 2 1 0 b.flac\n");
            File.WriteAllText(Path.Combine(kd, "old.kept"), $"rbstemsplus-kept 1\nuntil {Unix(-10)}\n");
            File.WriteAllText(Path.Combine(kd, "bad.kept"), "nonsense");
            File.WriteAllText(Path.Combine(kd, ".hidden.kept"), "x");
            File.WriteAllText(Path.Combine(kd, "no space.kept"), "x");
            var sets = KeptSets(kd, now);
            Assert.Equal(new[] { "123", "bad" }, sets.Select(s => s.Name));
            Assert.Equal(2, sets[0].Entries); Assert.Equal(2, sets[0].Files); Assert.NotNull(sets[0].Until); Assert.Null(sets[1].Until);
            Assert.True(StopKeeping("123", kd));
            Assert.False(File.Exists(Path.Combine(kd, "123.kept"))); Assert.False(File.Exists(Path.Combine(kd, "123.manifest")));
            Assert.False(StopKeeping(@"..\x", kd));
            Assert.True(KeptNameOK("133477215") && KeptNameOK("a-b_C"));
            Assert.False(KeptNameOK("") || KeptNameOK("a b") || KeptNameOK(new string('a', 65)));
        }
        finally { Directory.Delete(kd, true); }
    }

    [Fact] public void RunWords()
    {
        Assert.Equal("Last time: 1 track done, stopped before the end.", LastRunLine(1, 0, "stopped"));
        Assert.Equal("Last time: 12 tracks done, 2 skipped.", LastRunLine(12, 2, null));
        Assert.Equal("Last time: 3 tracks done, stopped at the 75% share.", LastRunLine(3, 0, "prepared stems would take more than 75% of the cache's 20 GB limit"));
        Assert.Equal("Last time: 12 tracks done, the set couldn't be saved (see the log).", LastRunLine(12, 0, null, "the kept set can't be written"));
        Assert.Equal("Last time: 0 tracks done, 5 skipped.", LastRunLine(0, 5, null));
        string T(int status = 0, bool gotDone = true, string? stopped = null, int finished = 3, int skipped = 0, string? error = null, bool partial = false) =>
            RunEndTitle("Fri", status, gotDone, stopped, finished, skipped, error, partial);
        Assert.Equal("“Fri” is prepared", T());
        Assert.Equal("“Fri” isn't fully prepared", T(stopped: "prepared stems would take more than 75%"));
        Assert.Equal("“Fri” isn't fully prepared", T(stopped: "stopped"));
        Assert.Equal("“Fri” isn't fully prepared", T(skipped: 1));
        Assert.Equal("“Fri” isn't fully prepared", T(partial: true));
        Assert.Equal("“Fri” isn't fully prepared", T(error: "the kept set can't be written"));
        Assert.Equal("“Fri” couldn't be prepared", T(stopped: "stopped", finished: 0));
        Assert.Equal("“Fri” couldn't be prepared", T(finished: 0, skipped: 5));
        Assert.Equal("“Fri” couldn't be prepared", T(status: 1, gotDone: false));
        Assert.Equal("“Fri” couldn't be prepared", T(gotDone: false));
    }

    [Fact] public void SetChecks_AndNotifications()
    {
        var good = "{\"estimate\":1,\"ready\":1,\"prepare\":0,\"skipped\":0}";
        var e = ParseEstimate("{\"file\":\"C:\\\\a.mp3\",\"result\":\"ready\"}\n{\"file\":\"C:\\\\b.mp3\",\"result\":\"prepare\"}\n{\"file\":\"C:\\\\c.mp3\",\"result\":\"skipped\",\"why\":\"gone\"}\n" +
                              "{\"file\":\"D:\\\\d.mp3\",\"result\":\"unchecked\"}\n{\"estimate\":4,\"ready\":1,\"prepare\":1,\"skipped\":1,\"unchecked\":1}")!;
        Assert.Equal(new[] { @"C:\b.mp3", @"C:\c.mp3" }, NotReadyTracks(e));
        Assert.True(NewlyNotReady(new[] { "/b" }, new string[0]));
        Assert.False(NewlyNotReady(new[] { "/b" }, new[] { "/b" }));
        Assert.False(NewlyNotReady(new[] { "/b" }, new[] { "/b", "/c" }));
        Assert.True(NewlyNotReady(new[] { "/b", "/d" }, new[] { "/b" }));
        Assert.False(NewlyNotReady(new string[0], new[] { "/b" }));
        Assert.Equal("No tracks in the playlist", SetState(ParseEstimate("{\"estimate\":0,\"ready\":0,\"prepare\":0,\"skipped\":0}")));
        Assert.Equal("Ready", SetState(ParseEstimate(good)));
        Assert.Equal("Couldn't be checked", SetState(null));
        var r = new Estimate { Ready = 2 };
        Assert.Equal("Ready", SetState(r));
        r.PrepareCount = 3; r.Skipped = 1;
        Assert.Equal("3 tracks to prepare, 1 can't be read", SetState(r));
        r.PrepareCount = 0; r.Skipped = 0; r.Unchecked = 2;
        Assert.Equal("Ready (2 not checked)", SetState(r));
        Assert.Null(KeptSetNotice(r));
        r.PrepareCount = 1;
        Assert.Equal("1 track isn't fully prepared: rekordbox may separate it as you play. Open RB Stems Plus to see why and prepare it again.", KeptSetNotice(r));
        r.Skipped = 2;
        Assert.StartsWith("3 tracks aren't fully prepared: rekordbox may separate them", KeptSetNotice(r));
        Assert.Equal(new[] { "--until", "99", "--keep", "42", "--playlist", "42" }, RunArgs("42", "99", null));
        Assert.Equal(new[] { "--until", "99", "--keep", "42", "--playlist", "42", "--first", "3" }, RunArgs("42", "99", 3));
        var pat = new System.Text.RegularExpressions.Regex(SetRunPattern("42"));
        Assert.Matches(pat, "\"C:\\x\\rbsp-prepare.exe\" --model m --until 99 --keep 42 --playlist 42");
        Assert.DoesNotMatch(pat, "rbsp-prepare.exe --model m --keep 42 --quick");
        Assert.DoesNotMatch(pat, "rbsp-prepare.exe --until 99 --keep 421 --playlist 421");
    }

    [Fact] public void Words()
    {
        Assert.Equal("can't read <path>", NoPaths(@"can't read C:\Users\dj\Music\Café ♫.flac"));
        Assert.Equal("answer for <path>: then check again", NoPaths(@"answer for \\nas\share\My Drive: then check again"));
        Assert.Equal("the file isn't there (moved, renamed, or its drive isn't connected)", NoPaths("the file isn't there (moved, renamed, or its drive isn't connected)"));
        Assert.Equal("under a minute", DurationText(30)); Assert.Equal("10 min", DurationText(600)); Assert.Equal("1 h 05 min", DurationText(3900));
        Assert.Equal("160 MB", SizeText(0.156)); Assert.Equal("2.4 GB", SizeText(2.44));
    }
}
