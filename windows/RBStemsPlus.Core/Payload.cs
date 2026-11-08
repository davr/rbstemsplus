// The Windows payload: what the app downloads from the release, described by payload-windows.json
// (the same schema as the macOS payload.json: app/Sources/Payload.swift) and its signature
// payload-windows.json.sig (ECDSA P-256 / SHA-256, DER, over the exact bytes), checked against the
// compiled-in keys (keys/*.pub.pem). One older than the newest accepted on this PC, or than the
// app itself, is refused; a key given twice in any object is refused, signed or not.
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace RBStemsPlus;

public sealed record TrustedKey(string Name, ECDsa Key)
{
    /// From the base64 of the key's DER (SubjectPublicKeyInfo, a .pub.pem without its armour);
    /// null unless it is a P-256 public key.
    public static TrustedKey? FromBase64(string name, string der)
    {
        try
        {
            var k = ECDsa.Create();
            k.ImportSubjectPublicKeyInfo(Convert.FromBase64String(der), out _);
            if (k.KeySize != 256) return null;
            return new TrustedKey(name, k);
        }
        catch { return null; }
    }

    public static TrustedKey? FromPem(string name, string pem)
    {
        var b64 = string.Concat(pem.Split('\n').Select(l => l.Trim()).Where(l => l.Length > 0 && !l.StartsWith("-----")));
        return FromBase64(name, b64);
    }
}

public static class Signing
{
    /// The name of the key that made `signature` (DER, as openssl writes it) over `data`, or null.
    public static string? Signer(byte[] data, byte[] signature, IEnumerable<TrustedKey> keys)
    {
        if (signature.Length == 0) return null;
        foreach (var k in keys)
        {
            try { if (k.Key.VerifyData(data, signature, HashAlgorithmName.SHA256, DSASignatureFormat.Rfc3279DerSequence)) return k.Name; }
            catch (CryptographicException) { }
        }
        return null;
    }

    /// The keys compiled into this app (Keys.g.cs, written by scripts/build-windows.sh).
    public static List<TrustedKey> Compiled => CompiledKeys.All.Select(k => TrustedKey.FromBase64(k.Name, k.Der)).OfType<TrustedKey>().ToList();
}

public sealed class Manifest
{
    public sealed record App(string Version, string Zip, string Sha256);
    public sealed record FileRef(string File, string Sha256, long? Size);
    public sealed record Compatible(List<string> Rekordbox, string OrtVersionPrefix, List<string> StemsEngine);

    public string PayloadVersion = "";
    public App AppInfo = new("", "", "");
    public FileRef Bridge = new("", "", null);
    public FileRef Model = new("", "", null);
    /// The fixed-shape DirectML copy of the model (experimental); null when the payload has none.
    public FileRef? ModelDml;
    public Compatible Compat = new(new(), "", new());
    public List<string>? PreviousModels;
    /// The compiled-in key that signed it; set only by Payload.Check.
    public string? SignedBy { get; internal set; }

    public string Dir(AppPaths p) => Path.Combine(p.PayloadRoot, PayloadVersion);
    public string ModelPath(AppPaths p) => Path.Combine(Dir(p), "model.onnx");
    public string ModelDmlPath(AppPaths p) => Path.Combine(Dir(p), "model-dml.onnx");
    public string BridgePath(AppPaths p) => Path.Combine(Dir(p), "onnxruntime.dll");

    static bool Plain(string s) => s.Length > 0 && s != "." && s != ".." && s.All(c => c < 128 && (char.IsLetterOrDigit(c) || "._-".Contains(c)));

    public bool Valid =>
        new[] { PayloadVersion, AppInfo.Version, AppInfo.Zip, Bridge.File, Model.File }.All(Plain)
        && new[] { AppInfo.Sha256, Bridge.Sha256, Model.Sha256 }.Concat(PreviousModels ?? new()).All(Files.IsSha256)
        && (Model.Size ?? 0) > 0
        && (ModelDml == null || (Plain(ModelDml.File) && Files.IsSha256(ModelDml.Sha256) && (ModelDml.Size ?? 0) > 0));

    /// Parses the JSON (keys must pass PlainJsonKeys first); unknown keys are ignored.
    public static Manifest? Decode(byte[] data)
    {
        if (!Payload.PlainJsonKeys(data)) return null;
        try
        {
            using var doc = JsonDocument.Parse(data);
            var r = doc.RootElement;
            static string S(JsonElement e, string k) => e.GetProperty(k).GetString() ?? throw new FormatException(k);
            static List<string> L(JsonElement e, string k) => e.GetProperty(k).EnumerateArray().Select(x => x.GetString() ?? throw new FormatException(k)).ToList();
            static FileRef F(JsonElement e) => new(S(e, "file"), S(e, "sha256"), e.TryGetProperty("size", out var z) && z.ValueKind == JsonValueKind.Number ? z.GetInt64() : null);
            var a = r.GetProperty("app"); var c = r.GetProperty("compatible");
            var m = new Manifest
            {
                PayloadVersion = S(r, "payload_version"),
                AppInfo = new App(S(a, "version"), S(a, "zip"), S(a, "sha256")),
                Bridge = F(r.GetProperty("bridge")),
                Model = F(r.GetProperty("model")),
                ModelDml = r.TryGetProperty("model_dml", out var md) && md.ValueKind == JsonValueKind.Object ? F(md) : null,
                Compat = new Compatible(L(c, "rekordbox"), S(c, "ort_version_prefix"), L(c, "stems_engine")),
                PreviousModels = r.TryGetProperty("previous_models", out var pm) ? pm.EnumerateArray().Select(x => x.GetString() ?? "").ToList() : null,
            };
            return m.Valid ? m : null;
        }
        catch { return null; }
    }
}

public enum PayloadRefusal { None, NoSignature, BadSignature, NotValid, NotSaved, Older }

public sealed class PayloadFiles
{
    public string Json, Signature, Floor;
    public List<TrustedKey> Keys;
    public string Minimum;
    public PayloadFiles(AppPaths p, List<TrustedKey>? keys = null, string? minimum = null)
    {
        Json = p.ManifestFile; Signature = p.ManifestSigFile; Floor = p.PayloadFloorFile;
        Keys = keys ?? Signing.Compiled; Minimum = minimum ?? AppVersion.Current;
    }
}

public static class Payload
{
    /// The compatibility list used until a payload-windows.json has been downloaded: rekordbox 7.2.14
    /// (the version tested on Windows), ONNX Runtime 1.18 (Pioneer's is 1.18.0, DirectML), STEMS
    /// Engine 0002 (the same model as on macOS).
    public static readonly Manifest.Compatible BuiltIn = new(new() { "7.2.14" }, "1.18.", new() { "0002" });

    /// Whether `data` is one JSON object in which no object has the same key twice, and every key is
    /// printable ASCII without escapes (Payload.swift's plainJSONKeys, the same check).
    public static bool PlainJsonKeys(byte[] b)
    {
        int i = 0;
        void Ws() { while (i < b.Length && (b[i] == 0x20 || b[i] == 0x09 || b[i] == 0x0a || b[i] == 0x0d)) i++; }
        bool At(byte c) => i < b.Length && b[i] == c;
        string? Str()
        {
            if (!At(0x22)) return null;
            i++;
            int start = i;
            while (i < b.Length)
            {
                if (b[i] == 0x22) { i++; return Encoding.Latin1.GetString(b, start, i - 1 - start); }
                if (b[i] == 0x5c) { i += 2; continue; }
                if (b[i] < 0x20) return null;
                i++;
            }
            return null;
        }
        bool Value(int depth)
        {
            Ws();
            if (depth >= 32 || i >= b.Length) return false;
            switch (b[i])
            {
                case 0x7b:
                    i++; Ws();
                    if (At(0x7d)) { i++; return true; }
                    var keys = new HashSet<string>(StringComparer.Ordinal);
                    while (true)
                    {
                        Ws();
                        var k = Str();
                        if (k == null || k.Length == 0 || k.Any(c => c < 0x20 || c >= 0x7f || c == '\\') || !keys.Add(k)) return false;
                        Ws();
                        if (!At(0x3a)) return false;
                        i++;
                        if (!Value(depth + 1)) return false;
                        Ws();
                        if (At(0x2c)) { i++; continue; }
                        if (At(0x7d)) { i++; return true; }
                        return false;
                    }
                case 0x5b:
                    i++; Ws();
                    if (At(0x5d)) { i++; return true; }
                    while (true)
                    {
                        if (!Value(depth + 1)) return false;
                        Ws();
                        if (At(0x2c)) { i++; continue; }
                        if (At(0x5d)) { i++; return true; }
                        return false;
                    }
                case 0x22: return Str() != null;
                default:
                    int s0 = i;
                    while (i < b.Length && "+-.0123456789Eaeflnrstu".IndexOf((char)b[i]) >= 0) i++;
                    return i > s0;
            }
        }
        Ws();
        if (!At(0x7b) || !Value(0)) return false;
        Ws();
        return i == b.Length;
    }

    /// Whether payload version a is older than b ("1.0.9" < "1.0.10").
    public static bool Older(string a, string b) => CompareVersions(a, b) < 0;

    public static int CompareVersions(string a, string b)
    {
        // numeric runs compare as numbers, everything else as text (NSString's .numeric)
        int i = 0, j = 0;
        while (i < a.Length && j < b.Length)
        {
            if (char.IsDigit(a[i]) && char.IsDigit(b[j]))
            {
                int si = i, sj = j;
                while (i < a.Length && char.IsDigit(a[i])) i++;
                while (j < b.Length && char.IsDigit(b[j])) j++;
                var x = a[si..i].TrimStart('0'); var y = b[sj..j].TrimStart('0');
                if (x.Length != y.Length) return x.Length.CompareTo(y.Length);
                int c = string.CompareOrdinal(x, y);
                if (c != 0) return c;
            }
            else
            {
                if (a[i] != b[j]) return a[i].CompareTo(b[j]);
                i++; j++;
            }
        }
        return (a.Length - i).CompareTo(b.Length - j);
    }

    public static string? Floor(PayloadFiles f)
    {
        var v = Files.ReadText(f.Floor)?.Trim();
        return !string.IsNullOrEmpty(v) && v.Length < 64 && v.All(c => c < 128 && (char.IsLetterOrDigit(c) || "._-".Contains(c))) ? v : null;
    }

    public static string? AcceptedFloor(PayloadFiles f)
    {
        var min = string.IsNullOrEmpty(f.Minimum) ? null : f.Minimum;
        var saved = Floor(f);
        if (saved == null) return min;
        if (min == null) return saved;
        return Older(saved, min) ? min : saved;
    }

    /// The bytes checked: signed by one of `keys`, valid, not older than `floor`.
    public static (Manifest? M, PayloadRefusal Why) Check(byte[] data, byte[]? signature, List<TrustedKey> keys, string? floor)
    {
        if (signature == null || signature.Length == 0) return (null, PayloadRefusal.NoSignature);
        var who = Signing.Signer(data, signature, keys);
        if (who == null) return (null, PayloadRefusal.BadSignature);
        var m = Manifest.Decode(data);
        if (m == null) return (null, PayloadRefusal.NotValid);
        if (floor != null && Older(m.PayloadVersion, floor)) return (null, PayloadRefusal.Older);
        m.SignedBy = who;
        return (m, PayloadRefusal.None);
    }

    public static string Describe(PayloadRefusal r) => r switch
    {
        PayloadRefusal.NoSignature => "it has no signature",
        PayloadRefusal.BadSignature => "its signature doesn't verify with RB Stems Plus's keys",
        PayloadRefusal.NotValid => "it isn't a valid payload description",
        PayloadRefusal.NotSaved => "it couldn't be saved",
        PayloadRefusal.Older => "it is older than the oldest this app accepts now",
        _ => "",
    };

    static byte[]? ReadBytes(string p) { try { return File.ReadAllBytes(p); } catch { return null; } }

    static readonly object refusedGate = new();
    static string? lastRefused;

    /// The last good payload, checked again now (null if none, or not valid any more).
    public static Manifest? Saved(PayloadFiles f, Action<string>? log = null)
    {
        var data = ReadBytes(f.Json);
        if (data == null) return null;
        var (m, why) = Check(data, ReadBytes(f.Signature), f.Keys, AcceptedFloor(f));
        if (m == null)
        {
            // said once per reason and process: the window reads it every few seconds
            var line = "saved " + Consts.ManifestName + " ignored: " + Describe(why) + " (the next download replaces it)";
            lock (refusedGate) { if (line == lastRefused) return null; lastRefused = line; }
            log?.Invoke(line);
        }
        return m;
    }

    /// Checks a downloaded payload and, if it counts, saves it (signature first) and raises the floor.
    public static (Manifest? M, PayloadRefusal Why) Accept(byte[] data, byte[] signature, PayloadFiles f)
    {
        var floor = Floor(f);
        var r = Check(data, signature, f.Keys, AcceptedFloor(f));
        if (r.M == null) return r;
        if (!Files.TryWriteAtomically(f.Signature, signature) || !Files.TryWriteAtomically(f.Json, data)) return (null, PayloadRefusal.NotSaved);
        if (floor == null || Older(floor, r.M.PayloadVersion)) Files.TryWriteText(f.Floor, r.M.PayloadVersion + "\n");
        return r;
    }

    public static readonly HttpClient Http = MakeClient();

    static HttpClient MakeClient()
    {
        var h = new HttpClientHandler { AllowAutoRedirect = false, AutomaticDecompression = DecompressionMethods.None };
        return new HttpClient(new HttpsOnlyRedirects(h)) { Timeout = Timeout.InfiniteTimeSpan };
    }

    /// Follows redirects only to https (a test build: http too), like curl --proto-redir =https.
    sealed class HttpsOnlyRedirects : DelegatingHandler
    {
        public HttpsOnlyRedirects(HttpMessageHandler inner) : base(inner) { }
        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage req, CancellationToken ct)
        {
            for (int hops = 0; ; hops++)
            {
                if (!AllowedScheme(req.RequestUri!)) throw new HttpRequestException("refused a non-HTTPS address: " + req.RequestUri);
                var resp = await base.SendAsync(req, ct).ConfigureAwait(false);
                int code = (int)resp.StatusCode;
                if (code is < 300 or >= 400 || resp.Headers.Location == null || hops >= 10) return resp;
                var next = resp.Headers.Location.IsAbsoluteUri ? resp.Headers.Location : new Uri(req.RequestUri!, resp.Headers.Location);
                var copy = new HttpRequestMessage(req.Method, next);
                foreach (var hd in req.Headers) copy.Headers.TryAddWithoutValidation(hd.Key, hd.Value);
                resp.Dispose();
                req = copy;
            }
        }
    }

    public static bool AllowedScheme(Uri u) =>
#if RBSTEMSPLUS_TEST_BUILD
        u.Scheme is "https" or "http";
#else
        u.Scheme == "https";
#endif

    public enum Fetch { Ok, Offline, Unverified }

    /// Downloads the payload and its signature; keeps them if they count.
    public static (Fetch Result, Manifest? M) FetchChecked(AppPaths p, PayloadFiles f, Action<string> log, string? baseUrl = null)
    {
        baseUrl ??= Consts.DownloadBase;
        byte[] data, sig;
        try
        {
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(60));
            data = Http.GetByteArrayAsync(baseUrl + Consts.ManifestName, cts.Token).GetAwaiter().GetResult();
        }
        catch (Exception e) { log($"{Consts.ManifestName}: couldn't download ({e.Message})"); return (Fetch.Offline, null); }
        try
        {
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(60));
            sig = Http.GetByteArrayAsync(baseUrl + Consts.ManifestSigName, cts.Token).GetAwaiter().GetResult();
        }
        catch (HttpRequestException e) when (e.StatusCode != null) { log($"{Consts.ManifestSigName}: not published ({e.StatusCode})"); return (Fetch.Unverified, null); }
        catch (Exception e) { log($"{Consts.ManifestSigName}: couldn't download ({e.Message})"); return (Fetch.Offline, null); }
        var (m, why) = Accept(data, sig, f);
        if (m == null)
        {
            log($"{Consts.ManifestName}: REFUSED, {Describe(why)}: nothing changed, the last good copy stays");
            return (why == PayloadRefusal.NotSaved ? Fetch.Offline : Fetch.Unverified, null);
        }
        log($"{Consts.ManifestName}: payload {m.PayloadVersion}, app {m.AppInfo.Version}, rekordbox {string.Join("/", m.Compat.Rekordbox)}, engine {string.Join("/", m.Compat.StemsEngine)}, signed with the {m.SignedBy} key");
        return (Fetch.Ok, m);
    }

    public enum Fetched { Ok, Offline, Damaged, NoSpace }

    /// Makes sure `dest` holds the release asset `name` with this sha256 (and size): kept if it is
    /// there, reused from another payload folder, or downloaded (resuming a partial file) and
    /// verified; a damaged download is fetched once more from scratch, then refused.
    /// `progress`: the percent downloaded, or null while a file is checked (no percentage).
    public static Fetched Asset(AppPaths p, string name, string dest, string sha, long? size, string what,
                                Action<string> status, Action<string> log, string? baseUrl = null, Action<int?>? progress = null)
    {
        progress ??= _ => { };
        baseUrl ??= Consts.DownloadBase;
        Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
        status($"Checking {what}…");
        progress(null);
        if (Files.Verified(dest, sha, size)) { log($"{name}: already downloaded and verified"); return Fetched.Ok; }
        Files.Remove(dest);
        try
        {
            foreach (var other in Directory.EnumerateDirectories(p.PayloadRoot))
            {
                var cand = Path.Combine(other, Path.GetFileName(dest));
                if (Path.GetDirectoryName(dest) == other || !Files.Verified(cand, sha, size)) continue;
                if (Files.Copy(cand, dest) == null) { log($"{name}: reused from payload {Path.GetFileName(other)}"); return Fetched.Ok; }
            }
        }
        catch { }
        var part = dest + ".part";
        for (int attempt = 1; attempt <= 2; attempt++)
        {
            long have = Files.Size(part) ?? 0;
            bool complete = attempt == 1 && size != null && have == size;
            bool resume = attempt == 1 && have > 0 && !complete && (size == null || have < size);
            if (!complete && !resume) Files.Remove(part);
            if (!complete)
            {
                status($"Downloading {what}…");
                progress(size is > 0 ? (int)(have * 100 / size.Value) : null);
                try
                {
                    var req = new HttpRequestMessage(HttpMethod.Get, baseUrl + name);
                    if (resume) req.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(have, null);
                    using var resp = Http.SendAsync(req, HttpCompletionOption.ResponseHeadersRead).GetAwaiter().GetResult();
                    if (resume && resp.StatusCode != HttpStatusCode.PartialContent) { Files.Remove(part); have = 0; resume = false; }
                    if (!resp.IsSuccessStatusCode && resp.StatusCode != HttpStatusCode.PartialContent)
                    {
                        log($"{name}: download failed (HTTP {(int)resp.StatusCode})");
                        return Fetched.Offline;
                    }
                    using var src = resp.Content.ReadAsStream();
                    using var dst = new FileStream(part, resume ? FileMode.Append : FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20);
                    var buf = new byte[1 << 20];
                    long got = have;
                    long total = size ?? (resp.Content.Headers.ContentLength is { } cl ? have + cl : 0);
                    var lastShown = DateTime.MinValue;
                    var started = DateTime.UtcNow;
                    int n;
                    while ((n = src.Read(buf, 0, buf.Length)) > 0)
                    {
                        dst.Write(buf, 0, n);
                        got += n;
                        if (DateTime.UtcNow - lastShown > TimeSpan.FromMilliseconds(250))
                        {
                            double secs = Math.Max(0.001, (DateTime.UtcNow - started).TotalSeconds);
                            var speed = $"{(got - have) / secs / 1_048_576:0.0} MB/s";
                            if (total > 0)
                            {
                                int pct = (int)Math.Min(99, got * 100 / total);
                                status($"Downloading {what}… {got / 1_048_576.0:0} of {total / 1_048_576.0:0} MB ({pct}%, {speed})");
                                progress(pct);
                            }
                            else { status($"Downloading {what}… {got / 1_048_576.0:0} MB ({speed})"); progress(null); }
                            lastShown = DateTime.UtcNow;
                        }
                    }
                    dst.Flush(true);
                }
                catch (IOException e) when (Files.IsDiskFull(e)) { log($"{name}: the disk is full"); return Fetched.NoSpace; }
                catch (Exception e)
                {
                    log($"{name}: download failed ({e.Message}{(resume ? ", resuming" : "")})");
                    if (resume) continue;
                    return Fetched.Offline;
                }
            }
            status($"Checking {what}…");
            progress(null);
            if (Files.Verified(part, sha, size))
            {
                try { File.Move(part, dest, true); log($"{name}: downloaded and verified"); return Fetched.Ok; } catch { }
            }
            log($"{name}: checksum or size didn't match ({Files.Size(part) ?? 0} bytes): {(attempt == 1 ? "downloading again" : "refused")}");
            Files.Remove(part);
        }
        return Fetched.Damaged;
    }

    /// Deletes the folders of other payload versions once this one's files are here.
    public static void Prune(AppPaths p, Manifest keep)
    {
        try
        {
            foreach (var d in Directory.EnumerateDirectories(p.PayloadRoot))
                if (Path.GetFileName(d) != keep.PayloadVersion && !Files.IsReparsePoint(d)) Directory.Delete(d, true);
        }
        catch { }
    }
}

/// The app's version (from the assembly, written from VERSION by scripts/build-windows.sh).
public static class AppVersion
{
    public static string Current =>
        typeof(AppVersion).Assembly.GetCustomAttributes(typeof(System.Reflection.AssemblyInformationalVersionAttribute), false)
            .OfType<System.Reflection.AssemblyInformationalVersionAttribute>().FirstOrDefault()?.InformationalVersion?.Split('+')[0] ?? "0";
}
