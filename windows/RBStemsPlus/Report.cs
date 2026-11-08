// Help > Create Report (app/Sources/Report.swift): a zip on the Desktop with RB Stems Plus's logs and
// settings, what it finds on this PC, and rekordbox's recent crash reports' names. It never holds
// the library, music or track names; the user name is replaced by <user>. Never sent anywhere:
// the user attaches it to a GitHub issue.
using System.IO.Compression;
using System.Text;

namespace RBStemsPlus;

static class Report
{
    public static string? Create(AppPaths p, Log log)
    {
        try
        {
            var desk = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
            var zip = Path.Combine(desk, $"RB Stems Plus report {DateTime.Now:yyyy-MM-dd HHmm}.zip");
            using var z = ZipFile.Open(zip, ZipArchiveMode.Create);
            void Add(string name, string? text) { if (text == null) return; using var w = new StreamWriter(z.CreateEntry(name).Open(), new UTF8Encoding(false)); w.Write(Redact(text)); }
            Add("probe.txt", Probe.Text(p));
            foreach (var f in new[] { p.AppLog, p.WatcherLog, p.AdminLog, p.BridgeLog, p.BridgeLog + ".1", p.BridgeStateFile })
                Add(Path.GetFileName(f), ReadShared(f));
            Add("config.ini", Files.ReadText(p.ConfigFile));
            Add("installed", Files.ReadText(p.ChosenFile));
            var crashes = new StringBuilder();
            foreach (var d in new[] { Path.Combine(p.Roaming, "Pioneer", "rekordbox", "Crashes"),
                                      Path.Combine(p.Local, "Microsoft", "Windows", "WER", "ReportArchive") })
            {
                try
                {
                    foreach (var e in new DirectoryInfo(d).EnumerateFileSystemInfos().Where(e => e.Name.Contains("rekordbox", StringComparison.OrdinalIgnoreCase) || d.EndsWith("Crashes"))
                                 .OrderByDescending(e => e.LastWriteTimeUtc).Take(20))
                        crashes.AppendLine($"{e.LastWriteTime:yyyy-MM-dd HH:mm}  {e.Name}");
                }
                catch { }
            }
            Add("crashes.txt", crashes.ToString());
            log.Write("report: saved " + zip);
            return zip;
        }
        catch (Exception e) { log.Write("report: FAILED " + e.Message); return null; }
    }

    static string? ReadShared(string f)
    {
        try { using var fs = new FileStream(f, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete); return new StreamReader(fs).ReadToEnd(); }
        catch { return null; }
    }

    static string Redact(string s)
    {
        var u = Environment.UserName;
        return string.IsNullOrEmpty(u) ? s : s.Replace(u, "<user>", StringComparison.OrdinalIgnoreCase);
    }

    public static string IssueUrl(AppPaths p)
    {
        var body = $"**What happened?**\n\n\n\n**Details**\n```\n{Redact(Probe.Text(p))}```\n(Drag the report zip from your Desktop here.)";
        return Consts.IssuesUrl + "?title=" + Uri.EscapeDataString("Windows: ") + "&body=" + Uri.EscapeDataString(body);
    }
}
