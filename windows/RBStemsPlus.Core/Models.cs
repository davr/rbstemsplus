// Our stems models and Pioneer's (app/Sources/Models.swift, the same decisions): which models are
// ours (by checksum, from signed facts only), saving Pioneer's model before ours replaces it,
// choosing and putting back the right one, and installing ours. Pioneer's models are saved in
// originals\ as <sha256>.onnx, each with <sha256>.engine holding the STEMS Engine version.
namespace RBStemsPlus;

public sealed record Outcome(bool Ok, string Message, bool NoSpace = false);

public sealed class Models
{
    public const string UnknownModelMessage = "rekordbox's stems model isn't one RB Stems Plus knows, so it was left as it is.";

    public string Dir { get; }
    public string Originals { get; }
    public HashSet<string> Ours { get; }
    public Action<string> Log = _ => { };
    /// Called right before rekordbox's model is checked again and replaced (the tests change it there).
    public Action WillReplace = () => { };

    public Models(string dir, string originals, IEnumerable<string> ours) { Dir = dir; Originals = originals; Ours = ours.ToHashSet(); }

    public static Models ForAccount(AppPaths p, Manifest? m, Action<string> log) => new(p.ModelDir, p.OriginalsDir, OurChecksums(m)) { Log = log };

    /// The checksums of the models that are ours: the compiled-in one and, from a payload whose
    /// signature verified, its model and previous_models.
    public static HashSet<string> OurChecksums(Manifest? m)
    {
        var s = new HashSet<string> { Consts.OurModel };
        if (m?.SignedBy != null) { s.Add(m.Model.Sha256); foreach (var x in m.PreviousModels ?? new()) s.Add(x); }
        return s;
    }

    public string Model => Path.Combine(Dir, "hdemucs.onnx");
    string NewTemp => Path.Combine(Dir, ".hdemucs.new");
    string RestoreTemp => Path.Combine(Dir, ".hdemucs.restore");
    public string Saved(string sha) => Path.Combine(Originals, sha + ".onnx");
    string EngineFile(string sha) => Path.Combine(Originals, sha + ".engine");
    string Engine => Rekordbox.EngineVersion(Dir);

    Outcome Fail(string s) { Log("  " + s); return new(false, s); }
    Outcome Done(string s) { Log("restore Pioneer's model: " + s); return new(true, s); }

    public bool SavedIsGood(string sha) => File.Exists(Saved(sha)) && !Files.IsReparsePoint(Saved(sha)) && Files.Sha256(Saved(sha)) == sha;

    public Outcome SaveOriginal(string sha)
    {
        if (!Files.IsSha256(sha) || Ours.Contains(sha)) return Fail("rekordbox's stems model is the Demucs v4 model, so it wasn't saved as rekordbox's own.");
        if (SavedIsGood(sha)) { Log($"save Pioneer's model: already saved ({sha[..12]})"); return new(true, ""); }
        Directory.CreateDirectory(Originals);
        var e = Engine;
        var part = Saved(sha) + ".part";
        Files.Remove(part);
        var err = Files.Copy(Model, part);
        if (err != null || Files.Sha256(part) != sha)
        {
            Files.Remove(part);
            Log($"save Pioneer's model: FAILED {err ?? "the copy didn't match"}");
            return new(false, err?.Contains("No space") == true ? "There isn't enough free space to save a copy of rekordbox's own stems model."
                                                                : "A copy of rekordbox's own stems model couldn't be saved.");
        }
        if (e.Length > 0 && !File.Exists(EngineFile(sha))) Files.TryWriteText(EngineFile(sha), e + "\n");
        if (!Place(part, sha)) { Files.Remove(part); return Fail("A copy of rekordbox's own stems model couldn't be saved."); }
        Log($"save Pioneer's model: saved ({sha[..12]}, STEMS Engine {(e.Length == 0 ? "unknown" : e)})");
        return new(true, "");
    }

    /// Renames a checked file to originals\<sha>.onnx, never over another file; a damaged copy
    /// there is set aside first.
    bool Place(string file, string sha)
    {
        var dest = Saved(sha);
        if (File.Exists(dest) && !SavedIsGood(sha))
        {
            Log($"saved model {sha[..12]} is damaged: set aside");
            Files.Remove(dest + ".damaged");
            try { File.Move(dest, dest + ".damaged"); } catch { }
        }
        try { File.Move(file, dest, overwrite: false); return true; } catch { return false; }
    }

    /// Pioneer's saved models, best first: the current STEMS Engine's, then the highest known
    /// engine, then unknown ones; the most recently saved first among equals.
    public List<(string Sha, string Engine)> SavedOriginals()
    {
        var current = Engine;
        var list = new List<(string Sha, string Engine, DateTime Date)>();
        try
        {
            foreach (var f in Directory.EnumerateFiles(Originals, "*.onnx"))
            {
                var sha = Path.GetFileNameWithoutExtension(f);
                if (!Files.IsSha256(sha) || Ours.Contains(sha) || Files.IsReparsePoint(f)) continue;
                var e = (Files.ReadText(EngineFile(sha)) ?? "").Trim();
                list.Add((sha, e, File.GetLastWriteTimeUtc(f)));
            }
        }
        catch { }
        int Rank(string e) => current.Length > 0 && e == current ? 0 : e.Length == 0 ? 2 : 1;
        list.Sort((a, b) =>
        {
            if (Rank(a.Engine) != Rank(b.Engine)) return Rank(a.Engine).CompareTo(Rank(b.Engine));
            if (a.Engine != b.Engine) return -Payload.CompareVersions(a.Engine, b.Engine);
            return b.Date.CompareTo(a.Date);
        });
        return list.Select(x => (x.Sha, x.Engine)).ToList();
    }

    public List<string> VerifiedOriginals() => SavedOriginals().Select(x => x.Sha).Where(SavedIsGood).ToList();

    /// Whether a model that isn't ours can be taken for Pioneer's: none saved, it is one of them, or
    /// the current STEMS Engine differs from every saved one's.
    public bool MayBePioneers(string sha, List<(string Sha, string Engine)>? saved = null)
    {
        saved ??= SavedOriginals();
        if (saved.Count == 0 || saved.Any(s => s.Sha == sha)) return true;
        var e = Engine;
        return e.Length > 0 && saved.All(s => s.Engine.Length > 0 && s.Engine != e);
    }

    /// Puts Pioneer's model back over ours (see Models.swift restore()).
    public Outcome Restore()
    {
        if (!Directory.Exists(Dir)) return Done("rekordbox's model folder isn't there: nothing to put back.");
        bool present = File.Exists(Model);
        var live = present ? Files.Sha256(Model) : null;
        if (present && live == null) return Fail("rekordbox's stems model can't be read.");
        var candidates = SavedOriginals();
        if (live != null && !Ours.Contains(live))
        {
            if (!MayBePioneers(live, candidates)) return Fail(UnknownModelMessage);
            return Done("Pioneer's stems model is already in place.");
        }
        if (candidates.Count == 0)
        {
            if (!present) return Done("rekordbox has no stems model and none was saved: left as it is.");
            return Fail("There's no saved copy of rekordbox's own stems model on this PC.");
        }
        foreach (var o in candidates)
        {
            Files.Remove(RestoreTemp);
            var err = Files.Copy(Saved(o.Sha), RestoreTemp);
            if (err != null)
            {
                Files.Remove(RestoreTemp);
                if (err.Contains("No space")) return new(false, "There isn't enough free space to put rekordbox's own stems model back.", true);
                return Fail($"rekordbox's own stems model couldn't be put back: {err}.");
            }
            if (Files.Sha256(RestoreTemp) != o.Sha) { Files.Remove(RestoreTemp); Log($"  saved model {o.Sha[..12]} didn't verify: trying the next one"); continue; }
            WillReplace();
            if ((File.Exists(Model) ? Files.Sha256(Model) : null) != live) { Files.Remove(RestoreTemp); return Fail("rekordbox's stems model changed meanwhile."); }
            try { File.Move(RestoreTemp, Model, overwrite: true); }
            catch (Exception e) { Files.Remove(RestoreTemp); return Fail($"rekordbox's own stems model couldn't be put back: {e.Message}."); }
            if (Files.Sha256(Model) != o.Sha) return Fail("rekordbox's stems model didn't verify after it was put back.");
            return Done($"Pioneer's stems model is back ({o.Sha[..12]}, STEMS Engine {(o.Engine.Length == 0 ? "unknown" : o.Engine)}).");
        }
        return Fail("The saved copy of rekordbox's own stems model is damaged.");
    }

    /// Puts our model (`source`, checksum `sha`) in rekordbox's place (see Models.swift installOurs()).
    public Outcome InstallOurs(string source, string sha)
    {
        bool present = File.Exists(Model);
        if (present && Files.IsReparsePoint(Model)) return Fail("rekordbox's stems model isn't a file.");
        var live = present ? Files.Sha256(Model) : null;
        if (present && live == null) return Fail("rekordbox's stems model can't be read.");
        if (live != null && !Ours.Contains(live))
        {
            if (!MayBePioneers(live)) return Fail(UnknownModelMessage);
            var s = SaveOriginal(live);
            if (!s.Ok) return s;
            if (!SavedIsGood(live)) return Fail("A copy of rekordbox's own stems model couldn't be saved.");
        }
        else if (!present) Log("Pioneer's model: none to save");
        Directory.CreateDirectory(Dir);
        Files.Remove(NewTemp);
        var err = Files.Copy(source, NewTemp);
        if (err != null)
        {
            Files.Remove(NewTemp);
            return Fail(err.Contains("No space") ? "There isn't enough free space to copy the Demucs v4 model." : $"The Demucs v4 model couldn't be copied: {err}.");
        }
        if (Files.Sha256(NewTemp) != sha) { Files.Remove(NewTemp); return Fail("The Demucs v4 model didn't copy correctly."); }
        WillReplace();
        if ((File.Exists(Model) ? Files.Sha256(Model) : null) != live) { Files.Remove(NewTemp); return Fail("rekordbox's stems model changed meanwhile."); }
        try { File.Move(NewTemp, Model, overwrite: true); }
        catch (Exception e) { Files.Remove(NewTemp); return Fail($"The Demucs v4 model couldn't be put in place: {e.Message}."); }
        Log($"Demucs v4: installed ({sha[..12]})");
        return new(true, "");
    }

    /// What rekordbox's model is when RB Stems Plus is removed without putting Pioneer's back.
    public string LeftBehind()
    {
        if (!File.Exists(Model)) return "rekordbox has no stems model.";
        var sha = Files.Sha256(Model);
        if (sha == null) return "rekordbox's stems model can't be read.";
        return Ours.Contains(sha) ? "rekordbox still has the Demucs v4 model." : "rekordbox's stems model isn't one RB Stems Plus knows.";
    }
}
