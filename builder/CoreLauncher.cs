using System;
using System.Diagnostics;
using System.IO;
using System.Text;

static class CoreLauncher
{
    static string Quote(string value)
    {
        StringBuilder result = new StringBuilder("\"");
        int slashes = 0;
        foreach (char c in value)
        {
            if (c == '\\') { ++slashes; continue; }
            result.Append('\\', c == '"' ? slashes * 2 + 1 : slashes);
            slashes = 0;
            result.Append(c);
        }
        result.Append('\\', slashes * 2);
        return result.Append('"').ToString();
    }

    static int Main(string[] args)
    {
        Console.OutputEncoding = new UTF8Encoding(false);
        string root = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "_internal");
        string python = Path.Combine(root, "python", "python.exe");
        string backend = Path.Combine(root, "builder_backend.py");
        if (!File.Exists(python) || !File.Exists(backend))
        {
            Console.Error.WriteLine("Extract the entire tool ZIP; bundled runtime files are missing.");
            return 2;
        }
        StringBuilder command = new StringBuilder("-B -X utf8 " + Quote(backend));
        foreach (string arg in args) command.Append(" " + Quote(arg));
        try
        {
            ProcessStartInfo info = new ProcessStartInfo {
                FileName = python, Arguments = command.ToString(),
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
            };
            info.EnvironmentVariables.Remove("MMMERGE_FFMPEG");
            info.EnvironmentVariables.Remove("PYTHONPATH");
            info.EnvironmentVariables.Remove("PYTHONHOME");
            using (Process child = new Process { StartInfo = info })
            {
                child.OutputDataReceived += delegate(object s, DataReceivedEventArgs e) { if (e.Data != null) Console.Out.WriteLine(e.Data); };
                child.ErrorDataReceived += delegate(object s, DataReceivedEventArgs e) { if (e.Data != null) Console.Error.WriteLine(e.Data); };
                child.Start();
                child.BeginOutputReadLine();
                child.BeginErrorReadLine();
                child.WaitForExit();
                return child.ExitCode;
            }
        }
        catch (Exception error) { Console.Error.WriteLine(error.Message); return 2; }
    }
}
