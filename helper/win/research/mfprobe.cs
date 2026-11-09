using NAudio.Wave;
// mfprobe FILE OUT.raw [float]: decodes FILE through Media Foundation's source reader to 16-bit (or float) PCM and writes it raw
var path = args[0]; var outp = args[1]; bool flt = args.Length > 2 && args[2] == "float";
var s = new MediaFoundationReader(path, new MediaFoundationReader.MediaFoundationReaderSettings { RequestFloatOutput = flt });
Console.WriteLine($"{s.WaveFormat} length {s.Length} bytes");
using var o = File.Create(outp);
var buf = new byte[1 << 20]; int n;
while ((n = s.Read(buf, 0, buf.Length)) > 0) o.Write(buf, 0, n);
