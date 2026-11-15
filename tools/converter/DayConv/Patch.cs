// Patch.cs - the v1.20E update data (tools/converter/make_patch.py builds it).
using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;

namespace DayConv;

public sealed class PatchEntry
{
    public string Target, Source;      // Source "" = stored whole
    public byte[] SourceMd5, TargetMd5, Ops;
    public int TargetSize;

    public byte[] Apply(byte[] src)
    {
        if (Source.Length > 0 && !MD5.HashData(src).AsSpan().SequenceEqual(SourceMd5))
            throw new InvalidDataException($"{Source} on the disc is not the expected file.");
        var outp = new byte[TargetSize];
        int o = 0, p = 0;
        int Varint() { int v = 0, s = 0; for (;;) { byte b = Ops[p++]; v |= (b & 0x7f) << s; s += 7; if ((b & 0x80) == 0) return v; } }
        while (p < Ops.Length) {
            byte t = Ops[p++];
            if (t == 0) { int off = Varint(), len = Varint(); Buffer.BlockCopy(src, off, outp, o, len); o += len; }
            else { int len = Varint(); Buffer.BlockCopy(Ops, p, outp, o, len); p += len; o += len; }
        }
        if (o != TargetSize || !MD5.HashData(outp).AsSpan().SequenceEqual(TargetMd5))
            throw new InvalidDataException($"Patching {Target} gave a wrong result.");
        return outp;
    }
}

public static class Patch
{
    public static List<PatchEntry> Load()
    {
        using var res = Assembly.GetExecutingAssembly().GetManifestResourceStream("patch.bin")
                        ?? throw new InvalidDataException("The patch data is missing from the tool.");
        using var z = new ZLibStream(res, CompressionMode.Decompress);
        using var ms = new MemoryStream();
        z.CopyTo(ms);
        var r = new BinaryReader(new MemoryStream(ms.ToArray()));
        if (Encoding.ASCII.GetString(r.ReadBytes(4)) != "DAYP" || r.ReadUInt32() != 1) throw new InvalidDataException("Bad patch data.");
        int n = (int)r.ReadUInt32();
        string Str() => Encoding.UTF8.GetString(r.ReadBytes(r.ReadUInt16()));
        var list = new List<PatchEntry>();
        for (int i = 0; i < n; i++) {
            var e = new PatchEntry { Target = Str(), Source = Str(), SourceMd5 = r.ReadBytes(16), TargetMd5 = r.ReadBytes(16) };
            e.TargetSize = (int)r.ReadUInt32();
            e.Ops = r.ReadBytes((int)r.ReadUInt32());
            list.Add(e);
        }
        return list;
    }
}
