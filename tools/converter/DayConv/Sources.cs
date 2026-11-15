// Sources.cs - where the game comes from: a CUE sheet (BIN tracks), a lone
// BIN or ISO, or the game's exe on a CD drive / in a folder.
using System.Globalization;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace DayConv;

/// <summary>A CD audio track: raw 44.1 kHz 16-bit stereo, 2352 bytes per sector.</summary>
public abstract class AudioTrack
{
    public int Number;
    public long Sectors;
    /// <summary>Reads up to count sectors from sector first (relative to the track).</summary>
    public abstract int Read(long first, int count, byte[] buf);
}

public abstract class GameSource : IDisposable
{
    public string Description = "";
    public readonly List<AudioTrack> Audio = new();
    /// <summary>Paths relative to the game folder ("Resource/course0.mdl").</summary>
    public abstract IEnumerable<string> GameFiles();
    public abstract byte[] ReadGameFile(string rel);
    public virtual void Dispose() { }

    public const string ExeName = "DAYTONA USA Deluxe.exe";

    public static GameSource Open(string path, Action<string> log)
    {
        string ext = Path.GetExtension(path).ToLowerInvariant();
        switch (ext) {
        case ".cue": return new DiscImageSource(CueSheet.Parse(path), log);
        case ".bin": case ".img": {
            string cue = Path.ChangeExtension(path, ".cue");
            if (!File.Exists(cue)) {   // "Name (Track 01).bin" next to "Name.cue"
                var dir = Path.GetDirectoryName(path);
                cue = Directory.GetFiles(dir, "*.cue").FirstOrDefault(c =>
                    File.ReadAllText(c).Contains(Path.GetFileName(path), StringComparison.OrdinalIgnoreCase));
            }
            if (cue != null && File.Exists(cue)) {
                log($"Using the CUE sheet {Path.GetFileName(cue)}");
                return new DiscImageSource(CueSheet.Parse(cue), log);
            }
            log("No CUE sheet next to the BIN: only the data track is read (no music).");
            return new DiscImageSource(CueSheet.SingleData(path, FileSectorSource.LooksRaw(path) ? 2352 : 2048), log);
        }
        case ".iso":
            log("An ISO holds only the data track: the CD music is not in it.");
            return new DiscImageSource(CueSheet.SingleData(path, FileSectorSource.LooksRaw(path) ? 2352 : 2048), log);
        case ".exe":
            return new FolderSource(Path.GetDirectoryName(path), log);
        }
        throw new InvalidDataException("Choose a .cue, .bin, .iso or the game's .exe.");
    }
}

/// <summary>A CUE sheet: the data track holds the ISO file system, the
/// other tracks are the CD audio.</summary>
public sealed class CueSheet
{
    public sealed class Track
    {
        public int Number;
        public string File;
        public bool Audio, BigEndian;
        public int SectorSize = 2352;
        public long Index0 = -1, Index1;   // sectors from the start of the file
        public long End = -1;              // sector after the last one (-1: end of file)
    }
    public readonly List<Track> Tracks = new();

    public static CueSheet Parse(string path)
    {
        var cue = new CueSheet();
        string dir = Path.GetDirectoryName(path), file = null;
        bool big = false;
        Track t = null;
        foreach (var raw in System.IO.File.ReadAllLines(path)) {
            var line = raw.Trim();
            if (line.StartsWith("FILE ", StringComparison.OrdinalIgnoreCase)) {
                int q1 = line.IndexOf('"'), q2 = line.LastIndexOf('"');
                string name = q1 >= 0 && q2 > q1 ? line[(q1 + 1)..q2] : line.Split(' ')[1];
                file = Path.Combine(dir, name);
                big = line.EndsWith("MOTOROLA", StringComparison.OrdinalIgnoreCase);
                if (!System.IO.File.Exists(file)) throw new FileNotFoundException("The CUE sheet names a missing file: " + name);
            } else if (line.StartsWith("TRACK ", StringComparison.OrdinalIgnoreCase)) {
                var p = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                t = new Track { Number = int.Parse(p[1], CultureInfo.InvariantCulture), File = file, BigEndian = big };
                string mode = p.Length > 2 ? p[2].ToUpperInvariant() : "AUDIO";
                t.Audio = mode == "AUDIO";
                if (mode.EndsWith("/2048")) t.SectorSize = 2048;
                else if (mode.EndsWith("/2336")) t.SectorSize = 2336;
                cue.Tracks.Add(t);
            } else if (line.StartsWith("INDEX ", StringComparison.OrdinalIgnoreCase) && t != null) {
                var p = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                var m = p[2].Split(':');
                long s = (long.Parse(m[0]) * 60 + long.Parse(m[1])) * 75 + long.Parse(m[2]);
                if (p[1] == "00") t.Index0 = s; else if (p[1] == "01") t.Index1 = s;
            }
        }
        // a track ends where the next one in the same file starts (its pregap included)
        for (int i = 0; i < cue.Tracks.Count; i++) {
            var a = cue.Tracks[i];
            if (i + 1 < cue.Tracks.Count && cue.Tracks[i + 1].File == a.File) {
                var b = cue.Tracks[i + 1];
                a.End = b.Index0 >= 0 ? b.Index0 : b.Index1;
            }
        }
        if (!cue.Tracks.Any(x => !x.Audio)) throw new InvalidDataException("The CUE sheet has no data track.");
        return cue;
    }

    public static CueSheet SingleData(string path, int sectorSize)
    {
        var c = new CueSheet();
        c.Tracks.Add(new Track { Number = 1, File = path, Audio = false, SectorSize = sectorSize });
        return c;
    }
}

public sealed class DiscImageSource : GameSource
{
    readonly ISectorSource _data;
    readonly Iso9660 _iso;
    readonly Dictionary<string, IsoFile> _game = new(StringComparer.OrdinalIgnoreCase);

    sealed class BinAudio : AudioTrack
    {
        public string File; public long Start; public bool Big;
        public override int Read(long first, int count, byte[] buf)
        {
            long n = Math.Min(count, Sectors - first);
            if (n <= 0) return 0;
            using var f = new FileStream(File, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16);
            f.Position = (Start + first) * 2352;
            int bytes = (int)n * 2352, got = 0;
            while (got < bytes) { int r = f.Read(buf, got, bytes - got); if (r <= 0) break; got += r; }
            if (Big) for (int i = 0; i + 1 < got; i += 2) (buf[i], buf[i + 1]) = (buf[i + 1], buf[i]);
            return got / 2352;
        }
    }

    public DiscImageSource(CueSheet cue, Action<string> log)
    {
        var dt = cue.Tracks.First(t => !t.Audio);
        _data = new FileSectorSource(dt.File, dt.Index1, dt.SectorSize);
        _iso = new Iso9660(_data);
        foreach (var f in _iso.Files)
            if (f.Path.StartsWith("GAME/", StringComparison.OrdinalIgnoreCase)) _game[f.Path[5..]] = f;
        foreach (var t in cue.Tracks.Where(t => t.Audio)) {
            long fileSectors = new FileInfo(t.File).Length / 2352;
            long end = t.End >= 0 ? t.End : fileSectors;
            if (end > t.Index1) Audio.Add(new BinAudio { Number = t.Number, File = t.File, Start = t.Index1, Sectors = end - t.Index1, Big = t.BigEndian });
        }
        Description = $"disc image: {_iso.Files.Count} files, {Audio.Count} audio tracks";
    }

    public override IEnumerable<string> GameFiles() => _game.Keys;
    public override byte[] ReadGameFile(string rel) =>
        _game.TryGetValue(rel, out var f) ? _iso.Read(f) : throw new FileNotFoundException(rel);
    public override void Dispose() => _data.Dispose();
}

/// <summary>The exe's folder (the CD's GAME folder, or a copy); the music is
/// read from the drive when that folder is on an audio CD.</summary>
public sealed class FolderSource : GameSource
{
    readonly string _dir;
    CdDrive _drive;

    public FolderSource(string dir, Action<string> log)
    {
        _dir = dir;
        var root = Path.GetPathRoot(Path.GetFullPath(dir));
        try {
            var di = new DriveInfo(root);
            if (di.DriveType == DriveType.CDRom) {
                _drive = new CdDrive(root.TrimEnd('\\', '/'));
                Audio.AddRange(_drive.AudioTracks());
                log($"CD drive {root}: {Audio.Count} audio tracks");
            } else log("The exe is not on a CD drive: the music can only be read from the CD (or a BIN/CUE).");
        } catch (Exception e) {
            log("Could not read the CD's audio tracks: " + e.Message);
        }
        Description = $"folder {dir}";
    }

    public override IEnumerable<string> GameFiles() =>
        Directory.EnumerateFiles(_dir, "*", SearchOption.AllDirectories)
                 .Select(p => Path.GetRelativePath(_dir, p).Replace('\\', '/'));
    public override byte[] ReadGameFile(string rel) => File.ReadAllBytes(Path.Combine(_dir, rel));
    public override void Dispose() => _drive?.Dispose();
}

/// <summary>Raw CD audio from a physical drive (IOCTL_CDROM_READ_TOC / RAW_READ).</summary>
public sealed class CdDrive : IDisposable
{
    const uint IOCTL_CDROM_READ_TOC = 0x00024000, IOCTL_CDROM_RAW_READ = 0x0002403E;
    readonly SafeFileHandle _h;

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sa, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern unsafe bool DeviceIoControl(SafeFileHandle h, uint code, void* inBuf, int inSize, void* outBuf, int outSize, out int ret, IntPtr ov);

    public CdDrive(string letter)   // "D:"
    {
        _h = CreateFile(@"\\.\" + letter, 0x80000000 /* GENERIC_READ */, 3, IntPtr.Zero, 3 /* OPEN_EXISTING */, 0, IntPtr.Zero);
        if (_h.IsInvalid) throw new IOException("Cannot open the CD drive " + letter + " (error " + Marshal.GetLastWin32Error() + ")");
    }

    sealed class DriveAudio : AudioTrack
    {
        public CdDrive Drive; public long Start;
        public override int Read(long first, int count, byte[] buf)
        {
            int n = (int)Math.Min(count, Sectors - first);
            return n <= 0 ? 0 : Drive.RawRead(Start + first, n, buf);
        }
    }

    public unsafe List<AudioTrack> AudioTracks()
    {
        var toc = new byte[804];
        int ret;
        fixed (byte* p = toc)
            if (!DeviceIoControl(_h, IOCTL_CDROM_READ_TOC, null, 0, p, toc.Length, out ret, IntPtr.Zero))
                throw new IOException("Reading the CD's table of contents failed (error " + Marshal.GetLastWin32Error() + ")");
        int first = toc[2], last = toc[3];
        var list = new List<AudioTrack>();
        long Lba(int i) { int o = 4 + i * 8; return ((toc[o + 5] * 60L + toc[o + 6]) * 75 + toc[o + 7]) - 150; }
        for (int i = 0; i <= last - first; i++) {
            int o = 4 + i * 8;
            bool data = (toc[o + 1] & 0x04) != 0;   // control: data track
            if (data) continue;
            long start = Lba(i), end = Lba(i + 1);  // the entry after the last one is the lead-out
            list.Add(new DriveAudio { Drive = this, Number = toc[o + 2], Start = start, Sectors = end - start });
        }
        return list;
    }

    public unsafe int RawRead(long lba, int count, byte[] buf)
    {
        int done = 0;
        var info = stackalloc byte[16];
        while (done < count) {
            int n = Math.Min(20, count - done);   // drives take ~64 KB per request
            *(long*)info = (lba + done) * 2048;  // DiskOffset in 2048-byte units
            *(int*)(info + 8) = n;               // SectorCount
            *(int*)(info + 12) = 2;              // TrackMode = CDDA
            fixed (byte* p = &buf[done * 2352]) {
                if (!DeviceIoControl(_h, IOCTL_CDROM_RAW_READ, info, 16, p, n * 2352, out _, IntPtr.Zero))
                    throw new IOException($"Reading CD audio at sector {lba + done} failed (error {Marshal.GetLastWin32Error()})");
            }
            done += n;
        }
        return done;
    }

    public void Dispose() => _h.Dispose();
}
