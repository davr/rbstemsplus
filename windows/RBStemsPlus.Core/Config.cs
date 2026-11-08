// config.ini in %APPDATA%\rbstemsplus: the Stems Cache settings the bridge reads when rekordbox
// starts. One grammar, the same as the bridge's (read_config in bridge/win/rbstems_bridge_win.c)
// and the macOS app's (app/Sources/Config.swift), read as bytes:
// - the whole file: over 64 KB, a NUL byte or invalid UTF-8 make it unreadable; missing: defaults;
// - a UTF-8 BOM only at offset 0; lines end at "\n" and hold at most 1024 bytes (a CR counts);
// - leading spaces, tabs and CRs are skipped; an empty line or one starting with '#' is a comment;
// - "[name]": a section; ours at the top and in [cache] (case and surrounding blanks ignored);
//   a section line over the limit leaves an unknown section;
// - key=value: the key before the first '=', trimmed, case ignored; the value quoted ("..." or
//   '...') or up to the first space, tab, CR or '#'. An over-long line keeps its key, not its value.
// The Windows path rule for cache_dir: CacheDirProblem below (the bridge's location_path_ok).
using System.Globalization;
using System.Text;

namespace RBStemsPlus;

public sealed record Config(bool Enabled = true, bool RekordboxModel = false, double MaxGB = 20, double MaxDays = 60);

/// Where config.ini says the stems are saved.
public abstract record ConfigLocation
{
    public sealed record ThisPC : ConfigLocation;
    public sealed record Chosen(string Dir, string Id) : ConfigLocation;
    public sealed record Malformed(string Why) : ConfigLocation;
}

public sealed class ConfigState
{
    public Config Config { get; set; } = new();
    public ConfigLocation Location { get; set; } = new ConfigLocation.ThisPC();
    /// why the file can't be read (it exists); null when it was read or isn't there
    public string? Unreadable { get; set; }
}

public sealed class ConfigLine
{
    public string Text = "";
    public bool Section, Ours, OverLong;
    public string? Key, Value;
}

public static class ConfigFile
{
    public const int MaxBytes = 65_536;
    public const int LineMax = 1024;
    public static readonly (double Lo, double Hi) MaxGBRange = (0.1, 100_000), MaxDaysRange = (1, 36_500);
    static readonly UTF8Encoding Strict = new(false, true);

    /// The file's bytes, or why not; (null, null): it doesn't exist.
    public static (byte[]? Bytes, string? Unreadable) ReadBytes(string file)
    {
        byte[] b;
        try
        {
            var fi = new FileInfo(file);
            if (!fi.Exists) return Directory.Exists(file) ? (null, "a folder") : (null, null);
            if (fi.Length > MaxBytes) return (null, "over 64 KB");
            b = File.ReadAllBytes(file);
        }
        catch (FileNotFoundException) { return (null, null); }
        catch (DirectoryNotFoundException) { return (null, null); }
        catch (Exception e) { return (null, e.Message); }
        if (b.Length > MaxBytes) return (null, "over 64 KB");
        if (Array.IndexOf(b, (byte)0) >= 0) return (null, "a NUL byte");
        if (!IsStrictUtf8(b)) return (null, "not UTF-8");
        return (b, null);
    }

    public static bool IsStrictUtf8(byte[] b)
    {
        try { Strict.GetString(b); return true; } catch (DecoderFallbackException) { return false; }
    }

    static bool Blank(byte c) => c == 0x20 || c == 0x09 || c == 0x0D;

    static ReadOnlySpan<byte> Trim(ReadOnlySpan<byte> s, bool cr = true)
    {
        static bool T(byte c, bool cr) => c == 0x20 || c == 0x09 || (cr && c == 0x0D);
        int a = 0, z = s.Length;
        while (a < z && T(s[a], cr)) a++;
        while (z > a && T(s[z - 1], cr)) z--;
        return s[a..z];
    }

    static string Lower(ReadOnlySpan<byte> s)
    {
        var b = s.ToArray();
        for (int i = 0; i < b.Length; i++) if (b[i] >= 0x41 && b[i] <= 0x5A) b[i] += 32;
        return Encoding.UTF8.GetString(b);
    }

    static string Value(ReadOnlySpan<byte> s)
    {
        int i = 0;
        while (i < s.Length && (s[i] == 0x20 || s[i] == 0x09)) i++;
        var v = s[i..];
        if (v.Length > 0 && (v[0] == 0x22 || v[0] == 0x27))
        {
            int end = v[1..].IndexOf(v[0]);
            if (end >= 0) return Encoding.UTF8.GetString(v.Slice(1, end));
        }
        int n = 0;
        while (n < v.Length && !(v[n] == 0x20 || v[n] == 0x09 || v[n] == 0x0D || v[n] == 0x23)) n++;
        return Encoding.UTF8.GetString(v[..n]);
    }

    /// The lines of a readable file, and whether a section line was over the limit.
    public static (List<ConfigLine> Lines, bool OverlongSection) Lines(byte[] bytes)
    {
        bool ours = true, overlongSection = false;
        var outl = new List<ConfigLine>();
        if (bytes.Length == 0) return (outl, false);
        int start = 0, index = 0;
        while (start <= bytes.Length)
        {
            int nl = Array.IndexOf(bytes, (byte)0x0A, start);
            int end = nl < 0 ? bytes.Length : nl;
            var raw = new ReadOnlySpan<byte>(bytes, start, end - start);
            var line = new ConfigLine { Text = Encoding.UTF8.GetString(raw) };
            var s = raw;
            if (index == 0 && s.StartsWith(new byte[] { 0xEF, 0xBB, 0xBF })) s = s[3..];
            line.OverLong = s.Length > LineMax;
            int k = 0;
            while (k < s.Length && Blank(s[k])) k++;
            s = s[k..];
            if (s.Length == 0 || s[0] == 0x23) outl.Add(line);
            else if (s[0] == 0x5B)
            {
                line.Section = true;
                if (line.OverLong) { ours = false; overlongSection = true; }
                else
                {
                    int close = s.IndexOf((byte)0x5D);
                    ours = close >= 0 && Lower(Trim(s[1..close], cr: false)) == "cache";
                }
                outl.Add(line);
            }
            else
            {
                line.Ours = ours;
                int eq = s.IndexOf((byte)0x3D);
                if (eq >= 0)
                {
                    line.Key = Lower(Trim(s[..eq]));
                    if (!line.OverLong) line.Value = Value(s[(eq + 1)..]);
                }
                outl.Add(line);
            }
            if (nl < 0) break;
            start = nl + 1;
            index++;
        }
        return (outl, overlongSection);
    }

    static bool ParseNumber(string v, out double x) =>
        double.TryParse(v, NumberStyles.AllowDecimalPoint | NumberStyles.AllowExponent | NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out x)
        && v.Length > 0 && !char.IsWhiteSpace(v[0]);

    /// Reads config.ini by the grammar. `log` gets a line per ignored value.
    public static ConfigState Read(string file, Action<string>? log = null)
    {
        log ??= _ => { };
        var st = new ConfigState();
        var (bytes, why) = ReadBytes(file);
        if (why != null) { st.Unreadable = why; st.Location = new ConfigLocation.Malformed($"config.ini can't be read ({why})"); return st; }
        if (bytes == null) return st;
        var (lines, overlong) = Lines(bytes);
        var dirs = new List<string?>(); var ids = new List<string?>();
        bool anyLocation = false;
        var cfg = st.Config;
        foreach (var l in lines)
        {
            if (l.Key is not { } key) continue;
            if (key is "cache_dir" or "cache_id") anyLocation = true;
            if (!l.Ours) continue;
            if (l.Value is not { } value)
            {
                if (key == "cache_dir") dirs.Add(null);
                else if (key == "cache_id") ids.Add(null);
                else if (key is "enabled" or "rekordbox_model" or "max_gb" or "max_days") log($"config.ini: ignored {key} (a line over {LineMax} bytes)");
                continue;
            }
            switch (key)
            {
                case "enabled":
                case "rekordbox_model":
                    bool? on = value.ToLowerInvariant() switch { "1" or "yes" or "on" or "true" => true, "0" or "no" or "off" or "false" => false, _ => null };
                    if (on == null) { log($"config.ini: ignored {key}={value} (allowed 1 or 0)"); break; }
                    cfg = key == "enabled" ? cfg with { Enabled = on.Value } : cfg with { RekordboxModel = on.Value };
                    break;
                case "max_gb":
                case "max_days":
                    var range = key == "max_gb" ? MaxGBRange : MaxDaysRange;
                    if (ParseNumber(value, out var x) && x >= range.Lo && x <= range.Hi)
                        cfg = key == "max_gb" ? cfg with { MaxGB = x } : cfg with { MaxDays = x };
                    else log($"config.ini: ignored {key}={value} (allowed {Number(range.Lo)} to {Number(range.Hi)})");
                    break;
                case "cache_dir": dirs.Add(value); break;
                case "cache_id": ids.Add(value); break;
            }
        }
        st.Config = cfg;
        st.Location = Location(dirs, ids, overlong, anyLocation);
        if (st.Location is ConfigLocation.Malformed m) log($"config.ini: where stems are saved is set wrongly ({m.Why}): Stems Cache saves nothing");
        return st;
    }

    public static ConfigLocation Location(List<string?> dirs, List<string?> ids, bool overlongSection, bool anyLocationKey)
    {
        if (overlongSection && anyLocationKey) return new ConfigLocation.Malformed($"a section line over {LineMax} bytes");
        if (dirs.Count == 0 && ids.Count == 0) return new ConfigLocation.ThisPC();
        if (dirs.Count == 0) return new ConfigLocation.Malformed("cache_id without cache_dir");
        if (ids.Count == 0) return new ConfigLocation.Malformed("cache_dir without cache_id");
        if (dirs.Contains(null) || ids.Contains(null)) return new ConfigLocation.Malformed($"a line over {LineMax} bytes");
        if (dirs.Distinct(StringComparer.Ordinal).Count() > 1) return new ConfigLocation.Malformed("cache_dir set twice, differently");
        if (ids.Select(i => i!.ToLowerInvariant()).Distinct().Count() > 1) return new ConfigLocation.Malformed("cache_id set twice, differently");
        if (CacheDirProblem(dirs[0]!) is { } why) return new ConfigLocation.Malformed("cache_dir: " + why);
        if (!IsUuid(ids[0]!)) return new ConfigLocation.Malformed("cache_id isn't an id");
        return new ConfigLocation.Chosen(dirs[0]!, ids[0]!);
    }

    /// Why a chosen location's path can't be used, or null: a drive letter's absolute path
    /// ("E:\..."), at most 700 bytes of UTF-8, plain components (none empty, "." or "..", none
    /// ending in a dot or a space), none of <>:"/|?* or control characters, ending in our folder's
    /// name; a drive's root holds it fine ("E:\RB Stems Plus Cache"). The bridge's location_path_ok.
    public static string? CacheDirProblem(string dir)
    {
        if (dir.Length < 3 || !(dir[0] is >= 'A' and <= 'Z' or >= 'a' and <= 'z') || dir[1] != ':' || dir[2] != '\\') return "not a drive letter's absolute path";
        if (Encoding.UTF8.GetByteCount(dir) > Consts.CacheDirMax) return $"over {Consts.CacheDirMax} bytes";
        foreach (var c in dir[3..])
            if (c < 0x20 || c == 0x7F || "<>:\"/|?*".Contains(c)) return "a character Windows doesn't allow in names";
        var parts = dir[3..].Split('\\');
        foreach (var p in parts)
            if (p.Length == 0 || p == "." || p == ".." || p.EndsWith('.') || p.EndsWith(' ')) return "an empty, \".\" or \"..\" component, or one ending in a dot or a space";
        if (parts[^1] != Consts.LocationFolderName) return $"not a folder named {Consts.LocationFolderName}";
        return null;
    }

    public static bool IsUuid(string s)
    {
        var p = s.Split('-');
        return p.Length == 5 && p.Select(x => x.Length).SequenceEqual(new[] { 8, 4, 4, 4, 12 }) && p.All(x => x.All(Uri.IsHexDigit));
    }

    public static string Number(double x) => x == Math.Round(x) ? ((long)x).ToString(CultureInfo.InvariantCulture) : x.ToString(CultureInfo.InvariantCulture);

    public static bool Sets(string file, string key)
    {
        var (b, _) = ReadBytes(file);
        return b != null && Lines(b).Lines.Any(l => l.Ours && l.Key == key);
    }

    public static bool WriteLimits(string file, double maxGB, double maxDays) =>
        WriteValues(file, new() { ("max_gb", Number(maxGB)), ("max_days", Number(maxDays)) });

    public static bool WriteRekordboxModel(string file, bool on) => WriteValues(file, new() { ("rekordbox_model", on ? "1" : "0") });

    /// Where the stems are saved: our folder `dir` (double-quoted) and the marker's `id`; null dir:
    /// this PC. The write counts only if the file reads back as meant.
    public static bool WriteLocation(string file, string? dir, string? id)
    {
        if (dir == null || id == null)
            return WriteValues(file, new(), remove: new[] { "cache_dir", "cache_id" }) && Read(file).Location is ConfigLocation.ThisPC;
        if (CacheDirProblem(dir) != null || !IsUuid(id)) return false;
        return WriteValues(file, new() { ("cache_dir", "\"" + dir + "\""), ("cache_id", id) })
               && Read(file).Location is ConfigLocation.Chosen c && c.Dir == dir && c.Id == id;
    }

    /// Writes these keys, keeping every other line as it is: each key replaced where it is (every
    /// line of it, in our scope) or added before the first section; keys in `remove` lose every line
    /// of theirs in our scope. Never over a file that exists but can't be read. A BOM stays first.
    public static bool WriteValues(string file, List<(string Key, string Value)> values, string[]? remove = null)
    {
        remove ??= Array.Empty<string>();
        var (bytes, why) = ReadBytes(file);
        if (why != null) return false;
        var locationKeys = new HashSet<string> { "cache_dir", "cache_id" };
        bool touchesLocation = values.Any(v => locationKeys.Contains(v.Key)) || remove.Any(locationKeys.Contains);
        var lines = new List<string>();
        var done = new HashSet<string>();
        int? firstSection = null;
        bool bom = false;
        if (bytes != null)
        {
            var (parsed, overlong) = Lines(bytes);
            for (int i = 0; i < parsed.Count; i++)
            {
                var l = parsed[i];
                var text = l.Text;
                if (i == 0 && text.StartsWith('\uFEFF')) { text = text[1..]; bom = true; }
                if (l.Section && firstSection == null) firstSection = lines.Count;
                if (touchesLocation && overlong)
                {
                    if (l.Section && l.OverLong) { lines.Add("[unreadable-section]"); continue; }
                    if (!l.Ours && l.Key != null && locationKeys.Contains(l.Key)) continue;
                }
                if (l.Ours && l.Key != null && remove.Contains(l.Key)) continue;
                var v = values.FirstOrDefault(x => l.Ours && x.Key == l.Key);
                if (l.Ours && l.Key != null && v.Key != null) { lines.Add($"{l.Key}={v.Value}"); done.Add(l.Key); }
                else lines.Add(text);
            }
        }
        else lines.AddRange(new[] { "# RB Stems Plus settings (RB Stems Plus > Settings), read by Stems Cache when rekordbox starts.", "" });
        int at = firstSection ?? lines.Count;
        var add = values.Where(v => !done.Contains(v.Key)).Select(v => $"{v.Key}={v.Value}").ToList();
        int insertAt = at == lines.Count && lines.Count > 0 && lines[^1] == "" ? lines.Count - 1 : at;
        lines.InsertRange(insertAt, add);
        var text2 = (bom ? "\uFEFF" : "") + string.Join("\n", lines);
        if (!text2.EndsWith('\n')) text2 += "\n";
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(file)!);
            Files.WriteAtomically(file, new UTF8Encoding(false).GetBytes(text2));
            return true;
        }
        catch { return false; }
    }
}
