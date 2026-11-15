// Iso9660.cs - sector sources (ISO, raw BIN, a CD drive) and an ISO 9660 /
// Joliet file system reader.
using System.Text;

namespace DayConv;

/// <summary>2048-byte user data sectors of the data track.</summary>
public interface ISectorSource : IDisposable
{
    byte[] ReadSector(long lba);
}

/// <summary>A .iso (2048 bytes per sector) or a raw track (2352 bytes per
/// sector, MODE1 or MODE2/form 1) inside a file.</summary>
public sealed class FileSectorSource : ISectorSource
{
    readonly FileStream _f;
    readonly long _start;          // byte offset of the track's first sector
    readonly int _size, _skip;     // bytes per sector, header bytes before the data

    public FileSectorSource(string path, long startSector, int sectorSize)
    {
        _f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16);
        _size = sectorSize;
        _start = startSector * sectorSize;
        _skip = 0;
        if (sectorSize == 2352) {
            // the mode byte of the first sector: 1 = MODE1 (16-byte header), 2 = MODE2 (24)
            var hdr = new byte[16];
            _f.Position = _start;
            _f.ReadExactly(hdr);
            _skip = hdr[15] == 2 ? 24 : 16;
        }
    }

    public static bool LooksRaw(string path)
    {
        using var f = File.OpenRead(path);
        var b = new byte[12];
        if (f.Read(b, 0, 12) < 12) return false;
        return b[0] == 0 && b[11] == 0 && b.Skip(1).Take(10).All(x => x == 0xff);
    }

    public byte[] ReadSector(long lba)
    {
        var buf = new byte[2048];
        _f.Position = _start + lba * _size + _skip;
        _f.ReadExactly(buf);
        return buf;
    }

    public void Dispose() => _f.Dispose();
}

public sealed class IsoFile
{
    public string Path;            // "GAME/Resource/course0.mdl"
    public long Lba;
    public long Size;
}

/// <summary>Lists the files of an ISO 9660 file system, with the Joliet
/// (long, mixed case) names when the disc has them.</summary>
public sealed class Iso9660
{
    readonly ISectorSource _src;
    public readonly List<IsoFile> Files = new();

    public Iso9660(ISectorSource src)
    {
        _src = src;
        byte[] pvd = null, joliet = null;
        for (long s = 16; s < 64; s++) {
            var d = src.ReadSector(s);
            if (d[1] != 'C' || d[2] != 'D' || d[3] != '0' || d[4] != '0' || d[5] != '1')
                throw new InvalidDataException("No ISO 9660 file system on the data track.");
            if (d[0] == 255) break;
            if (d[0] == 1 && pvd == null) pvd = d;
            if (d[0] == 2 && d[88] == '%' && d[89] == '/' && (d[90] == '@' || d[90] == 'C' || d[90] == 'E')) joliet = d;
        }
        var vd = joliet ?? pvd ?? throw new InvalidDataException("No volume descriptor.");
        bool j = joliet != null;
        Walk(BitConverter.ToUInt32(vd, 156 + 2), BitConverter.ToUInt32(vd, 156 + 10), "", j, 0);
    }

    void Walk(long lba, long size, string path, bool joliet, int depth)
    {
        if (depth > 16) return;
        var data = ReadExtent(lba, size);
        int i = 0;
        while (i < data.Length) {
            int len = data[i];
            if (len == 0) { i = (i / 2048 + 1) * 2048; continue; }
            if (i + len > data.Length) break;
            long elba = BitConverter.ToUInt32(data, i + 2), esz = BitConverter.ToUInt32(data, i + 10);
            int flags = data[i + 25], nl = data[i + 32];
            string name;
            if (nl == 1 && (data[i + 33] == 0 || data[i + 33] == 1)) name = null;
            else name = joliet ? Encoding.BigEndianUnicode.GetString(data, i + 33, nl & ~1)
                               : Encoding.Latin1.GetString(data, i + 33, nl);
            i += len;
            if (name == null) continue;
            int semi = name.IndexOf(';');
            if (semi >= 0) name = name[..semi];
            if (!joliet && name.EndsWith('.')) name = name[..^1];
            string p = path.Length == 0 ? name : path + "/" + name;
            if ((flags & 2) != 0) Walk(elba, esz, p, joliet, depth + 1);
            else Files.Add(new IsoFile { Path = p, Lba = elba, Size = esz });
        }
    }

    byte[] ReadExtent(long lba, long size)
    {
        var buf = new byte[size];
        long done = 0, s = lba;
        while (done < size) {
            var sec = _src.ReadSector(s++);
            int n = (int)Math.Min(2048, size - done);
            Array.Copy(sec, 0, buf, done, n);
            done += n;
        }
        return buf;
    }

    public byte[] Read(IsoFile f) => ReadExtent(f.Lba, f.Size);
}
