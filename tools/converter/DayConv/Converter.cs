// Converter.cs - the conversion: check the disc's exe, copy the game folder,
// apply the v1.20E update, encode the CD music to MP3.
using System.Security.Cryptography;
using NAudio.Lame;
using NAudio.Wave;

namespace DayConv;

public sealed class Converter
{
    /// <summary>The exe as pressed on the original CDs (all regions / publishers).</summary>
    public const string CdExeMd5 = "c1e334a15d9472219441ed5f2bb89fb1";
    static readonly Dictionary<string, string> KnownOther = new() {
        ["61a168d474f6bab8fdeffc3a1058082e"] = "the v1.20E update exe (already patched)",
        ["5b726410caeee505adda28df2d52025a"] = "the v1.20E update exe with a No-CD change (already patched)",
        ["ee476877f27b8c77905033726b9704af"] = "the exe this converter makes (already converted)",
    };

    public Action<string> Log = _ => { };
    public Action<int, string> Progress = (_, _) => { };   // percent, step
    public int Bitrate = 192;

    public static string Md5Hex(byte[] d) => Convert.ToHexString(MD5.HashData(d)).ToLowerInvariant();

    /// <summary>Opens the source and checks the exe; returns null when it is not the original.</summary>
    public GameSource OpenAndCheck(string input, out string md5)
    {
        md5 = null;
        var src = GameSource.Open(input, Log);
        try {
            var exe = src.GameFiles().FirstOrDefault(f => f.Equals(GameSource.ExeName, StringComparison.OrdinalIgnoreCase))
                      ?? throw new FileNotFoundException("The game's exe (GAME\\" + GameSource.ExeName + ") is not on this disc.");
            md5 = Md5Hex(src.ReadGameFile(exe));
            Log($"{GameSource.ExeName}: md5 {md5}");
            if (md5 == CdExeMd5) return src;
            Log(KnownOther.TryGetValue(md5, out var what)
                ? $"This is {what}. Use the files from the original CD instead."
                : "This is not the exe of the original CD (expected md5 " + CdExeMd5 + ").");
            src.Dispose();
            return null;
        } catch {
            src.Dispose();
            throw;
        }
    }

    public void Run(GameSource src, string outRoot, CancellationToken ct)
    {
        string dest = Path.Combine(outRoot, "data", "dayusadx");
        Directory.CreateDirectory(dest);
        var patch = Patch.Load();
        var patched = new HashSet<string>(patch.Select(p => p.Target), StringComparer.OrdinalIgnoreCase);

        // 1. the game folder (the files the update replaces are written by step 2)
        var files = src.GameFiles().Where(f => !patched.Contains(f)).ToList();
        Log($"Copying {files.Count} game files...");
        for (int i = 0; i < files.Count; i++) {
            ct.ThrowIfCancellationRequested();
            var outp = Path.Combine(dest, files[i].Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(outp));
            File.WriteAllBytes(outp, src.ReadGameFile(files[i]));
            if (i % 25 == 0) Progress(i * 30 / files.Count, "Copying game files");
        }

        // 2. the v1.20E Direct3D update
        Log("Applying the v1.20E update...");
        var gameFiles = src.GameFiles().ToDictionary(f => f, f => f, StringComparer.OrdinalIgnoreCase);
        foreach (var e in patch) {
            ct.ThrowIfCancellationRequested();
            byte[] input = Array.Empty<byte>();
            if (e.Source.Length > 0) {
                string rel = e.Source.StartsWith("GAME/", StringComparison.OrdinalIgnoreCase) ? e.Source[5..] : e.Source;
                if (!gameFiles.TryGetValue(rel, out var real)) throw new FileNotFoundException("Missing on the disc: " + e.Source);
                input = src.ReadGameFile(real);
            }
            var data = e.Apply(input);
            var outp = Path.Combine(dest, e.Target.Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(Path.GetDirectoryName(outp));
            // a differently-cased copy from step 1 would shadow it on a case-insensitive card
            foreach (var old in Directory.GetFiles(Path.GetDirectoryName(outp)))
                if (string.Equals(Path.GetFileName(old), Path.GetFileName(outp), StringComparison.OrdinalIgnoreCase)) File.Delete(old);
            File.WriteAllBytes(outp, data);
            Log($"  {e.Target}: ok (md5 {Md5Hex(data)})");
        }
        Progress(35, "Update applied");

        // 3. the CD music: tracks 2.. as CDDA_BGM/NN.mp3 (runtime/w32_mm.c)
        var tracks = src.Audio.Where(t => t.Number >= 2).OrderBy(t => t.Number).ToList();
        if (tracks.Count == 0) {
            Log("WARNING: no CD audio tracks - the game will have no music. Use a BIN/CUE or the CD itself.");
        } else {
            string bgm = Path.Combine(dest, "CDDA_BGM");
            Directory.CreateDirectory(bgm);
            Log($"Encoding {tracks.Count} music tracks to MP3 ({Bitrate} kbps)...");
            int done = 0;
            var opts = new ParallelOptions { CancellationToken = ct, MaxDegreeOfParallelism = Math.Max(1, Environment.ProcessorCount - 1) };
            Parallel.ForEach(tracks, opts, t => {
                EncodeTrack(t, Path.Combine(bgm, $"{t.Number:00}.mp3"), ct);
                int n = Interlocked.Increment(ref done);
                Log($"  track {t.Number:00}: {t.Sectors / 75 / 60}:{t.Sectors / 75 % 60:00}");
                Progress(35 + n * 65 / tracks.Count, $"Encoding music ({n}/{tracks.Count})");
            });
            if (tracks.Count < 23) Log($"WARNING: the original CD has 23 music tracks, this source has {tracks.Count}.");
        }
        File.WriteAllText(Path.Combine(outRoot, "README.txt"),
            "Copy the \"data\" folder to the root of ux0: on your PS Vita (it merges with the\r\n" +
            "existing ux0:data folder). The game files must end up in ux0:data/dayusadx/.\r\n");
        Progress(100, "Done");
    }

    void EncodeTrack(AudioTrack t, string path, CancellationToken ct)
    {
        var tmp = path + ".part";
        using (var fs = File.Create(tmp))
        using (var w = new LameMP3FileWriter(fs, new WaveFormat(44100, 16, 2), Bitrate)) {
            var buf = new byte[2352 * 64];
            for (long s = 0; s < t.Sectors; ) {
                ct.ThrowIfCancellationRequested();
                int n = t.Read(s, 64, buf);
                if (n <= 0) break;
                w.Write(buf, 0, n * 2352);
                s += n;
            }
        }
        File.Move(tmp, path, true);
    }
}
