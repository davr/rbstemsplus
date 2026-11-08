// Stems Cache's changes to rekordbox, made as an administrator: the app starts itself again with
// "--elevated <op> ..." through UAC (runas), and this runs in that process. The counterpart of the
// macOS root scripts (app/Sources/Scripts.swift), on Windows:
// - install: Pioneer's onnxruntime.dll is checked (AlphaTheta's signature, ONNX Runtime 1.18),
//   backed up to %ProgramData%\rbstemsplus\pioneer\<version>\ with SHA256SUMS, copied to
//   %ProgramData%\rbstemsplus\ort\ (the copy the bridge loads), and the bridge goes in its place
//   through a temporary file renamed over it. %ProgramData%\rbstemsplus gets explicit permissions:
//   Administrators and SYSTEM may change it, everyone else only read (the bridge refuses a real
//   library others can change). No re-signing: Windows doesn't check the signature of the DLLs a
//   program loads. Any failure puts Pioneer's file back.
// - uninstall: Pioneer's file back, byte for byte (checked against SHA256SUMS and its signature),
//   then %ProgramData%\rbstemsplus removed.
// Refuses while rekordbox or its installer runs, and while another elevated change runs.
using System.Security.AccessControl;
using System.Security.Principal;
using System.Text;

namespace RBStemsPlus;

public enum AdminExit
{
    Ok = 0, Usage = 2, Busy = 3, RekordboxNotFound = 4, RekordboxOpen = 5, NotPioneers = 6, Damaged = 7, InstallerRunning = 8,
    NoOriginal = 9, CopyFailed = 10, NotAdmin = 11, VersionChanged = 12, Failed = 13,
}

public static class Elevated
{
    public static string Describe(AdminExit e) => e switch
    {
        AdminExit.Ok => "",
        AdminExit.Busy => "Another change to rekordbox is running. Try again in a moment.",
        AdminExit.RekordboxNotFound => "rekordbox 7 wasn't found.",
        AdminExit.RekordboxOpen => "Quit rekordbox first.",
        AdminExit.NotPioneers => "rekordbox's ONNX Runtime isn't the original: reinstall rekordbox.",
        AdminExit.Damaged => "The Stems Cache file didn't verify. Nothing was changed.",
        AdminExit.InstallerRunning => "rekordbox's installer is running. Try again once it's done.",
        AdminExit.NoOriginal => "Pioneer's original file isn't saved on this PC: reinstall rekordbox from rekordbox.com.",
        AdminExit.CopyFailed => "A file couldn't be copied (is the disk full?). rekordbox was left as it was.",
        AdminExit.NotAdmin => "Windows didn't give RB Stems Plus administrator rights.",
        AdminExit.VersionChanged => "rekordbox changed meanwhile (an update?). Nothing was changed.",
        _ => "Something went wrong; rekordbox was left as it was. The log says more.",
    };

    public static bool IsAdmin()
    {
        if (!OperatingSystem.IsWindows()) return false;
        using var id = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(id).IsInRole(WindowsBuiltInRole.Administrator);
    }

    /// Runs one elevated operation; its exit code. args: op, then its arguments.
    public static AdminExit Run(string[] args, AppPaths p, Log log)
    {
        if (args.Length < 1) return AdminExit.Usage;
        if (!IsAdmin()) return AdminExit.NotAdmin;
        using var mutex = new Mutex(false, Consts.AdminMutex);
        try { if (!mutex.WaitOne(TimeSpan.FromSeconds(5))) return AdminExit.Busy; }
        catch (AbandonedMutexException) { }
        try
        {
            if (Rekordbox.PioneerUpdating()) return AdminExit.InstallerRunning;
            if (Rekordbox.Running()) return AdminExit.RekordboxOpen;
            return args[0] switch
            {
                "install-cache" when args.Length == 5 => Install(p, log, args[1], args[2], args[3], args[4]),
                "uninstall-cache" when args.Length >= 1 => Uninstall(p, log, args.Skip(1).ToArray()),
                _ => AdminExit.Usage,
            };
        }
        catch (Exception e) { log.Write("elevated: FAILED " + e); return AdminExit.Failed; }
        finally { try { mutex.ReleaseMutex(); } catch { } }
    }

    /// install-cache <rekordbox folder> <its version> <bridge file> <bridge sha256>
    static AdminExit Install(AppPaths p, Log log, string dir, string version, string bridge, string bridgeSha)
    {
        var rb = Rekordbox.Describe(dir);
        if (rb == null) return AdminExit.RekordboxNotFound;
        if (rb.Version != version) return AdminExit.VersionChanged;
        if (!Files.Verified(bridge, bridgeSha, null)) return AdminExit.Damaged;
        var backup = p.PioneerBackupDir(rb.Version);
        var backupOrt = Path.Combine(backup, "onnxruntime.dll");
        var current = File.ReadAllBytes(rb.Ort);
        string pioneerSha;
        if (Rekordbox.IsBridge(current))
        {
            // a reinstall or an update of the bridge: Pioneer's file must be saved already
            if (!BackupGood(backup, out pioneerSha)) return AdminExit.NoOriginal;
        }
        else
        {
            if (!Rekordbox.SignedByPioneer(rb.Ort)) return AdminExit.NotPioneers;
            pioneerSha = Files.Sha256(current);
            Directory.CreateDirectory(p.RootDir);
            SecureRoot(p.RootDir);
            Directory.CreateDirectory(backup);
            if (!(File.Exists(backupOrt) && Files.Sha256(backupOrt) == pioneerSha))
            {
                Files.Remove(backupOrt + ".part");
                if (Files.Copy(rb.Ort, backupOrt + ".part") != null || Files.Sha256(backupOrt + ".part") != pioneerSha) { Files.Remove(backupOrt + ".part"); return AdminExit.CopyFailed; }
                File.Move(backupOrt + ".part", backupOrt, true);
            }
            Files.WriteAtomically(Path.Combine(backup, "SHA256SUMS"), Encoding.ASCII.GetBytes($"{pioneerSha}  onnxruntime.dll\n"));
            log.Write($"Stems Cache: Pioneer's onnxruntime.dll saved ({pioneerSha[..12]}, rekordbox {rb.Version})");
        }
        // the copy the bridge loads, always Pioneer's
        Directory.CreateDirectory(p.RootOrtDir);
        SecureRoot(p.RootDir);
        if (!(File.Exists(p.RootOrt) && Files.Sha256(p.RootOrt) == pioneerSha))
        {
            Files.Remove(p.RootOrt + ".part");
            if (Files.Copy(backupOrt, p.RootOrt + ".part") != null || Files.Sha256(p.RootOrt + ".part") != pioneerSha) { Files.Remove(p.RootOrt + ".part"); return AdminExit.CopyFailed; }
            File.Move(p.RootOrt + ".part", p.RootOrt, true);
        }
        SecureRoot(p.RootDir);
        // the bridge in rekordbox's folder: a temporary file, checked, renamed over Pioneer's
        var tmp = rb.Ort + ".rbsp-new";
        Files.Remove(tmp);
        if (Files.Copy(bridge, tmp) != null || Files.Sha256(tmp) != bridgeSha) { Files.Remove(tmp); return AdminExit.CopyFailed; }
        if (Rekordbox.Running() || Rekordbox.PioneerUpdating()) { Files.Remove(tmp); return AdminExit.RekordboxOpen; }
        try { File.Move(tmp, rb.Ort, true); }
        catch (Exception e) { Files.Remove(tmp); log.Write("Stems Cache: the bridge couldn't be put in place: " + e.Message); return AdminExit.CopyFailed; }
        if (Files.Sha256(rb.Ort) != bridgeSha)
        {
            log.Write("Stems Cache: the bridge didn't verify in place: putting Pioneer's back");
            RestoreFrom(backupOrt, rb.Ort, pioneerSha, log);
            return AdminExit.Failed;
        }
        log.Write($"Stems Cache: installed in rekordbox {rb.Version} ({bridgeSha[..12]})");
        return AdminExit.Ok;
    }

    /// uninstall-cache [<rekordbox folder>...]: Pioneer's file back in each folder holding the bridge
    /// (every rekordbox 7 found when none is given), then the admin-only folder removed.
    static AdminExit Uninstall(AppPaths p, Log log, string[] dirs)
    {
        var installs = dirs.Length > 0 ? dirs.Select(Rekordbox.Describe).OfType<RekordboxInstall>().ToList() : Rekordbox.All();
        foreach (var rb in installs)
        {
            if (!File.Exists(rb.Ort) || !Rekordbox.IsBridge(File.ReadAllBytes(rb.Ort))) continue;
            var backup = p.PioneerBackupDir(rb.Version);
            if (!BackupGood(backup, out var sha)) return AdminExit.NoOriginal;
            if (!RestoreFrom(Path.Combine(backup, "onnxruntime.dll"), rb.Ort, sha, log)) return AdminExit.CopyFailed;
            if (!Rekordbox.SignedByPioneer(rb.Ort)) log.Write("Stems Cache: WARNING the restored onnxruntime.dll doesn't verify as AlphaTheta's");
            log.Write($"Stems Cache: Pioneer's onnxruntime.dll is back in rekordbox {rb.Version}");
        }
        if (Directory.Exists(p.RootDir) && !Files.IsReparsePoint(p.RootDir))
        {
            try { Directory.Delete(p.RootDir, true); } catch (Exception e) { log.Write("Stems Cache: couldn't remove " + p.RootDir + ": " + e.Message); return AdminExit.Failed; }
        }
        log.Write("Stems Cache: uninstalled");
        return AdminExit.Ok;
    }

    static bool BackupGood(string backup, out string sha)
    {
        sha = "";
        var sums = Files.ReadText(Path.Combine(backup, "SHA256SUMS"));
        var f = Path.Combine(backup, "onnxruntime.dll");
        if (sums == null || !File.Exists(f)) return false;
        var want = sums.Split(' ', 2)[0].Trim();
        if (!Files.IsSha256(want) || Files.Sha256(f) != want) return false;
        sha = want;
        return true;
    }

    static bool RestoreFrom(string saved, string target, string sha, Log log)
    {
        var tmp = target + ".rbsp-restore";
        Files.Remove(tmp);
        if (Files.Copy(saved, tmp) != null || Files.Sha256(tmp) != sha) { Files.Remove(tmp); return false; }
        try { File.Move(tmp, target, true); } catch (Exception e) { Files.Remove(tmp); log.Write("restore failed: " + e.Message); return false; }
        return Files.Sha256(target) == sha;
    }

    /// Administrators and SYSTEM: full control; Users: read and run; nothing inherited; owned by
    /// Administrators. Applied to the folder and everything in it.
    public static void SecureRoot(string dir)
    {
        if (!OperatingSystem.IsWindows()) return;
        var admins = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
        var system = new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null);
        var users = new SecurityIdentifier(WellKnownSidType.BuiltinUsersSid, null);
        var sec = new DirectorySecurity();
        sec.SetOwner(admins);
        sec.SetAccessRuleProtection(true, false);
        var inherit = InheritanceFlags.ContainerInherit | InheritanceFlags.ObjectInherit;
        sec.AddAccessRule(new FileSystemAccessRule(admins, FileSystemRights.FullControl, inherit, PropagationFlags.None, AccessControlType.Allow));
        sec.AddAccessRule(new FileSystemAccessRule(system, FileSystemRights.FullControl, inherit, PropagationFlags.None, AccessControlType.Allow));
        sec.AddAccessRule(new FileSystemAccessRule(users, FileSystemRights.ReadAndExecute, inherit, PropagationFlags.None, AccessControlType.Allow));
        new DirectoryInfo(dir).SetAccessControl(sec);
        foreach (var f in Directory.EnumerateFileSystemEntries(dir, "*", SearchOption.AllDirectories))
        {
            if (Directory.Exists(f))
            {
                var ds = new DirectoryInfo(f).GetAccessControl();
                ds.SetOwner(admins);
                ds.SetAccessRuleProtection(false, false);
                foreach (FileSystemAccessRule r in ds.GetAccessRules(true, false, typeof(SecurityIdentifier))) ds.RemoveAccessRuleSpecific(r);
                new DirectoryInfo(f).SetAccessControl(ds);
            }
            else
            {
                var fs = new FileInfo(f).GetAccessControl();
                fs.SetOwner(admins);
                fs.SetAccessRuleProtection(false, false);
                foreach (FileSystemAccessRule r in fs.GetAccessRules(true, false, typeof(SecurityIdentifier))) fs.RemoveAccessRuleSpecific(r);
                new FileInfo(f).SetAccessControl(fs);
            }
        }
    }
}
