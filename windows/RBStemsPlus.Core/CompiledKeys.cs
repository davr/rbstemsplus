// The public keys compiled into this app: the embedded keys/*.pub.pem (RBStemsPlus.Core.csproj).
namespace RBStemsPlus;

public static class CompiledKeys
{
    public static IEnumerable<(string Name, string Der)> All
    {
        get
        {
            var asm = typeof(CompiledKeys).Assembly;
            foreach (var res in asm.GetManifestResourceNames().Where(n => n.StartsWith("keys.") && n.EndsWith(".pub.pem")).OrderBy(n => n))
            {
                using var s = asm.GetManifestResourceStream(res)!;
                var pem = new StreamReader(s).ReadToEnd();
                var der = string.Concat(pem.Split('\n').Select(l => l.Trim()).Where(l => l.Length > 0 && !l.StartsWith("-----")));
                yield return (res["keys.".Length..^".pub.pem".Length], der);
            }
        }
    }
}
