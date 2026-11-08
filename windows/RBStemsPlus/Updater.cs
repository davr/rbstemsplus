// Updating RB Stems Plus itself (app/Sources/Update.swift): at most once a day the app downloads
// payload-windows.json; when it names a newer app, "Update RB Stems Plus" downloads the zip,
// checks its sha256, unpacks RBStemsPlus.exe, checks its version, then renames the running exe to
// RBStemsPlus.exe.old (Windows allows renaming a running program), moves the new one in, starts it
// and quits. The next start deletes the .old file.
using System.Diagnostics;
using System.IO.Compression;

namespace RBStemsPlus;

static class Updater
{
    public static Manifest? Check(AppPaths p, Log log)
    {
        try
        {
            var last = File.Exists(p.LastUpdateCheckFile) ? File.GetLastWriteTimeUtc(p.LastUpdateCheckFile) : DateTime.MinValue;
            Manifest? m = null;
            if (DateTime.UtcNow - last > TimeSpan.FromDays(1))
            {
                (_, m) = Payload.FetchChecked(p, new PayloadFiles(p), log.Write);
                Files.TryWriteText(p.LastUpdateCheckFile, DateTime.UtcNow.ToString("o") + "\n");
            }
            m ??= Payload.Saved(new PayloadFiles(p));
            if (m != null && Payload.Older(AppVersion.Current, m.AppInfo.Version)) { log.Write($"update: RB Stems Plus {m.AppInfo.Version} is available"); return m; }
        }
        catch (Exception e) { log.Write("update check: " + e.Message); }
        return null;
    }

    /// The new exe, unpacked and checked, or null.
    public static string? Download(AppPaths p, Manifest m, Log log, Action<string> status, Action<int?>? progress = null)
    {
        var dir = Path.Combine(p.UpdateDir, m.AppInfo.Version);
        var zip = Path.Combine(dir, m.AppInfo.Zip);
        if (Payload.Asset(p, m.AppInfo.Zip, zip, m.AppInfo.Sha256, null, "RB Stems Plus " + m.AppInfo.Version, status, log.Write, progress: progress) != Payload.Fetched.Ok) return null;
        status("Unpacking the update…");
        progress?.Invoke(null);
        var outDir = Path.Combine(dir, "app");
        try
        {
            if (Directory.Exists(outDir)) Directory.Delete(outDir, true);
            ZipFile.ExtractToDirectory(zip, outDir);
            var exe = Path.Combine(outDir, "RBStemsPlus.exe");
            var v = FileVersionInfo.GetVersionInfo(exe).ProductVersion?.Split('+')[0];
            if (v != m.AppInfo.Version) { log.Write($"update: the unpacked app is {v}, expected {m.AppInfo.Version}: refused"); return null; }
            return exe;
        }
        catch (Exception e) { log.Write("update: couldn't unpack: " + e.Message); return null; }
    }

    public static void SwapAndRestart(string newExe, Log log)
    {
        var cur = Environment.ProcessPath!;
        var old = cur + ".old";
        try
        {
            Files.Remove(old);
            File.Move(cur, old);
            try { File.Copy(newExe, cur); }
            catch { File.Move(old, cur); throw; }
            log.Write("update: installed; restarting");
            Process.Start(new ProcessStartInfo(cur) { UseShellExecute = false });
            Application.Exit();
        }
        catch (Exception e) { log.Write("update: couldn't replace the app: " + e.Message); }
    }

    public static void CleanUpAfterUpdate()
    {
        try { var old = Environment.ProcessPath + ".old"; if (File.Exists(old)) File.Delete(old); } catch { }
    }

    /// The end of the complete uninstall: the app's folder, its Start menu shortcut and logs go once it has quit.
    public static void DeleteSelfAndQuit(AppPaths p)
    {
        var exe = Environment.ProcessPath!;
        var dir = Path.GetDirectoryName(exe)!;
        var shortcut = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), "RB Stems Plus.lnk");
        Files.Remove(shortcut);
        var logs = Path.GetDirectoryName(p.LogDir)!;        // %LOCALAPPDATA%\rbstemsplus
        // a cmd that waits for this process, then deletes the app's folder (only if it is ours) and the logs
        var ours = string.Equals(Path.GetFileName(dir), "RB Stems Plus", StringComparison.OrdinalIgnoreCase);
        var cmd = $"/c timeout /t 2 /nobreak >nul & rmdir /s /q \"{logs}\" & rmdir /s /q \"{p.Support}\"" + (ours ? $" & rmdir /s /q \"{dir}\"" : $" & del /q \"{exe}\"");
        try { Process.Start(new ProcessStartInfo("cmd.exe", cmd) { CreateNoWindow = true, UseShellExecute = false, WorkingDirectory = Path.GetTempPath() }); } catch { }
        Application.Exit();
    }
}
