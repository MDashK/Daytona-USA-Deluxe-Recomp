// MainForm.cs - the converter's window.
using System.Drawing;
using System.Reflection;

namespace DayConv;

public sealed class MainForm : Form
{
    readonly TextBox _input = new() { Anchor = AnchorStyles.Left | AnchorStyles.Right | AnchorStyles.Top };
    readonly TextBox _output = new() { Anchor = AnchorStyles.Left | AnchorStyles.Right | AnchorStyles.Top };
    readonly ComboBox _quality = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 110 };
    readonly Button _browseIn = new() { Text = "Browse...", Anchor = AnchorStyles.Right | AnchorStyles.Top };
    readonly Button _browseOut = new() { Text = "Browse...", Anchor = AnchorStyles.Right | AnchorStyles.Top };
    readonly Button _start = new() { Text = "Convert", Width = 120, Height = 32, Anchor = AnchorStyles.Right | AnchorStyles.Bottom };
    readonly Label _check = new() { AutoSize = true, Text = "", Anchor = AnchorStyles.Left | AnchorStyles.Top };
    readonly ProgressBar _bar = new() { Anchor = AnchorStyles.Left | AnchorStyles.Right | AnchorStyles.Bottom, Height = 22 };
    readonly Label _step = new() { AutoSize = true, Anchor = AnchorStyles.Left | AnchorStyles.Bottom };
    readonly TextBox _log = new() {
        Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, WordWrap = false,
        Anchor = AnchorStyles.Left | AnchorStyles.Right | AnchorStyles.Top | AnchorStyles.Bottom,
        Font = new Font(FontFamily.GenericMonospace, 8.5f), BackColor = SystemColors.Window };
    CancellationTokenSource _cts;

    public MainForm()
    {
        Text = "Daytona USA Deluxe - PS Vita data converter";
        ClientSize = new Size(720, 520);
        MinimumSize = new Size(600, 440);
        StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI", 9f);
        using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("app.ico"))
            if (s != null) Icon = new Icon(s);

        var intro = new Label {
            Text = "Makes the game folder for the PS Vita port from your original Daytona USA Deluxe CD.\n" +
                   "Choose the CD image (.cue / .bin / .iso) or the game's exe on the CD (GAME\\DAYTONA USA Deluxe.exe).\n" +
                   "The files are checked, updated to v1.20E (Direct3D) and the CD music is converted to MP3.",
            Location = new Point(12, 10), Size = new Size(696, 54), Anchor = AnchorStyles.Left | AnchorStyles.Right | AnchorStyles.Top };
        var l1 = new Label { Text = "Game CD / image:", Location = new Point(12, 76), AutoSize = true };
        _input.SetBounds(130, 72, 480, 24);
        _browseIn.SetBounds(618, 71, 90, 26);
        _check.Location = new Point(130, 100);
        var l2 = new Label { Text = "Output folder:", Location = new Point(12, 128), AutoSize = true };
        _output.SetBounds(130, 124, 480, 24);
        _browseOut.SetBounds(618, 123, 90, 26);
        var l3 = new Label { Text = "Music quality:", Location = new Point(12, 162), AutoSize = true };
        _quality.Location = new Point(130, 158);
        _quality.Items.AddRange(new object[] { "128 kbps", "160 kbps", "192 kbps", "256 kbps", "320 kbps" });
        _quality.SelectedIndex = 2;
        _log.SetBounds(12, 192, 696, 230);
        _step.Location = new Point(12, 432);
        _bar.SetBounds(12, 452, 696, 22);
        _start.Location = new Point(588, 480);
        _output.Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Desktop), "DaytonaUSA_Vita");

        Controls.AddRange(new Control[] { intro, l1, _input, _browseIn, _check, l2, _output, _browseOut, l3, _quality, _log, _step, _bar, _start });

        _browseIn.Click += (_, _) => {
            using var d = new OpenFileDialog {
                Title = "Choose the game CD image or the game's exe",
                Filter = "CD image or game exe (*.cue;*.bin;*.img;*.iso;*.exe)|*.cue;*.bin;*.img;*.iso;*.exe|All files (*.*)|*.*" };
            if (d.ShowDialog(this) == DialogResult.OK) { _input.Text = d.FileName; Check(); }
        };
        _browseOut.Click += (_, _) => {
            using var d = new FolderBrowserDialog { Description = "Where to put the converted files", UseDescriptionForTitle = true };
            if (d.ShowDialog(this) == DialogResult.OK) _output.Text = d.SelectedPath;
        };
        _input.Leave += (_, _) => { if (_input.Text.Length > 0) Check(); };
        _start.Click += async (_, _) => await StartAsync();
        AllowDrop = true;
        DragEnter += (_, e) => { if (e.Data.GetDataPresent(DataFormats.FileDrop)) e.Effect = DragDropEffects.Copy; };
        DragDrop += (_, e) => { var f = (string[])e.Data.GetData(DataFormats.FileDrop); if (f?.Length > 0) { _input.Text = f[0]; Check(); } };
        FormClosing += (_, e) => {
            if (_cts != null && MessageBox.Show(this, "Stop the conversion?", Text, MessageBoxButtons.YesNo) == DialogResult.No) e.Cancel = true;
            else _cts?.Cancel();
        };
    }

    void Log(string s)
    {
        if (InvokeRequired) { BeginInvoke(() => Log(s)); return; }
        _log.AppendText(s + Environment.NewLine);
    }

    void SetCheck(bool? ok, string text)
    {
        _check.Text = text;
        _check.ForeColor = ok == true ? Color.DarkGreen : ok == false ? Color.Firebrick : SystemColors.ControlText;
    }

    /// <summary>Checks the chosen source's exe at once (on a worker thread).</summary>
    async void Check()
    {
        string path = _input.Text.Trim('"', ' ');
        if (!File.Exists(path)) { SetCheck(false, "File not found."); return; }
        SetCheck(null, "Checking...");
        try {
            var conv = new Converter { Log = Log };
            var (ok, md5) = await Task.Run(() => {
                using var src = conv.OpenAndCheck(path, out var m);
                return (src != null, m);
            });
            SetCheck(ok, ok ? "Original CD exe found (md5 " + md5 + ")" : "Not the original CD exe" + (md5 != null ? " (md5 " + md5 + ")" : ""));
        } catch (Exception e) {
            SetCheck(false, e.Message);
        }
    }

    async Task StartAsync()
    {
        if (_cts != null) { _cts.Cancel(); return; }
        string input = _input.Text.Trim('"', ' '), output = _output.Text.Trim('"', ' ');
        if (!File.Exists(input)) { MessageBox.Show(this, "Choose the game CD image or exe first.", Text); return; }
        if (output.Length == 0) { MessageBox.Show(this, "Choose an output folder.", Text); return; }
        string dest = Path.Combine(output, "data", "dayusadx");
        if (Directory.Exists(dest) && Directory.EnumerateFileSystemEntries(dest).Any() &&
            MessageBox.Show(this, dest + " already has files. Replace them?", Text, MessageBoxButtons.YesNo) != DialogResult.Yes) return;

        var conv = new Converter {
            Log = Log,
            Bitrate = int.Parse(((string)_quality.SelectedItem).Split(' ')[0]),
            Progress = (p, s) => BeginInvoke(() => { _bar.Value = Math.Clamp(p, 0, 100); _step.Text = s; }),
        };
        _cts = new CancellationTokenSource();
        _start.Text = "Stop";
        foreach (var c in new Control[] { _input, _output, _browseIn, _browseOut, _quality }) c.Enabled = false;
        _log.Clear();
        var t0 = DateTime.Now;
        try {
            await Task.Run(() => {
                using var src = conv.OpenAndCheck(input, out _) ?? throw new InvalidDataException(
                    "This is not the exe of the original Daytona USA Deluxe CD (md5 " + Converter.CdExeMd5 + ").");
                Log("Source: " + src.Description);
                conv.Run(src, output, _cts.Token);
            });
            Log($"Finished in {(DateTime.Now - t0).TotalSeconds:0} s.");
            MessageBox.Show(this, "Done.\n\nCopy the \"data\" folder from\n" + output +
                            "\nto ux0: on your PS Vita (the game files go in ux0:data/dayusadx/).", Text,
                            MessageBoxButtons.OK, MessageBoxIcon.Information);
        } catch (OperationCanceledException) {
            Log("Stopped.");
        } catch (Exception e) {
            Log("ERROR: " + e.Message);
            MessageBox.Show(this, e.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error);
        } finally {
            _cts = null;
            _start.Text = "Convert";
            foreach (var c in new Control[] { _input, _output, _browseIn, _browseOut, _quality }) c.Enabled = true;
        }
    }
}
