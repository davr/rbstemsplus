// Where Stems Cache saves stems: this PC (%LOCALAPPDATA%\rbstemsplus\Cache) or a folder named
// "RB Stems Plus Cache" on an external drive, identified by its marker (.rbstemsplus-cache:
// "id=<UUID>\nformat=rbstems-v1\n", the same as on macOS: app/Sources/Location.swift). The bridge
// checks the drive again itself (check_location); this is the app's side.
using System.Runtime.InteropServices;
using System.Text;

namespace RBStemsPlus;

public static class CacheLocation
{
    public static string MarkerText(string id) => $"id={id}\nformat={Consts.MarkerFormat}\n";

    /// The marker's id when the text is a valid marker (the bridge's marker_ok), else null.
    public static string? MarkerId(string text)
    {
        if (text.Contains('\0')) return null;
        string? id = null;
        bool format = false;
        foreach (var line in text.Split('\n'))
        {
            if (line.StartsWith("id=") && Encoding.UTF8.GetByteCount(line) > 3)
            {
                var v = line[3..];
                if (!ConfigFile.IsUuid(v) || (id != null && !string.Equals(id, v, StringComparison.OrdinalIgnoreCase))) return null;
                id ??= v;
            }
            else if (line.StartsWith("format=") && Encoding.UTF8.GetByteCount(line) > 7)
            {
                if (line[7..] != Consts.MarkerFormat) return null;
                format = true;
            }
        }
        return format ? id : null;
    }

    public static string? ReadMarker(string dir)
    {
        try
        {
            var f = new FileInfo(Path.Combine(dir, Consts.MarkerName));
            if (!f.Exists || f.Length > Consts.MarkerMax || (f.Attributes & FileAttributes.ReparsePoint) != 0) return null;
            return MarkerId(File.ReadAllText(f.FullName));
        }
        catch { return null; }
    }

    public enum Refusal { None, NotExternal, Network, ReadOnly, SystemDrive, FileSystem, Link, Path, Unreadable }

    public static string RefusalText(Refusal r, string drive) => r switch
    {
        Refusal.NotExternal => $"\"{drive}\" isn't an external drive. Choose a USB or Thunderbolt drive.",
        Refusal.Network => $"\"{drive}\" is a network drive. Choose a drive connected to this PC.",
        Refusal.ReadOnly => $"\"{drive}\" is read-only.",
        Refusal.SystemDrive => "That's the drive Windows runs from. Choose an external drive, or keep the stems on this PC.",
        Refusal.FileSystem => $"\"{drive}\" has a file system Stems Cache doesn't use (NTFS, exFAT or FAT32 only).",
        Refusal.Link => "That folder is a link or a junction. Choose a real folder.",
        Refusal.Path => "That folder's path is too long or has characters Stems Cache can't use.",
        _ => $"\"{drive}\" doesn't answer.",
    };

    /// Whether a drive can hold the stems (the bridge refuses the same: check_location).
    public static Refusal Check(string folder)
    {
        if (!OperatingSystem.IsWindows()) return Refusal.Unreadable;
        try
        {
            var root = Path.GetPathRoot(Path.GetFullPath(folder));
            if (root == null || root.StartsWith(@"\\")) return Refusal.Network;
            var d = new DriveInfo(root);
            if (!d.IsReady) return Refusal.Unreadable;
            if (d.DriveType == DriveType.Network) return Refusal.Network;
            var win = Path.GetPathRoot(Environment.GetFolderPath(Environment.SpecialFolder.Windows));
            if (string.Equals(win, root, StringComparison.OrdinalIgnoreCase)) return Refusal.SystemDrive;
            if (d.DriveFormat is not ("NTFS" or "exFAT" or "FAT32" or "FAT")) return Refusal.FileSystem;
            if (!(d.DriveType == DriveType.Removable || (d.DriveType == DriveType.Fixed && Drives.OnExternalBus(root[0])))) return Refusal.NotExternal;
            for (var p = Path.GetFullPath(folder); p != null && p.Length > root.Length; p = Path.GetDirectoryName(p))
                if (Directory.Exists(p) && Files.IsReparsePoint(p)) return Refusal.Link;
            return Refusal.None;
        }
        catch { return Refusal.Unreadable; }
    }

    /// Makes our folder in `parent` (or uses it if `parent` is it) with a new marker; the folder's
    /// path, its id, or why not. An existing folder with a valid marker is used as it is.
    public static (string? Dir, string? Id, string? Error) Create(string parent)
    {
        try
        {
            var dir = Path.GetFileName(parent.TrimEnd('\\')) == Consts.LocationFolderName ? parent.TrimEnd('\\') : Path.Combine(parent, Consts.LocationFolderName);
            if (ConfigFile.CacheDirProblem(dir) != null) return (null, null, RefusalText(Refusal.Path, ""));
            Directory.CreateDirectory(dir);
            if (Files.IsReparsePoint(dir)) return (null, null, RefusalText(Refusal.Link, ""));
            if (ReadMarker(dir) is { } existing) return (dir, existing, null);
            var id = Guid.NewGuid().ToString().ToUpperInvariant();
            using (var fs = new FileStream(Path.Combine(dir, Consts.MarkerName), FileMode.CreateNew, FileAccess.Write))
            {
                fs.Write(Encoding.UTF8.GetBytes(MarkerText(id)));
                fs.Flush(true);
            }
            try { File.SetAttributes(Path.Combine(dir, Consts.MarkerName), FileAttributes.Hidden); } catch { }
            return (dir, id, null);
        }
        catch (Exception e) { return (null, null, e.Message); }
    }

    /// The disk space the cache's entries take (the bridge's layout: <64 hex>\<2 hex>\<key>.flac and
    /// .desc), allocation size where Windows tells it; 0 for no folder, null if it can't be read.
    public static long? EntriesSize(string folder)
    {
        if (!Directory.Exists(folder)) return File.Exists(folder) ? null : 0;
        long total = 0;
        try
        {
            foreach (var m in Directory.EnumerateDirectories(folder))
            {
                var mn = Path.GetFileName(m);
                if (!Files.IsSha256(mn) || Files.IsReparsePoint(m)) continue;
                foreach (var x in Directory.EnumerateDirectories(m))
                {
                    var xn = Path.GetFileName(x);
                    if (xn.Length != 2 || !xn.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f') || Files.IsReparsePoint(x)) continue;
                    foreach (var f in new DirectoryInfo(x).EnumerateFiles())
                        if (f.Name.StartsWith(xn) && (f.Name.EndsWith(".flac") || f.Name.EndsWith(".desc")) && (f.Attributes & FileAttributes.ReparsePoint) == 0)
                            total += Drives.AllocatedSize(f);
                }
            }
        }
        catch { return null; }
        return total;
    }

    public static string AboutSize(long bytes)
    {
        if (bytes <= 0) return "";
        double gb = bytes / 1_073_741_824.0;
        return gb >= 0.1 ? $" (about {gb:0.0} GB)" : $" (about {Math.Max(1, bytes / 1_048_576)} MB)";
    }

    /// Deletes the cache's entries in `folder` (only names the bridge writes; never the marker or
    /// anything else, never through a link).
    public static void ClearEntries(string folder, Action<string> log)
    {
        if (!Directory.Exists(folder) || Files.IsReparsePoint(folder)) return;
        int n = 0;
        foreach (var m in Directory.EnumerateDirectories(folder))
        {
            if (!Files.IsSha256(Path.GetFileName(m)) || Files.IsReparsePoint(m)) continue;
            try { Directory.Delete(m, true); n++; } catch (Exception e) { log($"couldn't clear {m}: {e.Message}"); }
        }
        foreach (var f in Directory.EnumerateFiles(folder, "model-*.sha256*")) Files.Remove(f);
        log($"saved stems cleared in {folder} ({n} model folders)");
    }
}

static class Drives
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern uint GetCompressedFileSizeW(string path, out uint high);

    public static long AllocatedSize(FileInfo f)
    {
        if (OperatingSystem.IsWindows())
        {
            try
            {
                uint lo = GetCompressedFileSizeW(f.FullName, out var hi);
                if (lo != 0xFFFFFFFF || Marshal.GetLastWin32Error() == 0) return (long)hi << 32 | lo;
            }
            catch { }
        }
        return f.Length;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct Query { public int PropertyId, QueryType; public byte Extra; }

    [StructLayout(LayoutKind.Sequential)]
    struct Descriptor
    {
        public uint Version, Size; public byte DeviceType, DeviceTypeModifier, RemovableMedia, CommandQueueing;
        public uint VendorIdOffset, ProductIdOffset, ProductRevisionOffset, SerialNumberOffset; public int BusType; public uint RawPropertiesLength;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, ref Query q, int qn, out Descriptor d, int dn, out int got, IntPtr ov);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    /// Whether drive `letter` sits on a USB, 1394, SD or MMC bus, or reports removable media.
    public static bool OnExternalBus(char letter)
    {
        var h = CreateFileW($@"\\.\{letter}:", 0, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (h == new IntPtr(-1)) return false;
        try
        {
            var q = new Query { PropertyId = 0, QueryType = 0 };
            if (!DeviceIoControl(h, 0x2D1400 /* IOCTL_STORAGE_QUERY_PROPERTY */, ref q, Marshal.SizeOf<Query>(), out var d, Marshal.SizeOf<Descriptor>(), out _, IntPtr.Zero)) return false;
            return d.BusType is 7 /* USB */ or 4 /* 1394 */ or 12 /* SD */ or 13 /* MMC */ || d.RemovableMedia != 0;
        }
        finally { CloseHandle(h); }
    }
}
