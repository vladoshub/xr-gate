using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Text;

namespace XrGateWsl
{
    public sealed class NativeOutput
    {
        public string Text { get; private set; }
        public bool IsError { get; private set; }
        public NativeOutput(string text, bool isError) { Text = text; IsError = isError; }
    }

    // Compatible with Add-Type in Windows PowerShell 5.1. Event handlers only
    // enqueue data; PowerShell drains it on its own runspace and keeps logging.
    public sealed class NativeCommand : IDisposable
    {
        private readonly Process process;
        private readonly ConcurrentQueue<NativeOutput> output = new ConcurrentQueue<NativeOutput>();
        private volatile bool stdoutEnded;
        private volatile bool stderrEnded;

        public NativeCommand(string file, string[] arguments)
        {
            var commandLine = new StringBuilder();
            foreach (string argument in arguments)
            {
                if (commandLine.Length > 0) commandLine.Append(' ');
                commandLine.Append(QuoteArgument(argument));
            }
            process = new Process();
            process.StartInfo = new ProcessStartInfo(file, commandLine.ToString());
            process.StartInfo.UseShellExecute = false;
            process.StartInfo.CreateNoWindow = true;
            process.StartInfo.RedirectStandardOutput = true;
            process.StartInfo.RedirectStandardError = true;
            process.StartInfo.StandardOutputEncoding = Encoding.UTF8;
            process.StartInfo.StandardErrorEncoding = Encoding.UTF8;
            process.OutputDataReceived += delegate(object sender, DataReceivedEventArgs e)
            {
                if (e.Data == null) stdoutEnded = true;
                else output.Enqueue(new NativeOutput(e.Data, false));
            };
            process.ErrorDataReceived += delegate(object sender, DataReceivedEventArgs e)
            {
                if (e.Data == null) stderrEnded = true;
                else output.Enqueue(new NativeOutput(e.Data, true));
            };
            try
            {
                if (!process.Start()) throw new InvalidOperationException("Could not start " + file);
                process.BeginOutputReadLine();
                process.BeginErrorReadLine();
            }
            catch { Dispose(); throw; }
        }

        public bool Finished { get { return process.HasExited && stdoutEnded && stderrEnded && output.IsEmpty; } }
        public int ExitCode { get { return process.ExitCode; } }

        public NativeOutput ReadOutput()
        {
            NativeOutput line;
            return output.TryDequeue(out line) ? line : null;
        }

        public void Dispose()
        {
            try
            {
                if (!process.HasExited)
                {
                    // Framework 4.x (Windows PowerShell 5.1) has no Kill(tree).
                    // Scope taskkill to this exact native command's PID; never
                    // kill usbipd by image name or stop the WSL distribution.
                    if (Environment.OSVersion.Platform == PlatformID.Win32NT)
                    {
                        var stopInfo = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "taskkill.exe"),
                            "/PID " + process.Id + " /T /F");
                        stopInfo.UseShellExecute = false;
                        stopInfo.CreateNoWindow = true;
                        stopInfo.RedirectStandardOutput = true;
                        stopInfo.RedirectStandardError = true;
                        using (var stop = Process.Start(stopInfo))
                        {
                            stop.BeginOutputReadLine();
                            stop.BeginErrorReadLine();
                            if (!stop.WaitForExit(2000)) stop.Kill();
                        }
                    }
                    if (!process.HasExited) process.Kill();
                    process.WaitForExit(2000);
                }
            }
            catch (InvalidOperationException) { }
            finally { process.Dispose(); }
        }

        // Quote only when needed. WSL parses its own options from the raw
        // command line: quoted --distribution/--user/--exec are not options.
        // Protect whitespace, literal quotes and trailing backslashes without a shell.
        public static string QuoteArgument(string value)
        {
            if (value == null) throw new ArgumentNullException("value");
            if (value.Length > 0 && value.IndexOfAny(new[] { ' ', '\t', '\r', '\n', '\v', '"' }) < 0) return value;
            var quoted = new StringBuilder("\"");
            int backslashes = 0;
            foreach (char c in value)
            {
                if (c == '\\') { ++backslashes; continue; }
                if (c == '"') quoted.Append('\\', backslashes * 2 + 1);
                else quoted.Append('\\', backslashes);
                backslashes = 0;
                quoted.Append(c);
            }
            quoted.Append('\\', backslashes * 2);
            return quoted.Append('"').ToString();
        }
    }
}
