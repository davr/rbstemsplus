// Small shared helpers: atomic writes, checksums, logs.
using System.Security.Cryptography;
using System.Text;

namespace RBStemsPlus;

public static class Files
{
    /// Writes to a temporary file next to `path`, flushed to the disk, then renamed over it.
    public static void WriteAtomically(string path, byte[] data)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        var tmp = path + "." + Environment.ProcessId + "." + Guid.NewGuid().ToString("N")[..8] + ".tmp";
        try
        {
            using (var fs = new FileStream(tmp, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                fs.Write(data);
                fs.Flush(true);
            }
            File.Move(tmp, path, overwrite: true);
        }
        finally { try { if (File.Exists(tmp)) File.Delete(tmp); } catch { } }
    }

    public static bool TryWriteAtomically(string path, byte[] data)
    {
        try { WriteAtomically(path, data); return true; } catch { return false; }
    }

    public static bool TryWriteText(string path, string text) => TryWriteAtomically(path, new UTF8Encoding(false).GetBytes(text));

    public static string? ReadText(string path)
    {
        try { return File.ReadAllText(path); } catch { return null; }
    }

    /// A file's SHA-256 in lowercase hex, read in 1 MB pieces; null if unreadable.
    public static string? Sha256(string path)
    {
        try
        {
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1 << 20);
            return Convert.ToHexString(SHA256.HashData(fs)).ToLowerInvariant();
        }
        catch { return null; }
    }

    public static string Sha256(byte[] data) => Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();

    public static long? Size(string path)
    {
        try { var f = new FileInfo(path); return f.Exists ? f.Length : null; } catch { return null; }
    }

    public static bool Verified(string path, string sha, long? size)
    {
        var s = Size(path);
        if (s == null || (size != null && s != size)) return false;
        return Sha256(path) == sha;
    }

    public static bool IsSha256(string s) => s.Length == 64 && s.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f');

    /// Copies with a flush to the disk; null when done, else why not.
    public static string? Copy(string from, string to)
    {
        try
        {
            using (var src = new FileStream(from, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, 1 << 20))
            using (var dst = new FileStream(to, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1 << 20))
            {
                src.CopyTo(dst, 1 << 20);
                dst.Flush(true);
            }
            return null;
        }
        catch (IOException e) when (IsDiskFull(e)) { return "No space left on the disk"; }
        catch (Exception e) { return e.Message; }
    }

    public static bool IsDiskFull(IOException e) => (e.HResult & 0xFFFF) is 112 or 39;   // ERROR_DISK_FULL, ERROR_HANDLE_DISK_FULL

    public static void Remove(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); } catch { }
    }

    /// Whether path is a real file or folder (not a link or junction).
    public static bool IsReparsePoint(string path)
    {
        try { return (File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0; } catch { return false; }
    }
}

/// An append-only log with a timestamp per line; over 5 MB it becomes <name>.1.
public sealed class Log
{
    readonly string path;
    static readonly object gate = new();
    public Action<string>? Echo;

    public Log(string path) { this.path = path; }

    public void Write(string line)
    {
        lock (gate)
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                var fi = new FileInfo(path);
                if (fi.Exists && fi.Length > 5 << 20) File.Move(path, path + ".1", true);
                File.AppendAllText(path, $"{DateTime.Now:yyyy-MM-dd HH:mm:ss}  {line}\n");
            }
            catch { }
        }
        Echo?.Invoke(line);
    }

    public static implicit operator Action<string>(Log l) => l.Write;
}
