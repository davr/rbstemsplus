// The Windows app's logic, checked on any OS (dotnet test). The config and marker fixtures are the
// ones the Windows bridge's tests use (tests/config-fixtures-windows.txt, tests/marker-fixtures.txt).
using System.Security.Cryptography;
using System.Text;
using Xunit;

namespace RBStemsPlus.Tests;

static class Fixtures
{
    public static string Repo
    {
        get
        {
            var d = AppContext.BaseDirectory;
            while (d != null && !File.Exists(Path.Combine(d, "VERSION"))) d = Path.GetDirectoryName(d);
            return d ?? throw new InvalidOperationException("repository root not found");
        }
    }

    /// The fixture escapes: \n \r \t \\ \xHH \*COUNT.C (bridge/tests/fixtures.py).
    public static byte[] Decode(string s)
    {
        var o = new List<byte>();
        for (int i = 0; i < s.Length;)
        {
            if (s[i] != '\\') { o.AddRange(Encoding.UTF8.GetBytes(s[i].ToString())); i++; continue; }
            char n = s[i + 1];
            if (n is 'n' or 'r' or 't' or '\\') { o.Add((byte)(n == 'n' ? '\n' : n == 'r' ? '\r' : n == 't' ? '\t' : '\\')); i += 2; }
            else if (n == 'x') { o.Add(Convert.ToByte(s.Substring(i + 2, 2), 16)); i += 4; }
            else if (n == '*')
            {
                int dot = s.IndexOf('.', i + 2);
                int count = int.Parse(s[(i + 2)..dot]);
                char c = s[dot + 1];
                for (int k = 0; k < count; k++) o.AddRange(Encoding.UTF8.GetBytes(c.ToString()));
                i = dot + 2;
            }
            else throw new FormatException(s);
        }
        return o.ToArray();
    }

    public static IEnumerable<(string Name, string Expected, byte[] Content)> Read(string file)
    {
        foreach (var line in File.ReadAllText(Path.Combine(Repo, "tests", file)).Split('\n'))
        {
            if (line.Length == 0 || line.StartsWith('#')) continue;
            var parts = line.Split('\t', 3);
            yield return (parts[0], parts[1], parts.Length > 2 ? Decode(parts[2]) : Array.Empty<byte>());
        }
    }
}

public class ConfigTests
{
    public static IEnumerable<object[]> ConfigCases() => Fixtures.Read("config-fixtures-windows.txt").Select(f => new object[] { f.Name });

    [Theory]
    [MemberData(nameof(ConfigCases))]
    public void FixtureReadsAsTheBridgeDoes(string name)
    {
        var f = Fixtures.Read("config-fixtures-windows.txt").First(x => x.Name == name);
        var dir = Directory.CreateTempSubdirectory().FullName;
        var file = Path.Combine(dir, "config.ini");
        if (name != "none") File.WriteAllBytes(file, f.Content);
        var st = ConfigFile.Read(file);
        var want = f.Expected.Split(' ').Select(w => w.Split('=', 2)).ToDictionary(w => w[0], w => w[1]);
        if (want["location"] == "unreadable") { Assert.NotNull(st.Unreadable); return; }
        Assert.Null(st.Unreadable);
        Assert.Equal(want["enabled"] == "1", st.Config.Enabled);
        Assert.Equal(double.Parse(want["max_gb"], System.Globalization.CultureInfo.InvariantCulture), st.Config.MaxGB);
        Assert.Equal(double.Parse(want["max_days"], System.Globalization.CultureInfo.InvariantCulture), st.Config.MaxDays);
        switch (want["location"])
        {
            case "thismac": Assert.IsType<ConfigLocation.ThisPC>(st.Location); break;
            case "malformed": Assert.IsType<ConfigLocation.Malformed>(st.Location); break;
            case "chosen":
                var c = Assert.IsType<ConfigLocation.Chosen>(st.Location);
                Assert.Equal(Encoding.UTF8.GetString(Fixtures.Decode(want["dir"])).Replace("%20", " "), c.Dir);
                break;
            default: throw new Exception("unknown location " + want["location"]);
        }
    }

    [Fact]
    public void WriterKeepsOtherLinesAndReadsBack()
    {
        var file = Path.Combine(Directory.CreateTempSubdirectory().FullName, "config.ini");
        File.WriteAllText(file, "\uFEFF# mine\nmax_gb=3\n[watcher]\nenabled=0\n");
        Assert.True(ConfigFile.WriteLimits(file, 7.5, 30));
        var t = File.ReadAllText(file);
        Assert.StartsWith("\uFEFF# mine\n", t);
        Assert.Contains("max_gb=7.5\n", t);
        Assert.Contains("max_days=30\n[watcher]\nenabled=0\n", t);
        Assert.True(ConfigFile.WriteLocation(file, @"E:\Sets\RB Stems Plus Cache", "0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F"));
        Assert.IsType<ConfigLocation.Chosen>(ConfigFile.Read(file).Location);
        Assert.True(ConfigFile.WriteLocation(file, null, null));
        Assert.IsType<ConfigLocation.ThisPC>(ConfigFile.Read(file).Location);
        Assert.False(ConfigFile.WriteLocation(file, @"E:\Sets\Other", "0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F"));
    }

    [Fact]
    public void NeverWritesOverAnUnreadableFile()
    {
        var file = Path.Combine(Directory.CreateTempSubdirectory().FullName, "config.ini");
        File.WriteAllBytes(file, new byte[] { (byte)'a', 0, (byte)'\n' });
        Assert.False(ConfigFile.WriteLimits(file, 5, 5));
        Assert.Equal(3, File.ReadAllBytes(file).Length);
    }
}

public class MarkerTests
{
    public static IEnumerable<object[]> MarkerCases() => Fixtures.Read("marker-fixtures.txt").Select(f => new object[] { f.Name });

    [Theory]
    [MemberData(nameof(MarkerCases))]
    public void FixtureJudgedAsTheBridgeDoes(string name)
    {
        var f = Fixtures.Read("marker-fixtures.txt").First(x => x.Name == name);
        var dir = Directory.CreateTempSubdirectory().FullName;
        File.WriteAllBytes(Path.Combine(dir, Consts.MarkerName), f.Content);
        var id = CacheLocation.ReadMarker(dir);
        bool valid = id != null && string.Equals(id, "0F8C2D3A-1B2C-4D5E-8F90-1A2B3C4D5E6F", StringComparison.OrdinalIgnoreCase);
        Assert.Equal(f.Expected == "valid", valid);
    }
}

public class PayloadTests
{
    static (ECDsa Key, TrustedKey Trusted) NewKey(string name)
    {
        var k = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        return (k, TrustedKey.FromBase64(name, Convert.ToBase64String(k.ExportSubjectPublicKeyInfo()))!);
    }

    static byte[] Json(string version = "1.3.0", string extra = "") => Encoding.UTF8.GetBytes(
        "{\"payload_version\":\"" + version + "\",\"commit\":\"abc\"," + extra +
        "\"app\":{\"version\":\"" + version + "\",\"zip\":\"rbstemsplus-windows.zip\",\"sha256\":\"" + new string('a', 64) + "\"}," +
        "\"bridge\":{\"file\":\"onnxruntime.dll\",\"sha256\":\"" + new string('b', 64) + "\"}," +
        "\"model\":{\"file\":\"stemsplus-model.onnx\",\"sha256\":\"" + new string('c', 64) + "\",\"size\":308572524}," +
        "\"previous_models\":[]," +
        "\"compatible\":{\"rekordbox\":[\"7.2.14\"],\"ort_version_prefix\":\"1.18.\",\"stems_engine\":[\"0002\"]}}");

    static byte[] Sign(ECDsa k, byte[] d) => k.SignData(d, HashAlgorithmName.SHA256, DSASignatureFormat.Rfc3279DerSequence);

    [Fact]
    public void SignedPayloadCountsAndIsSaved()
    {
        var (k, t) = NewKey("test");
        var p = AppPaths.ForTestRoot(Directory.CreateTempSubdirectory().FullName);
        var f = new PayloadFiles(p, new() { t }, "1.0.0");
        var data = Json();
        var (m, why) = Payload.Accept(data, Sign(k, data), f);
        Assert.Equal(PayloadRefusal.None, why);
        Assert.Equal("test", m!.SignedBy);
        Assert.Equal("1.3.0", Payload.Floor(f));
        Assert.Equal("7.2.14", Payload.Saved(f)!.Compat.Rekordbox[0]);
        Assert.Contains(new string('c', 64), Models.OurChecksums(m));
    }

    [Fact]
    public void RefusesUnsignedForgedOlderAndDuplicateKeys()
    {
        var (k, t) = NewKey("test");
        var (other, _) = NewKey("other");
        var keys = new List<TrustedKey> { t };
        var data = Json();
        Assert.Equal(PayloadRefusal.NoSignature, Payload.Check(data, null, keys, null).Why);
        Assert.Equal(PayloadRefusal.BadSignature, Payload.Check(data, Sign(other, data), keys, null).Why);
        Assert.Equal(PayloadRefusal.BadSignature, Payload.Check(Json("1.3.1"), Sign(k, data), keys, null).Why);
        Assert.Equal(PayloadRefusal.Older, Payload.Check(data, Sign(k, data), keys, "1.10.0").Why);
        var dup = Json(extra: "\"commit\":\"twice\",");
        Assert.Equal(PayloadRefusal.NotValid, Payload.Check(dup, Sign(k, dup), keys, null).Why);
        Assert.False(Payload.PlainJsonKeys(Encoding.UTF8.GetBytes("{\"a\\u0062\":1}")));
        Assert.True(Payload.PlainJsonKeys(Encoding.UTF8.GetBytes("{\"a\":[1,{\"b\":true}],\"c\":null}")));
    }

    [Fact]
    public void VersionOrder()
    {
        Assert.True(Payload.Older("1.0.9", "1.0.10"));
        Assert.False(Payload.Older("1.2.0", "1.2.0"));
        Assert.True(Payload.Older("1.2", "1.2.1"));
    }

    [Fact]
    public void CompiledKeysAreTheRepositorysKeys()
    {
        var names = CompiledKeys.All.Select(k => k.Name).ToList();
        Assert.Contains("release", names);
        Assert.Contains("backup", names);
        Assert.Equal(2, Signing.Compiled.Count);
    }
}

public class ModelsTests
{
    sealed class Bench
    {
        public string Root = Directory.CreateTempSubdirectory().FullName;
        public string Dir => Path.Combine(Root, "demucs3_model");
        public string Orig => Path.Combine(Root, "originals");
        public string Ours = "", OursSha = "";
        public Models M = null!;
        public Bench(string pioneer = "pioneer model", string engine = "0002")
        {
            Directory.CreateDirectory(Dir);
            File.WriteAllText(Path.Combine(Dir, "hdemucs.onnx"), pioneer);
            File.WriteAllText(Path.Combine(Dir, "demucs3_ver.txt"), engine + "\n");
            Ours = Path.Combine(Root, "ours.onnx");
            File.WriteAllText(Ours, "our model");
            OursSha = Files.Sha256(Ours)!;
            M = new Models(Dir, Orig, new[] { OursSha });
        }
        public string LiveSha => Files.Sha256(Path.Combine(Dir, "hdemucs.onnx"))!;
    }

    [Fact]
    public void InstallSavesPioneersThenRestorePutsItBack()
    {
        var b = new Bench();
        var pioneer = b.LiveSha;
        Assert.True(b.M.InstallOurs(b.Ours, b.OursSha).Ok);
        Assert.Equal(b.OursSha, b.LiveSha);
        Assert.True(b.M.SavedIsGood(pioneer));
        Assert.Equal("0002", File.ReadAllText(Path.Combine(b.Orig, pioneer + ".engine")).Trim());
        var r = b.M.Restore();
        Assert.True(r.Ok, r.Message);
        Assert.Equal(pioneer, b.LiveSha);
        Assert.True(b.M.Restore().Ok);   // already Pioneer's
    }

    [Fact]
    public void UnknownModelIsLeftAlone()
    {
        var b = new Bench();
        Assert.True(b.M.InstallOurs(b.Ours, b.OursSha).Ok);
        // a model that isn't ours, with the same STEMS Engine as the saved one: maybe a newer Demucs v4
        File.WriteAllText(Path.Combine(b.Dir, "hdemucs.onnx"), "something else");
        var r = b.M.InstallOurs(b.Ours, b.OursSha);
        Assert.False(r.Ok);
        Assert.Equal(Models.UnknownModelMessage, r.Message);
        // a new STEMS Engine: it is rekordbox's own, saved and replaced
        File.WriteAllText(Path.Combine(b.Dir, "demucs3_ver.txt"), "0003\n");
        Assert.True(b.M.InstallOurs(b.Ours, b.OursSha).Ok);
        Assert.Equal(2, b.M.SavedOriginals().Count);
        Assert.Equal("0003", b.M.SavedOriginals()[0].Engine);
    }

    [Fact]
    public void ModelChangedMeanwhileIsNotReplaced()
    {
        var b = new Bench();
        b.M.WillReplace = () => File.WriteAllText(Path.Combine(b.Dir, "hdemucs.onnx"), "rekordbox wrote this");
        var r = b.M.InstallOurs(b.Ours, b.OursSha);
        Assert.False(r.Ok);
        Assert.Equal("rekordbox wrote this", File.ReadAllText(Path.Combine(b.Dir, "hdemucs.onnx")));
        Assert.False(File.Exists(Path.Combine(b.Dir, ".hdemucs.new")));
    }

    [Fact]
    public void DamagedSaveIsSkipped()
    {
        var b = new Bench();
        var pioneer = b.LiveSha;
        Assert.True(b.M.InstallOurs(b.Ours, b.OursSha).Ok);
        File.WriteAllText(b.M.Saved(pioneer), "damaged");
        var r = b.M.Restore();
        Assert.False(r.Ok);
        Assert.Equal(b.OursSha, b.LiveSha);
    }
}

public class StateTests
{
    [Fact]
    public void BridgeStateParses()
    {
        var s = State.ParseBridgeState("pid=1234\nstart=1762560000\nlocation=E:\\RB Stems Plus Cache\nstate=unavailable\nreason=marker\n");
        Assert.NotNull(s);
        Assert.Equal(1234, s!.Pid);
        Assert.Equal("marker", s.Reason);
        Assert.Null(State.ParseBridgeState("pid=1\nstart=2\nstate=weird\n"));
        Assert.Null(State.ParseBridgeState("start=2\nstate=available\n"));
    }

    [Fact]
    public void BusyMarkerRoundTrip()
    {
        var p = AppPaths.ForTestRoot(Directory.CreateTempSubdirectory().FullName);
        State.SetBusy(p, new BusyAction("install", "Install"));
        var m = State.ReadBusy(p)!;
        Assert.Equal(Environment.ProcessId, m.Pid);
        Assert.Equal("install", m.Action!.Id);
        Assert.True(State.MarkerLive(m));
        Assert.False(State.MarkerLive(m with { Start = m.Start + 100 }));
        State.SetBusy(p, null);
        Assert.Null(State.ReadBusy(p));
    }

    [Fact]
    public void FreeFloor()
    {
        Assert.Equal(25_600_000_000, State.FreeFloor(256_000_000_000));
        Assert.Equal(State.FreeFloorMax, State.FreeFloor(2_000_000_000_000));
        Assert.Equal(State.FreeFloorMax, State.FreeFloor(null));
        Assert.Equal(26, State.FloorGB(25_600_000_000));
    }

    [Fact]
    public void ChosenAndDismissed()
    {
        var p = AppPaths.ForTestRoot(Directory.CreateTempSubdirectory().FullName);
        State.SetChosen(p, true, false);
        Assert.Equal(new Chosen(true, false), State.ReadChosen(p));
        State.Remember(p, new[] { "cache:7.2.14.0323" });
        Assert.Contains("cache:7.2.14.0323", State.Dismissed(p));
    }
}

public class RekordboxTests
{
    [Fact]
    public void CapabilitiesLine()
    {
        var d = Encoding.ASCII.GetBytes("xx\0rbstems bridge: \0rbstems-cache: rebuild=1 location=1 pioneer=" + new string('a', 64) + "," + new string('b', 64) + ";\0");
        Assert.True(Rekordbox.IsBridge(d));
        Assert.Equal(2, Rekordbox.BridgeCacheModels(d)!.Count);
        Assert.True(Rekordbox.BridgeSaysLocation(d));
        Assert.Null(Rekordbox.BridgeCacheModels(Encoding.ASCII.GetBytes("Pioneer's library")));
    }

    [Fact]
    public void VersionsListed()
    {
        Assert.True(Rekordbox.Listed("7.2.14.0323", new[] { "7.2.14" }));
        Assert.False(Rekordbox.Listed("7.2.140", new[] { "7.2.14" }));
        Assert.Equal("7.2.14", Rekordbox.ShortVersion("7.2.14.0323"));
    }

    [Fact]
    public void OrtVersionFromItsString()
    {
        var f = Path.Combine(Directory.CreateTempSubdirectory().FullName, "onnxruntime.dll");
        File.WriteAllBytes(f, Encoding.Latin1.GetBytes("MZ....\0Internal Build\0...\01.18.0\0...more\0"));
        Assert.Equal("1.18.0", Rekordbox.OrtVersion(f));
        var stock = Path.Combine(Fixtures.Repo, "build", "ort", "onnxruntime-win-x64-1.18.1", "lib", "onnxruntime.dll");
        if (File.Exists(stock)) Assert.Equal("1.18.1", Rekordbox.OrtVersion(stock));
    }

    [Fact]
    public void BuiltBridgeIsRecognised()
    {
        var dll = Path.Combine(Fixtures.Repo, "build", "bridge-win", "onnxruntime.dll");
        if (!File.Exists(dll)) return;                       // bridge/win/build.sh not run
        var d = File.ReadAllBytes(dll);
        Assert.True(Rekordbox.IsBridge(d));
        Assert.True(Rekordbox.BridgeSaysLocation(d));
        Assert.Contains("435c987855f7ea74ff5f20090541748cbbce5a2e8e501d07b10f4e703453215a", Rekordbox.BridgeCacheModels(d)!);
    }
}

public class LocationTests
{
    [Fact]
    public void PathRules()
    {
        Assert.Null(ConfigFile.CacheDirProblem(@"E:\RB Stems Plus Cache"));
        Assert.Null(ConfigFile.CacheDirProblem(@"E:\DJ's SSD\Sets #1\RB Stems Plus Cache"));
        Assert.NotNull(ConfigFile.CacheDirProblem(@"\\server\share\RB Stems Plus Cache"));
        Assert.NotNull(ConfigFile.CacheDirProblem(@"E:\GL?S\RB Stems Plus Cache"));
        Assert.NotNull(ConfigFile.CacheDirProblem(@"E:\RB Stems Plus Cache.noindex"));
    }

    [Fact]
    public void CreateMakesAFolderWithAMarker()
    {
        var parent = Directory.CreateTempSubdirectory().FullName;
        var (dir, id, err) = CacheLocation.Create(parent);
        if (!OperatingSystem.IsWindows()) { Assert.NotNull(err); return; }   // a drive letter's path only
        Assert.Null(err);
        Assert.Equal(id, CacheLocation.ReadMarker(dir!));
    }

    [Fact]
    public void EntriesSizeCountsOnlyTheBridgesNames()
    {
        var c = Directory.CreateTempSubdirectory().FullName;
        var x = Path.Combine(c, new string('a', 64), "ab");
        Directory.CreateDirectory(x);
        File.WriteAllBytes(Path.Combine(x, "ab" + new string('0', 62) + ".flac"), new byte[1000]);
        File.WriteAllBytes(Path.Combine(x, "other.txt"), new byte[5000]);
        File.WriteAllBytes(Path.Combine(c, "notes.txt"), new byte[5000]);
        Assert.InRange(CacheLocation.EntriesSize(c)!.Value, 1000, 4096 * 2);
        Assert.Equal(0, CacheLocation.EntriesSize(Path.Combine(c, "missing")));
    }
}
