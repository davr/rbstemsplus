// The watcher's check of the prepared playlists as rekordbox opens (app/Sources/Watcher.swift): once per rekordbox
// session (by its pid), each kept set is checked with the helper's quick estimate (nothing is read from a drive that
// may be away); a set that isn't fully ready gets a notification (a balloon, never a dialog over rekordbox), once
// for the tracks it names (Prepare.NewlyNotReady). Silent when nothing is prepared or preparing can't work.
using System.Text.Json;

namespace RBStemsPlus;

static class PrepareWatch
{
    static string NoticedFile(AppPaths p) => Path.Combine(p.Support, "prepare-noticed");

    static Dictionary<string, List<string>> ReadNoticed(AppPaths p)
    {
        try { return JsonSerializer.Deserialize<Dictionary<string, List<string>>>(Files.ReadText(NoticedFile(p)) ?? "{}") ?? new(); }
        catch (JsonException) { return new(); }
    }

    public static void Check(AppPaths p, Log log, Action<string, string> notify)
    {
        var sets = Prepare.KeptSets(p.KeptDir, DateTimeOffset.UtcNow);
        if (sets.Count == 0) { log.Write("prepared playlists: none"); return; }
        var facts = PrepareEnv.Facts(p, Payload.Saved(new PayloadFiles(p)));
        var problems = Prepare.Problems(facts);
        if (problems.Count > 0) { log.Write("prepared playlists not checked: " + string.Join(" ", problems)); return; }
        var names = Prepare.ParsePlaylists(PrepareEnv.RunToEnd(new[] { "--playlists", "--library", p.RbLibraryFile }, TimeSpan.FromSeconds(30)).Out);
        var baseArgs = PrepareEnv.BaseArgs(p, facts);
        var noticed = ReadNoticed(p);
        foreach (var s in sets)
        {
            if (PrepareEnv.RunningSet(p) == s.Name) { log.Write($"prepared playlist {s.Name}: being prepared, not checked"); continue; }
            var r = PrepareEnv.CheckSet(s.Name, baseArgs.Concat(new[] { "--estimate", "--quick", "--keep", s.Name }).ToList(),
                                        a => PrepareEnv.RunToEnd(a, TimeSpan.FromSeconds(60)));
            var e = Prepare.ParseEstimate(r.Out);
            if (e == null) { log.Write($"prepared playlist {s.Name}: couldn't be checked ({Prepare.NoPaths(r.Err)})"); continue; }
            var state = Prepare.SetState(e);
            log.Write($"prepared playlist {s.Name}: {state}");
            var now = Prepare.NotReadyTracks(e);
            var before = noticed.GetValueOrDefault(s.Name) ?? new();
            if (Prepare.NewlyNotReady(now, before) && Prepare.KeptSetNotice(e) is { } body)
                notify($"“{names.FirstOrDefault(x => x.Id == s.Name)?.Name ?? s.Name}” isn't fully prepared", body);
            noticed[s.Name] = now;
        }
        Files.TryWriteText(NoticedFile(p), JsonSerializer.Serialize(noticed));
    }
}
