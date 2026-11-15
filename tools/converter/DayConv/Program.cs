namespace DayConv;

static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        // DayConv.exe --cli <input> <output>: no window (testing / scripts)
        if (args.Length == 3 && args[0] == "--cli") {
            var conv = new Converter { Log = Console.WriteLine };
            using var src = conv.OpenAndCheck(args[1], out _);
            if (src == null) return 2;
            conv.Run(src, args[2], CancellationToken.None);
            return 0;
        }
        ApplicationConfiguration.Initialize();
        Application.Run(new MainForm());
        return 0;
    }
}
